/*
 * Cix dashboard: a pure client of docs/api/openapi.yaml, same as
 * cixctl (cli/src/main.c) -- no capability here that isn't already
 * one of the REST endpoints. Vanilla JS, no framework, no build step
 * (docs/adr/0010). Left resource tree + per-resource detail views,
 * routed via location.hash (#category or #category/name) -- no router
 * library, matching this project's own hand-rolled-over-dependency
 * posture everywhere else.
 */
"use strict";

/*
 * localStorage THROWS -- it does not merely return null.
 *
 * Firefox raises NS_ERROR_FAILURE on any access when site data is
 * blocked for the origin (cookie blocking, strict mode, some private
 * windows); Chrome throws SecurityError in the same situation, and both
 * throw on a quota failure. Reported from a real browser on
 * 192.168.15.95:
 *
 *   Uncaught NS_ERROR_FAILURE  app.js:639:30
 *
 * which was `let authToken = storageGet("cix-auth-token")` at
 * module scope. An uncaught throw there stops the whole script before
 * anything initialises, so the page renders its static HTML and NOTHING
 * loads -- a dead dashboard that looks like a data problem rather than
 * a storage one.
 *
 * Ten other uses in this file each hand-rolled their own try/catch and
 * were fine; the auth pair was written without one and took the page
 * down. So the guard stops being a thing each call site remembers and
 * becomes the only way this file touches storage. Every accessor
 * degrades to "no stored value" and the dashboard works, minus the
 * conveniences that persistence buys.
 */
function storageGet(key) {
	try {
		return storageGet(key);
	} catch (e) {
		return null;
	}
}

function storageSet(key, value) {
	try {
		storageSet(key, value);
		return true;
	} catch (e) {
		return false;
	}
}

function storageRemove(key) {
	try {
		storageRemove(key);
	} catch (e) {
		/* Nothing stored means nothing to remove. */
	}
}

const POLL_INTERVAL_MS = 2000;

/* Latest polled data, one source of truth the tree, the summary
 * tables, and the detail views all read from -- one fetch per
 * resource per poll cycle, not a second fetch path per view. */
const cache = {
	containers: [],
	networks: [],
	routes: [],
	images: [],
	devices: [],
	deviceMaps: [],
	storage: [],
	/* Cached so the Storage tree can hang each volume under whichever
	 * device actually holds it, without every tree render refetching. */
	volumes: [],
	storageRoles: [],
	diskFormatStatus: {},
	stateStorage: { disk: null },
	stateStorageMigrate: { state: "none" },
	logStorage: { disk: null },
	logStorageMigrate: { state: "none" },
	rebuildableStorage: { disk: null },
	rebuildableStorageMigrate: { state: "none" },
	backupConfig: { disk: null, enabled: false },
	backupStatus: { state: "never" },
	dnsRecords: [],
	dnsServers: [],
	ldapServers: [],
	ldapGroups: [],
	ldapUsers: [],
	ldapConfig: null,
	ntpConfig: null,
	ntpServers: [],
	syslogTargets: [],
	ntpStatus: null,
	ntpTime: null,
	pkiCa: null,
	pkiIntermediate: null,
	pkiCerts: [],
	pkgRecipes: [],
	pkgList: [],
	imageRecipes: [],
	containerRecipes: [],
	pkgRepoConfig: null,
	pkgSyncStatus: null,
	pkgCacheConfig: null,
	pkgCacheStatus: null,
	pkgArtifactConfig: null,
	siteConfig: null,
	daemonConfig: null,
	swap: null,
	sysctls: [],
	kmodModules: [],
	kmodConfig: [],
};

const statusLeds = document.getElementById("status-leds");
const ledTx = document.getElementById("led-tx");
const ledRx = document.getElementById("led-rx");
const statusMeta = document.getElementById("status-meta");
const statusBox = document.getElementById("status");
const treeEl = document.getElementById("tree");
const logOutput = document.getElementById("log-output");
const logPanel = document.getElementById("log-panel");
const logPanelHeader = document.getElementById("log-panel-header");
const logPanelArrow = document.getElementById("log-panel-arrow");
const logPanelSource = document.getElementById("log-panel-source");

const LOG_COLLAPSE_KEY = "cix-log-collapsed";
const LOG_HEIGHT_KEY = "cix-log-height";

/* Collapsing/expanding must also manage #log-panel's own inline
 * height -- makeResizable() below (log-panel-resize-handle) sets
 * that directly as a plain element style, which (being inline) always
 * outranks the .collapsed class's own `flex: 0 0 auto` rule regardless
 * of stylesheet specificity. Without this, the class toggles and
 * #log-output correctly hides, but the panel's own outer height stays
 * pinned to whatever it was last resized to -- confirmed live, exactly
 * the reported bug ("it no longer collapses..., it just hides the log
 * entries and the pane stays where it is"). Clearing the inline style
 * on collapse lets .collapsed's own rule govern; restoring the saved
 * height on expand brings back the user's own last chosen size instead
 * of silently forgetting it. */
function setLogCollapsed(collapsed) {
	logPanel.classList.toggle("collapsed", collapsed);
	logPanelArrow.textContent = collapsed ? "▸" : "▾";
	if (collapsed) {
		logPanel.style.height = "";
	} else {
		try {
			const saved = storageGet(LOG_HEIGHT_KEY);

			if (saved !== null) logPanel.style.height = saved + "px";
		} catch (e) {
			/* localStorage unavailable -- the base 200px height on
			 * #log-panel stands. */
		}
	}
	try {
		storageSet(LOG_COLLAPSE_KEY, collapsed ? "1" : "0");
	} catch (e) {
		/* localStorage unavailable -- state just won't survive a reload. */
	}
}

logPanelHeader.addEventListener("click", () => {
	setLogCollapsed(!logPanel.classList.contains("collapsed"));
});

/* Prevent a click/interaction on the source filter from also toggling
 * collapse via the header's own click handler above (event bubbling --
 * the select lives inside .log-panel-header for layout purposes only). */
logPanelSource.addEventListener("click", (event) => {
	event.stopPropagation();
});

try {
	setLogCollapsed(storageGet(LOG_COLLAPSE_KEY) === "1");
} catch (e) {
	/* localStorage unavailable -- default expanded. */
}

/*
 * ---------- resizable tree/log panels ----------
 *
 * Shared drag-to-resize helper, one instance per handle -- axis "x"
 * resizes width (the tree, via .layout's own grid-template-columns
 * first track), axis "y" resizes height (the log panel, via its own
 * flex-basis). invert accounts for which edge of the resized element
 * the handle sits on: the tree's handle is on its right edge (drag
 * right = grow, no inversion needed), the log panel's handle is on
 * its top edge but the panel itself is anchored to the page's bottom
 * (drag down = shrink, the delta has to flip). Persists to
 * localStorage (cix-tree-width/cix-log-height) and restores on
 * load -- same convention as theme/tree-collapse/log-collapse above.
 */
function makeResizable(handle, opts) {
	const { axis, invert, storageKey, min, max, getSize, apply } = opts;
	let dragging = false;
	let startPos = 0;
	let startSize = 0;
	let lastSize = getSize();

	function clamp(size) {
		return Math.min(max, Math.max(min, size));
	}

	function onMove(event) {
		if (!dragging)
			return;
		const pos = axis === "x" ? event.clientX : event.clientY;
		const delta = pos - startPos;

		lastSize = clamp(startSize + (invert ? -delta : delta));
		apply(lastSize);
	}

	function onUp() {
		if (!dragging)
			return;
		dragging = false;
		handle.classList.remove("dragging");
		document.body.style.cursor = "";
		document.body.style.userSelect = "";
		document.removeEventListener("mousemove", onMove);
		document.removeEventListener("mouseup", onUp);
		try {
			storageSet(storageKey, String(lastSize));
		} catch (e) {
			/* localStorage unavailable -- won't survive a reload. */
		}
	}

	handle.addEventListener("mousedown", (event) => {
		event.preventDefault();
		dragging = true;
		startPos = axis === "x" ? event.clientX : event.clientY;
		startSize = lastSize;
		handle.classList.add("dragging");
		document.body.style.cursor = axis === "x" ? "col-resize" : "row-resize";
		document.body.style.userSelect = "none";
		document.addEventListener("mousemove", onMove);
		document.addEventListener("mouseup", onUp);
	});

	try {
		const saved = storageGet(storageKey);

		/* skipInitialApplyIf(): for the log panel specifically, skip this
		 * initial apply() when the panel is currently collapsed --
		 * setLogCollapsed() (which already ran, earlier in load order)
		 * deliberately left the inline flex-basis cleared so the
		 * .collapsed CSS class governs the height; applying a saved
		 * height here would silently reintroduce the exact bug
		 * setLogCollapsed()'s own fix closes. lastSize is still updated
		 * so a later expand (via setLogCollapsed() reading LOG_HEIGHT_KEY
		 * directly) and any subsequent drag both still use the real
		 * saved value, not a stale default. */
		if (saved !== null) {
			lastSize = clamp(parseInt(saved, 10));
			if (!opts.skipInitialApplyIf || !opts.skipInitialApplyIf()) apply(lastSize);
		}
	} catch (e) {
		/* localStorage unavailable -- default size stands. */
	}
}

makeResizable(document.getElementById("tree-resize-handle"), {
	axis: "x",
	storageKey: "cix-tree-width",
	min: 160,
	max: 480,
	getSize: () => document.querySelector(".tree-panel").getBoundingClientRect().width,
	/* Publishes the width as a custom property rather than setting
	 * .layout's grid-template-columns directly -- one named place for
	 * "how wide is the tree", which anything else that needs to line up
	 * with that column can read. */
	apply: (size) => {
		document.documentElement.style.setProperty("--tree-width", size + "px");
	},
});

makeResizable(document.getElementById("log-panel-resize-handle"), {
	axis: "y",
	invert: true,
	storageKey: LOG_HEIGHT_KEY,
	min: 80,
	max: Math.round(window.innerHeight * 0.7),
	getSize: () => logPanel.getBoundingClientRect().height,
	apply: (size) => {
		logPanel.style.height = size + "px";
	},
	skipInitialApplyIf: () => logPanel.classList.contains("collapsed"),
});

/* ---------- the phone-only tree disclosure (#233) ---------- */

/*
 * On a narrow screen the tree is hidden and this button reveals it.
 * The state is a class on <body> rather than an inline style, so the
 * media query stays the single place that decides the tree is hidden
 * at all -- at desktop width the class is inert and the tree is a
 * permanent column regardless of what was last tapped on a phone.
 *
 * Nothing here restores state across loads. The tree is a detour on a
 * phone, not a mode: the useful default every time the page opens is
 * the content that was asked for, so it closes again on navigation
 * below rather than persisting.
 */
const treeDisclosure = document.getElementById("tree-disclosure");

function setTreeOpen(open) {
	document.body.classList.toggle("tree-open", open);
	if (treeDisclosure !== null)
		treeDisclosure.setAttribute("aria-expanded", open ? "true" : "false");
}

if (treeDisclosure !== null) {
	treeDisclosure.addEventListener("click", () => {
		setTreeOpen(!document.body.classList.contains("tree-open"));
	});
	/*
	 * Picking something in the tree is a navigation, and leaving the
	 * tree covering the page afterwards would hide the very thing that
	 * was just asked for. Delegated from the tree container so it keeps
	 * working across the re-renders that replace its contents.
	 */
	const treeNav = document.getElementById("tree");

	if (treeNav !== null) {
		treeNav.addEventListener("click", (e) => {
			/*
			 * Only a real navigation closes the panel. a.tree-item is
			 * what the tree's own click handling already treats as the
			 * navigable element; .tree-category and the expand/collapse
			 * chevrons open a branch instead, and closing the panel on
			 * either would make a branch impossible to open on a phone.
			 */
			if (e.target.closest("a.tree-item") !== null)
				setTreeOpen(false);
		});
	}
}

/* ---------- theme toggle (light / dark / auto) ---------- */

const THEME_KEY = "cix-theme";
const themeToggle = document.getElementById("theme-toggle");

/* "auto" (no stored value, or an unrecognized one) means "follow
 * prefers-color-scheme" -- represented as no data-theme attribute at
 * all, never a third CSS variable set, so the browser/OS preference
 * (style.css's own @media block) is always the real fallback, not a
 * value this code would ever need to duplicate or get out of sync
 * with. index.html's own inline bootstrap script already applied
 * whatever was saved before first paint -- this only needs to pick
 * the label back up and wire the click handler. */
function currentTheme() {
	const attr = document.documentElement.getAttribute("data-theme");

	return attr === "light" || attr === "dark" ? attr : "auto";
}

/*
 * The theme toggle's three states, drawn here rather than taken from the
 * brand set (#232): that set has no sun, moon or display glyph -- it is a
 * platform-resource vocabulary (container, disk, cgroup, certificate),
 * and a light/dark control is not one of its concepts.
 *
 * Drawn to the SHIPPED geometry even so -- 24x24 viewBox, 1.75 stroke,
 * currentColor -- so they sit at the same visual weight as the real
 * icons beside them. The previous versions were 16x16/1.3 and matched
 * the tree icons only while those were also hand-drawn.
 */
const THEME_ICONS = {
	light:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="4.2"/><path d="M12 2v2.4M12 19.6V22M2 12h2.4M19.6 12H22M4.9 4.9l1.7 1.7M17.4 17.4l1.7 1.7M4.9 19.1l1.7-1.7M17.4 6.6l1.7-1.7"/></svg>',
	dark:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M19.5 13.2A7.8 7.8 0 1 1 10.8 4.5a6.1 6.1 0 0 0 8.7 8.7Z"/></svg>',
	auto:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="2.8" y="4" width="18.4" height="12" rx="1.6"/><path d="M8.5 20h7M12 16v4"/></svg>',
};

function applyTheme(theme) {
	if (theme === "light" || theme === "dark")
		document.documentElement.setAttribute("data-theme", theme);
	else
		document.documentElement.removeAttribute("data-theme");
	themeToggle.innerHTML = THEME_ICONS[theme];
	themeToggle.title = "Theme: " + (theme === "dark" ? "Dark" : theme === "light" ? "Light" : "Auto") + " (click to cycle)";
	try {
		storageSet(THEME_KEY, theme);
	} catch (e) {
		/* localStorage unavailable -- choice won't survive a reload,
		 * same posture as every other localStorage-backed preference
		 * here (tree collapse, log panel collapse). */
	}
}

themeToggle.addEventListener("click", () => {
	const next = { auto: "light", light: "dark", dark: "auto" }[currentTheme()];

	applyTheme(next);
});

applyTheme(currentTheme());

/* ---------- create modal (header "+ Create" dropdown) ----------
 *
 * One shared modal shell; each create form already lives in the page
 * (moved here from its old inline spot in each category view) inside
 * its own hidden "modal-<form-id>" wrapper -- opening the modal just
 * un-hides the one wrapper that matches, rather than building/tearing
 * down form markup dynamically. */
const modalOverlay = document.getElementById("modal-overlay");
const modalTitle = document.getElementById("modal-title");

function closeModal() {
	modalOverlay.hidden = true;
	for (const panel of modalOverlay.querySelectorAll(".modal-body > div"))
		panel.hidden = true;

	/* Edit-mode state (tasks #749/#750) is scoped to a single modal
	 * session -- always reset back to "create" on close, regardless of
	 * how it closed (submit, X button, Escape, outside click). */
	dnsRecordEditName = null;
	document.getElementById("df-name").readOnly = false;
	document.getElementById("df-submit").textContent = "Create";
	ldapGroupEditName = null;
	document.getElementById("lgf-name").readOnly = false;
	document.getElementById("lgf-submit").textContent = "Create";
	ldapUserEditName = null;
	document.getElementById("luf-name").readOnly = false;
	document.getElementById("luf-submit").textContent = "Create";
	document.getElementById("irf-name").readOnly = false;

	/* Never leave a typed password sitting in the DOM past this modal
	 * session, whether closed by submit, X, Escape, or an outside click. */
	document.getElementById("lf-password").value = "";
}

function openModal(formId, title) {
	closeModal();
	modalTitle.textContent = title;
	document.getElementById("modal-" + formId).hidden = false;
	modalOverlay.hidden = false;
}

document.getElementById("signing-keys-form").addEventListener("submit", (event) => {
	event.preventDefault();
	installSigningKeys();
});
document.getElementById("signing-keys-clear").addEventListener("click", clearSigningKeys);
document.getElementById("release-key-form").addEventListener("submit", (event) => {
	event.preventDefault();
	installReleaseKey();
});
document.getElementById("release-key-clear").addEventListener("click", clearReleaseKey);
document.getElementById("images-gc-preview").addEventListener("click", () => runImageGc(true));
document.getElementById("images-gc").addEventListener("click", () => runImageGc(false));
document.getElementById("modal-close").addEventListener("click", closeModal);
modalOverlay.addEventListener("click", (event) => {
	if (event.target === modalOverlay)
		closeModal();
});
document.addEventListener("keydown", (event) => {
	if (event.key === "Escape" && !modalOverlay.hidden)
		closeModal();
});

/*
 * ---------- header menu bar ----------
 *
 * One generalized handler for every .menu-dropdown in the header
 * (logo/Create/Software/User-Group/Network Services/Hardware/Host/
 * Monitoring) instead of one hardcoded pair -- a click on a toggle
 * closes every other open menu and toggles this one; a click on any
 * link or button inside a menu closes it (event delegation on the
 * menu itself, so it covers plain nav <a>s, data-modal create
 * actions, and the logo menu's own special-purpose buttons -- reboot/
 * shutdown/login -- with no per-item wiring needed here).
 */
/* The one open top-level panel, as [toggle, menu], or null. Held
 * because a fixed panel has to be re-placed whenever its toggle
 * moves, and only this knows which panel that is. */
let openMenu = null;

function closeAllMenus() {
	openMenu = null;
	for (const menu of document.querySelectorAll(".menu-dropdown-menu, .menu-submenu-menu"))
		menu.hidden = true;
	for (const t of document.querySelectorAll(".menu-submenu-toggle"))
		t.setAttribute("aria-expanded", "false");
}

/*
 * A top-level panel is position: fixed (see style.css for why -- the
 * menu bar scrolls sideways, which makes it clip anything absolute
 * inside it), so it has no automatic relationship to its toggle at
 * all and every coordinate has to be measured here. Measured after
 * the panel is visible, because a hidden element has no box: left
 * edge under the toggle's left edge, top edge just under the bar,
 * flipped to hang off the toggle's RIGHT edge when a left-aligned
 * panel would run past the window -- the same rule the submenus
 * already use one level down, so both levels behave alike.
 */
function positionMenu(toggle, menu) {
	const t = toggle.getBoundingClientRect();

	/*
	 * Line the panel's ENTRIES up with the trigger's own label, not
	 * its box with the trigger's box. Both carry horizontal padding
	 * and the two are not equal, so a box-to-box alignment leaves the
	 * words a few pixels apart -- close enough to look like a mistake
	 * rather than a margin. Both paddings are read from the live
	 * computed style, so this stays true if either changes in CSS.
	 */
	const item = menu.querySelector("a, button");
	const pad = parseFloat(getComputedStyle(toggle).paddingLeft) -
	            (item !== null ? parseFloat(getComputedStyle(item).paddingLeft) : 0);

	menu.style.left = Math.max(8, t.left + pad) + "px";
	menu.style.top = t.bottom + 5 + "px";

	/*
	 * Flipped to hang off the trigger's RIGHT edge when a left-aligned
	 * panel would run past the window. `pad` is ADDED back here rather
	 * than subtracted again: on this edge it is the panel's own right
	 * inset that has to line up, and subtracting a negative pad pushed
	 * the panel out past the trigger by those same few pixels.
	 */
	const m = menu.getBoundingClientRect();
	if (m.right > window.innerWidth - 8)
		menu.style.left = Math.max(8, t.right - m.width + pad) + "px";
}

for (const dropdown of document.querySelectorAll(".menu-dropdown")) {
	const toggle = dropdown.querySelector(".menu-dropdown-toggle");
	const menu = dropdown.querySelector(".menu-dropdown-menu");

	toggle.addEventListener("click", (event) => {
		event.stopPropagation();
		const shouldOpen = menu.hidden;
		closeAllMenus();
		menu.hidden = !shouldOpen;
		if (shouldOpen) {
			positionMenu(toggle, menu);
			openMenu = [toggle, menu];
		}
	});
	menu.addEventListener("click", (event) => {
		/* A submenu's own toggle is the one button in here that must
		 * NOT close the menu it lives in -- it opens a level, it
		 * doesn't perform an action. */
		if (event.target.closest("a, button") && !event.target.closest(".menu-submenu-toggle"))
			menu.hidden = true;
	});
}

/*
 * Second level: the branches inside Services and System (PKI/DNS/...,
 * Software/Host/Devices) are real submenus that open on hover, rather
 * than headings over one long list. They were headings first; a menu
 * of thirteen items with five headings scans no faster than thirteen
 * items, whereas five things you point at do.
 *
 * Hover-to-open is done here rather than with CSS :hover so the same
 * open path serves click (touch, and keyboard focus) and so a submenu
 * that would run off the right edge of the window can be flipped to
 * the parent's left side -- neither of which CSS can decide on its
 * own. Opening one closes its siblings; leaving the parent <li>
 * closes it, which covers moving the pointer diagonally out of the
 * menu entirely.
 */
for (const sub of document.querySelectorAll(".menu-submenu")) {
	const toggle = sub.querySelector(".menu-submenu-toggle");
	const panel = sub.querySelector(".menu-submenu-menu");

	function openSubmenu() {
		for (const other of sub.parentElement.querySelectorAll(".menu-submenu-menu"))
			if (other !== panel)
				other.hidden = true;
		panel.hidden = false;
		toggle.setAttribute("aria-expanded", "true");

		/* Measured after it is visible, since a hidden element has no
		 * box to measure. 8px of margin so a flipped submenu never
		 * sits flush against the window edge. */
		panel.classList.remove("menu-submenu-menu-left");
		if (panel.getBoundingClientRect().right > window.innerWidth - 8)
			panel.classList.add("menu-submenu-menu-left");
	}
	function closeSubmenu() {
		panel.hidden = true;
		toggle.setAttribute("aria-expanded", "false");
	}

	sub.addEventListener("mouseenter", openSubmenu);
	sub.addEventListener("mouseleave", closeSubmenu);
	toggle.addEventListener("focus", openSubmenu);
	toggle.addEventListener("click", (event) => {
		event.stopPropagation();
		if (panel.hidden)
			openSubmenu();
		else
			closeSubmenu();
	});
}
document.addEventListener("click", closeAllMenus);
document.addEventListener("keydown", (event) => {
	if (event.key === "Escape")
		closeAllMenus();
});

/* A fixed panel is placed once, from coordinates that were true at
 * the moment it opened, so anything that moves its toggle afterwards
 * -- resizing the window, scrolling the menu bar sideways -- would
 * leave it behind, pointing at nothing. Re-place it rather than close
 * it: closing on scroll would also fire on the log panel's own
 * programmatic scrollTop write, which happens on every poll and has
 * nothing to do with the operator. Capture phase, because scrolling
 * inside an element does not bubble. */
function repositionOpenMenu() {
	if (openMenu)
		positionMenu(openMenu[0], openMenu[1]);
}
window.addEventListener("resize", repositionOpenMenu);
window.addEventListener("scroll", repositionOpenMenu, true);

/* Create-action items (data-modal, shared across every topical menu
 * above) all open the same modal shell the old single +Create
 * dropdown already used -- only where they live changed. */
for (const item of document.querySelectorAll(".menu-bar button[data-modal]")) {
	item.addEventListener("click", () => openModal(item.dataset.modal, item.dataset.title));
}

/* About Cix: read-only, fetched fresh on every open rather than
 * cached -- build/slot/kernel can change under an operator's feet
 * (an update staged to the inactive slot, a reboot) and this is the
 * one place meant to answer "what is this box actually running right
 * now," so a stale answer would defeat its own purpose. */
/*
 * This instance's own fully-qualified name, composed the same way
 * everywhere it is shown. site_name is the optional middle: a site
 * that has never been named yields host.domain rather than a name
 * with an empty label in the middle of it.
 */
function instanceFqdn(site) {
	return site.site_name
		? site.instance_name + "." + site.site_name + "." + site.domain_suffix
		: site.instance_name + "." + site.domain_suffix;
}

document.getElementById("menu-about").addEventListener("click", async () => {
	const instanceEl = document.getElementById("about-instance");
	const buildEl = document.getElementById("about-build");
	const slotEl = document.getElementById("about-slot");
	const kernelEl = document.getElementById("about-kernel");

	instanceEl.textContent = buildEl.textContent = slotEl.textContent = kernelEl.textContent = "…";
	try {
		const [boot, site] = await Promise.all([
			apiRequest("GET", CIX_API.getSystemBoot()),
			apiRequest("GET", CIX_API.getSystemSite()),
		]);
		instanceEl.textContent = instanceFqdn(site);
		buildEl.textContent = boot.build_version + " (" + boot.build_time + ")";
		slotEl.textContent = boot.slot || "(none)";
		kernelEl.textContent = boot.kernel_version || "(unknown)";
	} catch (e) {
		buildEl.textContent = "unavailable";
	}
});

/*
 * ---------- host authentication (ADR-0144) ----------
 *
 * A session token, persisted in localStorage -- this dashboard's own
 * analog of cixctl's ~/.cixctl_token dotfile, so a page reload
 * doesn't force a fresh login. GET requests never need it (the
 * daemon's write-gating leaves every GET open, always, regardless of
 * auth state); apiRequest()/apiRequestRaw() below attach it to every
 * request automatically once set -- one place, not every one of this
 * file's many call sites, the same design cix_client_set_token() gives
 * cixctl (client/include/httpclient.h).
 */
let authToken = storageGet("cix-auth-token") || null;
let authUsername = storageGet("cix-auth-username") || null;

const authStatusEl = document.getElementById("auth-status");
const authActionBtn = document.getElementById("menu-auth-action");

/*
 * #370: the dashboard showed "logged in: <user>" once you logged in and
 * showed NOTHING when write authentication was off entirely -- so a host
 * that would accept a mutating request from anyone rendered exactly like
 * a secured one. This is the missing state.
 *
 * Undefined means an older daemon that does not report the field; the
 * banner stays hidden rather than accusing a host it cannot ask.
 */
function updateAuthGatingBanner(active) {
	const el = document.getElementById("auth-gating-banner");

	if (!el)
		return;
	el.hidden = active !== false;
}

function updateAuthUi() {
	if (authToken) {
		authStatusEl.hidden = false;
		authStatusEl.className = statusBadge("ok");
		authStatusEl.textContent = authUsername ? "logged in: " + authUsername : "logged in";
		authActionBtn.textContent = "Log out";
	} else {
		authStatusEl.hidden = true;
		authActionBtn.textContent = "Log in";
	}
}

function setAuth(token, username) {
	authToken = token;
	authUsername = username;
	if (token) {
		storageSet("cix-auth-token", token);
		storageSet("cix-auth-username", username || "");
	} else {
		storageRemove("cix-auth-token");
		storageRemove("cix-auth-username");
	}
	updateAuthUi();
}

/* Opens the login modal on a 401 from any request, so "why did my
 * action just fail" has an immediate, actionable answer instead of
 * only a status-bar error -- the same reasoning cixctl's own
 * "authentication required -- POST /v1/login first" message serves,
 * adapted to a UI that can just show the form directly. Guarded so a
 * burst of 401s from one poll cycle (several concurrent GETs are
 * never gated, but a save-in-flight write easily could be) only ever
 * opens it once. */
function promptReauth() {
	if (modalOverlay.hidden)
		openModal("login-form", "Log in");
}

authActionBtn.addEventListener("click", async () => {
	if (authToken) {
		try {
			await apiRequest("POST", CIX_API.postLogout());
		} catch (e) {
			/* best-effort -- matches cixctl's own idempotent-logout
			 * posture; the local session clears either way */
		}
		setAuth(null, null);
		clearStatus();
	} else {
		openModal("login-form", "Log in");
		document.getElementById("lf-username").focus();
	}
});

document.getElementById("login-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const username = document.getElementById("lf-username").value.trim();
	const password = document.getElementById("lf-password").value;

	try {
		const result = await apiRequest("POST", CIX_API.postLogin(), { username: username, password: password });

		setAuth(result.token, username);
		document.getElementById("login-form").reset();
		closeModal();
		clearStatus();
	} catch (e) {
		showStatus("Login failed: " + e.message, true);
	}
});

updateAuthUi();

/*
 * Merged log panel (bottom of every page, ADR-0129): one stream, one
 * source filter, covering both this browser's own local activity
 * ("web-ui" -- every mutating request this dashboard makes, plus every
 * toast below, kept entirely client-side, never sent to the server
 * since it isn't a system component) and the server's own consolidated
 * log store (kernel/cixd/audit/container, GET /v1/system/logs,
 * ADR-0070/ADR-0126) re-read on every poll (pollServerLogs(), called
 * from the main poll() loop). The two are kept apart -- serverLogs is
 * replaced wholesale by each poll, clientLogs is only ever appended to
 * here -- and combined at render time, capped at MAX_LOG_ENTRIES across
 * both (a dedicated Logs page still exists, under System > Server,
 * for browsing/configuring the server's own full history/size cap;
 * this panel is a live tail, not a history browser).
 */
const MAX_LOG_ENTRIES = 300;
/*
 * Two lists, because they have different owners (#272).
 *
 * serverLogs is REPLACED wholesale by each poll rather than merged into.
 * That is the whole point: this store's timestamps are whole seconds,
 * and at boot hundreds of entries share one -- so a timestamp carries no
 * ordering information there at all, and the only correct order is the
 * one the server returns. Merging two overlapping responses destroyed
 * it, and no amount of client-side sorting could put it back: the panel
 * showed a kernel message on top while the newest entry was sshd
 * starting, both stamped the same second.
 *
 * clientLogs is what only the browser knows -- toasts from web-ui
 * actions -- and is the reason this cannot simply render the response
 * directly.
 */
let serverLogs = [];
let clientLogs = [];

let logSourceFilter = "";

const ANSI_FG_CLASSES = {
	30: "ansi-fg-black",
	31: "ansi-fg-red",
	32: "ansi-fg-green",
	33: "ansi-fg-yellow",
	34: "ansi-fg-blue",
	35: "ansi-fg-magenta",
	36: "ansi-fg-cyan",
	37: "ansi-fg-white",
	90: "ansi-fg-bright-black",
	91: "ansi-fg-bright-red",
	92: "ansi-fg-bright-green",
	93: "ansi-fg-bright-yellow",
	94: "ansi-fg-bright-blue",
	95: "ansi-fg-bright-magenta",
	96: "ansi-fg-bright-cyan",
	97: "ansi-fg-bright-white",
};

/* Parses SGR ("\x1b[...m") escape sequences into styled spans -- real
 * container log output (e.g. glauth's own zerolog) arrives pre-
 * colored; previously rendered as raw, unreadable escape bytes
 * (reported directly: "make those render on the web ui properly").
 * Only SGR (color/bold/dim/reset) is handled -- cursor-movement/
 * clear-screen and every other CSI escape class is matched and
 * dropped, never meaningful in a scrolling, append-only log panel.
 * An unrecognized SGR code number is silently ignored rather than
 * erroring, matching this codebase's own "degrade gracefully,
 * never invent structure" convention elsewhere (see e.g.
 * rewrite_basedn()'s own doc comment in daemon/src/ldap.c). */
function ansiToDom(text) {
	const fragment = document.createDocumentFragment();
	const re = /\x1b\[([0-9;]*)m|\x1b\[[0-9;]*[A-Za-z]/g;
	let lastIndex = 0;
	let activeClasses = [];
	let match;

	function flush(upTo) {
		if (upTo <= lastIndex) return;
		const chunk = text.slice(lastIndex, upTo);

		if (activeClasses.length === 0) {
			fragment.appendChild(document.createTextNode(chunk));
		} else {
			const span = document.createElement("span");

			span.className = activeClasses.join(" ");
			span.textContent = chunk;
			fragment.appendChild(span);
		}
	}

	while ((match = re.exec(text)) !== null) {
		flush(match.index);
		lastIndex = re.lastIndex;
		if (match[1] === undefined) continue; // a non-SGR CSI escape (cursor/clear) -- dropped
		const codes = match[1] === "" ? [0] : match[1].split(";").map(Number);

		for (const code of codes) {
			if (code === 0) activeClasses = [];
			else if (code === 1) activeClasses = activeClasses.filter((c) => c !== "ansi-bold").concat("ansi-bold");
			else if (code === 2) activeClasses = activeClasses.filter((c) => c !== "ansi-dim").concat("ansi-dim");
			else if (code === 39) activeClasses = activeClasses.filter((c) => !c.startsWith("ansi-fg-"));
			else if (ANSI_FG_CLASSES[code] !== undefined)
				activeClasses = activeClasses.filter((c) => !c.startsWith("ansi-fg-")).concat(ANSI_FG_CLASSES[code]);
		}
	}
	flush(text.length);
	return fragment;
}

/* A pure builder -- returns the element, does not touch the DOM tree
 * itself. Callers batch their own appendChild()/scrollTop so a poll
 * cycle adding many entries at once costs one reflow, not one per
 * entry (see rerenderLogPanel() below -- a real, confirmed-live
 * perf bug when this was previously one reflow per entry: the
 * dashboard's own routine 2s poll cycle alone issues ~30 requests,
 * each capable of contributing a new entry once server-log polling
 * merged in, ADR-0129). */
function buildLogEntryDom(entry) {
	const time = new Date(entry.ts * 1000).toTimeString().slice(0, 8);
	const el = document.createElement("div");

	el.className = "log-entry log-entry-" + entry.kind;

	const timeSpan = document.createElement("span");

	timeSpan.className = "log-entry-time";
	timeSpan.textContent = time + "  ";
	el.appendChild(timeSpan);

	const sourceSpan = document.createElement("span");

	sourceSpan.className = "log-entry-source";
	sourceSpan.textContent = "[" + entry.source + "] ";
	el.appendChild(sourceSpan);

	el.appendChild(ansiToDom(entry.text));
	return el;
}

/*
 * Newest first, at the top.
 *
 * The panel used to read like a terminal -- oldest at the top, newest
 * appended at the bottom, scrolled down to follow. That is the right
 * shape for a full-screen tail you are watching, and the wrong one for
 * a short pane at the bottom of a dashboard: what just happened is the
 * only thing anyone opens it for, and it was the one line furthest from
 * the eye and dependent on a scroll landing correctly.
 *
 * The cost, stated because it is real: a causal sequence now reads
 * upwards. glauth's own "watcher got event" -> "rewatching config" ->
 * "Config was reloaded" appears in that order bottom-to-top. For a
 * glanceable panel that is the better trade; for reading a sequence
 * through, GET /v1/system/logs and the Log Store view stay chronological.
 *
 * Everything below therefore inserts at the top, trims from the bottom
 * (the oldest, which is what a full panel should shed), and holds the
 * scroll at the top rather than the bottom.
 */
function trimAndScrollLogOutput() {
	while (logOutput.children.length > MAX_LOG_ENTRIES)
		logOutput.removeChild(logOutput.lastChild);
	logOutput.scrollTop = 0;
}

/* Single real-time entry (a toast, or one web-ui action) -- rare
 * enough that one reflow per call is fine. */
function addLogEntry(ts, source, level, text, kind) {
	const entry = { ts, source, level, text, kind };

	clientLogs.push(entry);
	while (clientLogs.length > MAX_LOG_ENTRIES)
		clientLogs.shift();
	if (logSourceFilter === "" || logSourceFilter === source) {
		logOutput.insertBefore(buildLogEntryDom(entry), logOutput.firstChild);
		trimAndScrollLogOutput();
	}
}

/*
 * A poll's worth of server entries. Replaces what was there rather than
 * merging into it -- see serverLogs' own note above for why merging
 * cannot be made correct at this timestamp resolution.
 */
function setServerLogs(entries) {
	serverLogs = entries.length > MAX_LOG_ENTRIES
	                     ? entries.slice(entries.length - MAX_LOG_ENTRIES)
	                     : entries;
	rerenderLogPanel();
}

function rerenderLogPanel() {
	const fragment = document.createDocumentFragment();

	/*
	 * The server's own order, kept exactly as sent, with the browser's
	 * own entries placed by timestamp around it. Rendered newest-first,
	 * so this walks backwards.
	 */
	const merged = serverLogs.concat(clientLogs);

	if (clientLogs.length > 0) {
		/* Stable, so server entries sharing a second keep the order the
		 * server gave them and a toast lands among them by time. */
		merged.sort((a, b) => a.ts - b.ts);
	}
	for (let i = merged.length - 1; i >= 0 && fragment.childNodes.length < MAX_LOG_ENTRIES; i--) {
		const entry = merged[i];

		if (logSourceFilter === "" || logSourceFilter === entry.source)
			fragment.appendChild(buildLogEntryDom(entry));
	}
	logOutput.textContent = "";
	logOutput.appendChild(fragment);
	logOutput.scrollTop = 0;
}

logPanelSource.addEventListener("change", () => {
	logSourceFilter = logPanelSource.value;
	rerenderLogPanel();
});

/* Every mutating request this dashboard makes, source "web-ui" --
 * routine poll GETs are deliberately not logged here (every 2s x 30
 * endpoints would drown out anything a human actually did) -- see
 * apiRequest()'s own method check. */
function logLine(method, path, statusText, kind) {
	addLogEntry(Math.floor(Date.now() / 1000), "web-ui", kind === "error" ? "error" : "info",
	            method + " " + path + " " + statusText, kind);
}

/*
 * Always the same request: the most recent LOG_TAIL entries the store
 * holds, every poll.
 *
 * This used to track a since= cursor and merge each response into what
 * was already rendered, with a dedupe set for the boundary second. All
 * of that existed to avoid re-fetching, and none of it could be made
 * correct: the store's timestamps are whole seconds, so an incremental
 * fetch cannot tell which entries within the newest second it has
 * already seen, and a bounded tail can cut some of them entirely -- they
 * then arrive on a later poll and get placed after entries that are
 * genuinely newer.
 *
 * Re-fetching a bounded tail is cheap, cannot drift, and needs no
 * cursor: what the server returns IS the panel. It also fixed a real
 * bug on the way in -- the old request was built as
 * getSystemLogs(logsSinceTs) + "?since=%s&tail=500", and
 * getSystemLogs() takes no arguments, so the timestamp was discarded
 * and the literal characters "%s" were sent as the value.
 */
const LOG_TAIL = MAX_LOG_ENTRIES;
const LOG_ERROR_LEVELS = new Set(["emerg", "alert", "crit", "err", "error", "warning", "warn"]);

async function pollServerLogs() {
	let entries;

	try {
		entries = await apiRequest("GET", CIX_API.getSystemLogs() + "?since=0&tail=" + LOG_TAIL);
	} catch (e) {
		return; /* best-effort, matches every other poll()'s own error tolerance */
	}
	if (!entries) return;

	setServerLogs(
	        entries.map((e) => ({
	                ts: e.ts,
	                source: e.source,
	                level: e.level,
	                text: e.container ? "(" + e.container + ") " + e.msg : e.msg,
	                kind: LOG_ERROR_LEVELS.has(e.level) ? "error" : "ok",
	        })));
}

/* Toast (ADR-0129): a fixed-position popup with a disappear timer,
 * replacing the old always-content-flow status bar -- same element/
 * id/classes (only the CSS positioning changed), so every existing
 * showStatus()/clearStatus() call site needed no change. Every toast
 * also lands in the merged log panel above (source "web-ui") -- the
 * user's own request: "it should be shown in the logs right?". */
const STATUS_AUTO_DISMISS_MS = 5000;
let statusTimeoutId = null;

/*
 * The one place the status-pill vocabulary lives (#459). A caller passes
 * a semantic kind and gets the full class string; the "badge " prefix
 * and the four valid colours live here and nowhere else, so a page
 * cannot invent a badge class or misspell one. Mapping a domain's own
 * state (a container "running", a format "ready") to a kind is the
 * caller's domain knowledge; the colour that kind resolves to is fixed:
 *   ok      -- healthy / present / running   (green)
 *   error   -- failed / broken               (red)
 *   paused  -- paused / idle / in progress   (amber)
 *   unknown -- unknown / not applicable      (grey, the fallback)
 * See docs/guides/web-ux-guidelines.md ("Status badge").
 */
function statusBadge(kind) {
	switch (kind) {
	case "ok":
	case "error":
	case "paused":
		return "badge badge-" + kind;
	default:
		return "badge badge-unknown";
	}
}

/*
 * The pipeline's own status vocabulary [ok, blocked, failed, cancelled,
 * not-implemented], mapped to the shared badge kinds (#460) so the
 * pipeline uses the one status-pill widget instead of its own outlined
 * pipeline-badge system. "blocked" is waiting, not broken -- deliberately
 * amber (paused), not red, so a screen of blocked rows does not read as a
 * screen of failures.
 */
function pipelineStatusKind(status) {
	switch (status) {
	case "ok":
		return "ok";
	case "blocked":
		return "paused";
	case "failed":
	case "cancelled":
		return "error";
	default:
		return "unknown"; /* not-implemented, and anything unrecognised */
	}
}

function showStatus(message, isError) {
	statusBox.textContent = message;
	statusBox.hidden = false;
	statusBox.className = "status " + (isError ? "status-error" : "status-ok");
	addLogEntry(Math.floor(Date.now() / 1000), "web-ui", isError ? "error" : "info", message,
	            isError ? "error" : "ok");
	if (statusTimeoutId !== null)
		clearTimeout(statusTimeoutId);
	statusTimeoutId = setTimeout(() => {
		statusBox.hidden = true;
		statusTimeoutId = null;
	}, STATUS_AUTO_DISMISS_MS);
}

function clearStatus() {
	statusBox.hidden = true;
	if (statusTimeoutId !== null) {
		clearTimeout(statusTimeoutId);
		statusTimeoutId = null;
	}
}

/*
 * The status bar's two LEDs: color is daemon reachability
 * (refreshHealth() below owns that), a blink is real traffic -- TX on
 * every request this page sends, RX on every response that comes back.
 *
 * Restarted rather than extended when a blink is already running: back
 * -to-back requests (the poll loop sends a couple of dozen) would
 * otherwise hold the lamp permanently lit, which is exactly as
 * informative as no lamp at all. Clearing the class, forcing a reflow
 * and re-adding it is what makes each one a distinct flash.
 */
function ledBlink(led) {
	/*
	 * Coalesced: a burst of requests (the poll loop still sends a
	 * handful at once) would otherwise force a synchronous layout per
	 * request per lamp, and hold the lamp permanently lit into the
	 * bargain -- neither useful nor free. One flash per window is what
	 * an eye can resolve anyway.
	 */
	const now = Date.now();

	if (led.blinkingUntil !== undefined && now < led.blinkingUntil)
		return;
	led.blinkingUntil = now + 120;
	led.classList.remove("led-blink");
	void led.offsetWidth;
	led.classList.add("led-blink");
	clearTimeout(led.blinkTimer);
	led.blinkTimer = setTimeout(() => led.classList.remove("led-blink"), 130);
}

/*
 * How long a health check may go unanswered before it counts as
 * unreachable (#231).
 *
 * Under the poll interval on purpose, so a check can never outlive the
 * next one and leave two in flight arguing about the same lamp.
 */
const HEALTH_TIMEOUT_MS = 1500;

async function apiRequest(method, path, body, timeoutMs) {
	ledBlink(ledTx);
	const opts = { method: method, headers: {} };

	/*
	 * A request that hangs is not a request that succeeded, and without
	 * this the difference was invisible (#231).
	 *
	 * fetch() has no timeout of its own: against a daemon that accepts
	 * the connection and then never answers -- exactly what #237's
	 * blocking writes produced -- the promise neither resolves nor
	 * rejects. Neither branch of refreshHealth() runs, the reachability
	 * state keeps whatever it last had, and the panel shows GREEN for
	 * as long as the outage lasts while TX keeps blinking on every
	 * doomed poll. That is the reported symptom precisely: "no
	 * reachability and tx rx stay green and blink occasionally".
	 *
	 * An indicator that cannot go red while nothing answers is worse
	 * than no indicator, because it is trusted.
	 */
	if (timeoutMs !== undefined && typeof AbortSignal !== "undefined" && AbortSignal.timeout)
		opts.signal = AbortSignal.timeout(timeoutMs);
	if (authToken)
		opts.headers["Authorization"] = "Bearer " + authToken;
	if (body !== undefined) {
		opts.headers["Content-Type"] = "application/json";
		opts.body = JSON.stringify(body);
	}

	let res;

	/*
	 * TX blinks above, before this, and stays that way deliberately: a
	 * transmit lamp flashes on transmit, whether or not anything
	 * answers, which is what the panel this imitates does. What was
	 * missing is the other half -- an exchange that failed left no
	 * trace at all beyond a colour that had not caught up yet.
	 */
	try {
		res = await fetch(path, opts);
	} catch (e) {
		noteTransportFailure();
		throw e;
	}

	ledBlink(ledRx);
	let json = null;
	if (res.status !== 204) {
		try {
			json = await res.json();
		} catch (e) {
			json = null;
		}
	}
	if (!res.ok) {
		const message = json && json.error ? json.error : "request failed (HTTP " + res.status + ")";
		/*
		 * The status travels with the error. Without it a caller can
		 * only match on the message text, and some non-2xx answers are
		 * ordinary facts rather than failures -- a container created
		 * directly through the API has no recipe, and its 404 should
		 * read as "there isn't one" rather than as something broken.
		 * Telling those apart by string comparison would break the
		 * moment the daemon reworded itself.
		 */
		const err = new Error(message);

		err.status = res.status;
		if (method !== "GET")
			logLine(method, path, "-> " + res.status + " " + message, "error");
		if (res.status === 401 && path !== CIX_API.postLogin())
			promptReauth();
		throw err;
	}
	if (method !== "GET")
		logLine(method, path, "-> " + res.status, "ok");
	return json;
}

/* Raw variant for /v1/system/backup|restore -- the response/request
 * body IS the bundle, written/read byte-for-byte, the same "exact
 * round trip, not re-serialized" guarantee cixctl's own backup/
 * restore commands already have (cli/src/main.c). */
async function apiRequestRaw(method, path, rawBody) {
	const opts = { method: method, headers: {} };
	if (authToken)
		opts.headers["Authorization"] = "Bearer " + authToken;
	if (rawBody !== undefined) {
		opts.headers["Content-Type"] = "application/json";
		opts.body = rawBody;
	}
	const res = await fetch(path, opts);
	const text = await res.text();
	if (!res.ok) {
		let message = "request failed (HTTP " + res.status + ")";
		try {
			const json = JSON.parse(text);
			if (json && json.error)
				message = json.error;
		} catch (e) {
			/* not JSON -- keep the generic message */
		}
		logLine(method, path, "-> " + res.status + " " + message, "error");
		if (res.status === 401)
			promptReauth();
		throw new Error(message);
	}
	logLine(method, path, "-> " + res.status, "ok");
	return text;
}

function readFileAsText(file) {
	return new Promise((resolve, reject) => {
		const reader = new FileReader();

		reader.onload = () => resolve(reader.result);
		reader.onerror = () => reject(reader.error);
		reader.readAsText(file);
	});
}

/*
 * issue #20: a single missed poll used to flip the dot straight to
 * "unreachable" -- indistinguishable from a real outage. consecutiveHealthFailures
 * tracks misses since the last success so a lone transient blip reads as
 * "degraded" (amber, same token .paused already uses) rather than red;
 * only 2+ *consecutive* misses earn the harsher "unreachable" state. The
 * next poll (POLL_INTERVAL_MS, already 2s) is the real retry here -- no
 * separate backoff timer needed on top of a loop that already re-checks
 * this often.
 */
let consecutiveHealthFailures = 0;

/*
 * A request that never reached the daemon is evidence about
 * reachability, and used to be discarded (#231).
 *
 * refreshHealth() polls every POLL_INTERVAL_MS and owns the lamp
 * colour, so a click that fails right now left the panel showing the
 * last poll's answer -- green, while the thing the operator just did
 * had already failed. Up to a couple of seconds of the display
 * disagreeing with what the operator watched happen.
 *
 * Counted into the same consecutive-failure tally rather than a second
 * one, so a real outage still needs two failures to go red and one
 * blip still reads as degraded. This adds evidence to that count; it
 * does not invent a second opinion about reachability.
 */
function noteTransportFailure() {
	consecutiveHealthFailures++;
	if (consecutiveHealthFailures >= 2) {
		statusLeds.className = "status-leds led-state-error";
		statusLeds.title = "Daemon unreachable (" + consecutiveHealthFailures +
			" consecutive failures)";
	} else {
		statusLeds.className = "status-leds led-state-degraded";
		statusLeds.title = "A request did not reach the daemon — retrying";
	}
}


async function refreshHealth() {
	const start = performance.now();

	try {
		const health = await apiRequest("GET", CIX_API.getHealth(), undefined, HEALTH_TIMEOUT_MS);
		const ms = Math.round(performance.now() - start);

		/*
		 * #370: an open control plane must never look like a secured
		 * one. health carries auth_gating_active, so the banner rides
		 * the poll that already runs -- no extra request, and it
		 * clears by itself the moment gating is configured.
		 */
		updateAuthGatingBanner(health && health.auth_gating_active);

		/* Back after an absence: the daemon may have restarted into a
		 * different build, so re-ask rather than keep showing the one
		 * that was running before it went away. */
		if (consecutiveHealthFailures > 0)
			refreshStatusVersion();
		consecutiveHealthFailures = 0;
		statusLeds.className = "status-leds led-state-ok";
		statusLeds.title = "Daemon reachable — " + ms + "ms";
	} catch (e) {
		/*
		 * A timeout is reported as what it is. "Unreachable" and
		 * "accepted the connection and then said nothing" are
		 * different faults with different causes, and the second one
		 * is the one that used to be indistinguishable from health.
		 */
		const timedOut = e && (e.name === "TimeoutError" || e.name === "AbortError");

		consecutiveHealthFailures++;
		if (consecutiveHealthFailures >= 2) {
			statusLeds.className = "status-leds led-state-error";
			statusLeds.title = timedOut
				? "Daemon not answering — " + consecutiveHealthFailures +
				  " checks timed out after " + HEALTH_TIMEOUT_MS + "ms"
				: "Daemon unreachable (" + consecutiveHealthFailures + " consecutive failed checks)";
		} else {
			statusLeds.className = "status-leds led-state-degraded";
			statusLeds.title = timedOut
				? "Daemon did not answer within " + HEALTH_TIMEOUT_MS + "ms — retrying"
				: "Daemon check failed once — retrying";
		}
	}
}

/* "3d 4h" / "2h 17m" / "45s" -- two units at most, largest first,
 * which is as much precision as an uptime reading is ever read for. */
function formatUptime(seconds) {
	const d = Math.floor(seconds / 86400);
	const h = Math.floor((seconds % 86400) / 3600);
	const m = Math.floor((seconds % 3600) / 60);

	if (d > 0)
		return d + "d " + h + "h";
	if (h > 0)
		return h + "h " + m + "m";
	if (m > 0)
		return m + "m " + (seconds % 60) + "s";
	return seconds + "s";
}

/*
 * The last stats fetch, so the uptimes can tick locally between them.
 * Counting seconds client-side is exactly as accurate as asking, and a
 * clock that only moves when you poll it is a poll you are doing for a
 * number you could have added yourself.
 */
let statusMetaBase = null;

/*
 * Which build is actually running, and out of which slot. Fetched
 * rather than derived: after an A/B update the page in front of you
 * may well be older than the daemon answering it, and the version in
 * the corner is the one thing that has to be the daemon's own answer.
 */
let statusVersion = null;

/*
 * The build this PAGE came from.
 *
 * These assets were served by the daemon that answered the first
 * /system/boot of this page's life, so the first version seen is the
 * version of the HTML and JavaScript now running. If a later answer
 * differs, the daemon has been updated underneath a page that has not
 * been -- the dashboard is then a client of a contract it was not
 * built against, which is exactly how a renamed field reads as an
 * empty panel rather than as an error.
 *
 * Derived, never stamped at build time: nothing has to be kept in step
 * for this to stay true.
 */
let loadedVersion = null;

function versionIsSkewed() {
	return loadedVersion !== null && statusVersion !== null &&
	       statusVersion.version !== loadedVersion;
}

/*
 * Built once and updated in place, never rebuilt.
 *
 * This runs on a one-second ticker (the uptimes count up), and
 * recreating the version control each tick would reset a tooltip the
 * moment someone hovered it and could swallow a click landing between
 * the removal and the re-append. The listener is attached once; only
 * text and class change.
 */
let statusVersionEl = null;
let statusRestEl = null;

function renderStatusMeta() {
	const parts = [];

	if (statusVersionEl === null) {
		statusVersionEl = document.createElement("button");
		statusVersionEl.type = "button";
		statusVersionEl.className = "status-version";
		statusVersionEl.addEventListener("click", () => location.reload());
		statusRestEl = document.createElement("span");
		statusMeta.textContent = "";
		statusMeta.appendChild(statusVersionEl);
		statusMeta.appendChild(statusRestEl);
	}

	if (statusVersion !== null) {
		/* Slot is an A/B fact and only a real installed host has one --
		 * a dev daemon reports none, and "slot ?" would be inventing an
		 * answer to a question that does not apply. */
		statusVersionEl.hidden = false;
		statusVersionEl.textContent = "Cix " + statusVersion.version +
		    (statusVersion.slot !== null ? " \u00b7 slot " + statusVersion.slot : "");
		statusVersionEl.classList.toggle("status-version-skew", versionIsSkewed());
		statusVersionEl.title = versionIsSkewed()
		    ? "This page was loaded from Cix " + loadedVersion + ", but the daemon is now " +
		      "running " + statusVersion.version + ". Click to reload the dashboard."
		    : "Click to reload the dashboard.";
	} else {
		statusVersionEl.hidden = true;
	}

	if (statusMetaBase !== null) {
		const elapsed = Math.floor((Date.now() - statusMetaBase.fetchedAt) / 1000);

		parts.push("up " + formatUptime(statusMetaBase.host + elapsed));
		parts.push("cixd " + formatUptime(statusMetaBase.daemon + elapsed));
		parts.push("load " + statusMetaBase.load1.toFixed(2) +
		           " " + statusMetaBase.load5.toFixed(2) +
		           " " + statusMetaBase.load15.toFixed(2));
	}
	statusRestEl.textContent = parts.length === 0 ? ""
	    : (statusVersion !== null ? "  \u00b7  " : "") + parts.join("  \u00b7  ");
}

/*
 * The running build never changes under a daemon that stays up, so
 * this is fetched once at load and again only after the daemon has
 * been away -- a restart is the one thing that can change the answer,
 * and it is exactly what the LEDs going red and back already detect.
 */
async function refreshStatusVersion() {
	try {
		const b = await apiRequest("GET", CIX_API.getSystemBoot());

		statusVersion = { version: b.build_version || "unknown", slot: b.slot || null };
		if (loadedVersion === null)
			loadedVersion = statusVersion.version;
		statusMeta.title = "Cix " + statusVersion.version +
		                   ", built " + (b.build_time || "unknown") +
		                   (statusVersion.slot !== null ? ", running from slot " + statusVersion.slot : "") +
		                   " on kernel " + (b.kernel_version || "unknown");
		renderStatusMeta();
	} catch (e) {
		/* Unreachable is already said by the LEDs; the last known
		 * version stays put rather than blanking, since it is still
		 * the last true thing anyone told us. */
	}
}

/*
 * Host uptime, this daemon's own uptime, and load average, along the
 * left of the status bar. The two uptimes are separate on purpose:
 * they diverge after a control-plane restart that was not a reboot,
 * and "the box has been up for days but cixd restarted four minutes
 * ago" is exactly the thing worth noticing.
 *
 * Its own slow interval rather than the 2s poll -- nothing here moves
 * fast enough to be worth a request every two seconds, and the poll
 * loop is already the busiest thing this page does.
 */
async function refreshStatusMeta() {
	try {
		const s = await apiRequest("GET", CIX_API.getSystemStats());
		const up = s.uptime || {};
		const load = s.load || {};

		statusMetaBase = {
			fetchedAt: Date.now(),
			host: up.host_seconds || 0,
			daemon: up.daemon_seconds || 0,
			load1: load.load1 || 0,
			load5: load.load5 || 0,
			load15: load.load15 || 0,
		};
		renderStatusMeta();
	} catch (e) {
		/* Unreachable is already said by the LEDs going red -- saying it
		 * twice in two places adds nothing, so the last known values
		 * simply stand. */
	}
}

/* ---------- routing ---------- */

/*
 * Remembers the last real page visited (LAST_VIEW_KEY, same
 * localStorage-preference convention as tree collapse/log panel
 * collapse/theme above) -- a bare load with no hash at all (a fresh
 * tab, a bookmark to the plain origin) restores it instead of always
 * landing on Containers. Reloading with a hash already present in the
 * URL bar needs none of this -- the browser keeps that on its own;
 * this only covers the case where there's nothing for the browser
 * itself to have remembered.
 */
const LAST_VIEW_KEY = "cix-last-view";

function saveLastView() {
	try {
		storageSet(LAST_VIEW_KEY, location.hash);
	} catch (e) {
		/* localStorage unavailable -- last view just won't survive a
		 * fresh tab/reload; starts on the default view instead. */
	}
}

function restoreLastViewIfNoHash() {
	if (location.hash !== "")
		return;
	try {
		const saved = storageGet(LAST_VIEW_KEY);

		if (saved)
			location.hash = saved;
	} catch (e) {
		/* localStorage unavailable -- start on the default view. */
	}
}

function parseHash() {
	const raw = location.hash.replace(/^#/, "");
	if (raw === "")
		return { category: "containers", name: null };
	const slash = raw.indexOf("/");
	if (slash < 0)
		return { category: raw, name: null };
	return { category: raw.slice(0, slash), name: decodeURIComponent(raw.slice(slash + 1)) };
}

const CATEGORY_VIEWS = {
	containers: "view-containers",
	networks: "view-networks",
	"routes": "view-networks",
	devices: "view-devices",
	devicemaps: "view-devices",
	kmod: "view-devices",
	sysctl: "view-kernel",
	storage: "view-storage",
	"dns-records": "view-dns-records",
	"dns-servers": "view-dns-records",
	"dns-forwarders": "view-dns-records",
	"ldap-servers": "view-ldap-servers",
	"ldap-groups": "view-ldap-servers",
	"ldap-users": "view-ldap-servers",
	"ldap-config": "view-ldap-servers",
	"ntp-config": "view-ntp-config",
	"ntp-servers": "view-ntp-config",
	"ntp-time": "view-ntp-config",
	"dhcp-servers": "view-dhcp",
	"dhcp-ranges": "view-dhcp",
	"dhcp-static": "view-dhcp",
	"dhcp-leases": "view-dhcp",
	"pki-ca": "view-pki-ca",
	"pki-intermediate": "view-pki-ca",
	"pki-certs": "view-pki-ca",
	/* The three recipe kinds were sub-tabs inside one "Recipes" tab;
	 * they are page-level tabs now, so each is a real address. The
	 * old #recipes still lands on the page (with its first tab) so a
	 * bookmark does not break -- it just no longer names a tab. */
	recipes: "view-catalogue",
	"pkg-recipes": "view-catalogue",
	"image-recipes": "view-catalogue",
	"container-recipes": "view-catalogue",
	pipeline: "view-pipeline",
	"pipeline-errors": "view-pipeline",
	"site": "view-host",
	"daemon-config": "view-control-plane",
	"host-swap": "view-host",
	"rolling-restart": "view-deployment",
	"build-overview": "view-integration",
	"pkg-build-config": "view-integration",
	"hostauth-sessions": "view-host",
	"host-stats": "view-host",
	processes: "view-host",
	"syslog-targets": "view-syslog-targets",
	"tls-throttle": "view-control-plane",
	"control-plane-reservation": "view-control-plane",
	"boot-console": "view-bootloader",
	esp: "view-bootloader",
	"signing-keys": "view-bootloader",
	"kernel-policy": "view-kernel",
	logs: "view-host",
	kmsg: "view-kernel",
	"server-health": "view-server-health",
	stalls: "view-control-plane",
	schedules: "view-control-plane",
	volumes: "view-storage",
	/* Repo & Sync and Cache & Artifacts became tabs on the Catalogue
	 * page. Their old addresses still resolve to it (with the right tab
	 * showing) rather than 404-ing a bookmark someone already has. */
	"volume-backup-config": "view-storage",
	/* Images and Packages are Catalogue tabs now, not pages. Their bare
	 * addresses still resolve to it (with the right tab showing) --
	 * #images/{name} is unaffected, since DETAIL_VIEWS is consulted
	 * first whenever a route carries a name. */
	images: "view-integration",
	packages: "view-integration",
	"pkg-repo": "view-catalogue",
	"pkg-cache": "view-integration",
	"factory-reset": "view-host",
	"storage-placement": "view-storage",
	"backup": "view-storage",
	"update": "view-deployment",
	"reconcile": "view-deployment",
	iso: "view-deployment",
	"running-config": "view-host",
};
/*
 * Is the page currently on screen the one that owns `category`?
 *
 * Renderers used to ask for their own route by name directly,
 * which was right when one route meant one page. Now a page carries a
 * dozen tabs and the route is the TAB, so a renderer guarding on its
 * own old category never fired while any other tab of the same page was
 * addressed -- the data was fetched and then thrown away, and the panel
 * read "Loading" forever. Comparing PAGES rather than routes is the
 * only comparison that stays true as tabs move between pages.
 */
function onPageOf(category) {
	const here = CATEGORY_VIEWS[parseHash().category];

	return here !== undefined && here === CATEGORY_VIEWS[category];
}



const DETAIL_VIEWS = {
	containers: "view-container-detail",
	storage: "view-storage-detail",
	volumes: "view-volume-detail",
	networks: "view-network-detail",
	images: "view-image-detail",
	packages: "view-package-detail",
};

/*
 * Shows the tab that `category` addresses, on whichever page owns it.
 *
 * There used to be two of these plus a SERVICE_TAB_VIEWS table -- a
 * hand-kept second copy of CATEGORY_VIEWS naming, for every tab-shaped
 * address, the page it sat on. Two tables describing one fact is how a
 * tab moves pages and silently stops selecting itself, and moving tabs
 * between pages is now routine rather than rare. Neither fact needs
 * storing twice: the page is CATEGORY_VIEWS' answer already, and
 * whether an address is a tab at all is a question that page's own tab
 * bar answers by having (or not having) a button for it.
 *
 * Scoped to the page's OWN tab bar and panels -- Catalogue's Recipes
 * panel contains a second, nested tab bar, and an unscoped query would
 * hide its sub-panels every time the outer tab changed.
 */
function tabButtonFor(category) {
	const view = document.getElementById(CATEGORY_VIEWS[category] || "");

	if (view === null)
		return null;
	return view.querySelector(
		':scope > .tab-bar > .tab-button[data-tab="' + category + '"]');
}

function selectTabFor(category) {
	const btn = tabButtonFor(category);

	if (btn === null)
		return;

	const view = btn.closest("section.view");

	for (const other of view.querySelectorAll(":scope > .tab-bar > .tab-button"))
		other.classList.toggle("active", other === btn);
	for (const panel of view.querySelectorAll(":scope > .tab-panel"))
		panel.hidden = panel.dataset.tab !== category;
}

/* The category last rendered, so a re-render triggered by the poll loop
 * can be told apart from real navigation. Anything that a user can
 * change after arriving -- which tab is showing -- must only be set on
 * the latter. */
let lastRenderedRoute = null;

/*
 * The console as its own window.
 *
 * Same page, same origin, same session cookie, same VT -- only the
 * chrome is gone. That is deliberate: a popout built as a second
 * standalone page would be a second copy of the terminal, the resize
 * handling and the reconnect rules, and this project has paid for
 * parallel implementations before. The route carries the container and,
 * optionally, which declared console to attach to.
 *
 * Idempotent, because the poll loop calls renderCurrentView() every two
 * seconds: openConsole()'s own same-container guard makes the repeat a
 * no-op rather than a reconnect.
 */
function renderConsolePopout(spec) {
	const slash = spec.indexOf("/");
	const cname = slash < 0 ? spec : spec.slice(0, slash);
	const csel = slash < 0 ? null : spec.slice(slash + 1);
	const panel = document.getElementById("cd-panel-console");
	/* The console lives in the container DETAIL view, not the list --
	 * DETAIL_VIEWS.containers. Naming the list here rendered the
	 * container table in the popout window instead of the terminal. */
	const view = document.getElementById(DETAIL_VIEWS.containers);

	document.body.classList.add("console-popout");
	{
		const btn = document.getElementById("cd-console-popout");

		/* This window IS the popout; offering to open another one from
		 * inside it is a control that does nothing useful. */
		if (btn !== null)
			btn.hidden = true;
	}
	document.title = cname + (csel ? " \u2014 " + csel : "") + " \u2014 console";

	/* The picker is not shown here: which console this window is for was
	 * decided when it was opened, and it is in the URL. Set the
	 * selection directly rather than deriving it from the container
	 * list, so this window does not depend on that fetch having
	 * completed. */
	if (csel)
		consoleSelected = csel;

	/* Every other view stays hidden: this window shows one thing. */
	for (const v of document.querySelectorAll(".view"))
		v.hidden = v.id !== DETAIL_VIEWS.containers;
	if (view !== null && panel !== null) {
		for (const other of view.querySelectorAll(":scope > .tab-panel"))
			other.hidden = other !== panel;
		panel.hidden = false;
	}
	openConsole(cname);
}

function renderCurrentView() {
	const route = parseHash();
	const allViews = document.querySelectorAll(".view");

	if (route.category === "console-popout" && route.name !== null) {
		renderConsolePopout(route.name);
		return;
	}

	for (const v of allViews)
		v.hidden = true;

	const detailViewId = route.name !== null ? DETAIL_VIEWS[route.category] : null;
	const viewId = detailViewId || CATEGORY_VIEWS[route.category];

	if (viewId === undefined)
		return;
	const view = document.getElementById(viewId);
	if (view === null)
		return;
	view.hidden = false;
	saveLastView();

	if (route.category === "containers" && route.name !== null) {
		renderContainerDetail(route.name);
	} else {
		/* Navigating away from a container's own detail view (or to a
		 * different one -- renderContainerDetail() itself handles the
		 * same-container case) -- never leave a console session's
		 * WebSocket open once its own view isn't showing. */
		if (consoleContainerName !== null)
			closeConsole();
		if (statsContainerName !== null)
			stopStatsPolling();
		if (route.category !== "host-stats")
			stopHostStatsPolling();
		if (route.category === "networks" && route.name !== null)
			renderNetworkDetail(route.name);
		else if (netPortsName !== null)
			stopNetPortsPolling();
		else if (route.category === "storage" && route.name !== null)
			renderDiskDetail(route.name);
		/*
		 * Independent per-PAGE checks, not an else-if chain on the
		 * route.
		 *
		 * As a chain, only the renderer for the ADDRESSED route ran --
		 * so on a page with a dozen tabs, eleven of them never
		 * rendered and sat on "Loading" while their data was fetched
		 * and cached. Kernel Modules was the last one still doing it
		 * after two earlier fixes to the same underlying mistake:
		 * comparing routes where the thing that matters is the page.
		 */
		if (onPageOf("routes"))
			renderRoutesList();
		if (onPageOf("sysctl"))
			renderSysctlList();
		if (onPageOf("kmod"))
			renderKmodList();
		if (onPageOf("host-stats"))
			startHostStatsPolling();
		/*
		 * On ARRIVAL and on its own Refresh button only. A real
		 * process table churns far faster than a person can read it,
		 * which is why this page was always fetch-on-demand; polling
		 * it made the page unusable rather than live.
		 */
		if (onPageOf("processes") && lastRenderedRoute !== route.category)
			renderProcessesList();
		if (onPageOf("syslog-targets"))
			renderSyslogTargetsList();
		if (onPageOf("tls-throttle"))
			refreshTlsThrottleStatus();
		if (onPageOf("control-plane-reservation"))
			refreshControlPlaneReservation();
		if (onPageOf("boot-console"))
			refreshBootConsole();
		if (onPageOf("esp"))
			refreshEsp();
		if (onPageOf("signing-keys")) {
			refreshSigningKeys();
			refreshReleaseKey();
		}
		if (onPageOf("kernel-policy"))
			refreshKernelPolicy();
		if (onPageOf("logs"))
			renderLogsList();
		if (onPageOf("kmsg"))
			refreshKmsg();
		if (onPageOf("server-health"))
			refreshServerHealth();
		if (onPageOf("stalls"))
			refreshStalls();
		if (onPageOf("schedules"))
			refreshSchedules();
		if (onPageOf("running-config"))
			refreshRunningConfig();
		if (onPageOf("volume-backup-config"))
			refreshVolumeBackupConfig();
		if (onPageOf("devices"))
			renderDevices();
		if (onPageOf("storage"))
			renderDisks();
		if (onPageOf("storage-placement"))
			renderAllStoragePlacements();
		if (onPageOf("backup"))
			renderBackupConfig();
		if (onPageOf("hostauth-sessions"))
			refreshHostauthSessions();

		/*
		 * A name in the address means a DETAIL page, which is a
		 * different section from the list it came from -- so these stay
		 * paired with else, unlike every check above.
		 */
		if (route.category === "volumes" && route.name !== null)
			renderVolumeDetail(route.name);
		else if (onPageOf("volumes"))
			refreshVolumes();
		if (route.category === "images" && route.name !== null)
			renderImageDetail(route.name);
		if (route.category === "packages" && route.name !== null)
			renderPackagesView(route.name);

		/*
		 * The Pipeline's four pages. Each renders everything its own
		 * tabs show, because a tab is switched without navigating and a
		 * panel that renders only when addressed sits on "Loading".
		 */
		if (onPageOf("pkg-recipes")) {
			renderRecipesList();
			renderImageRecipesTable();
			renderContainerRecipesTable();
		}
		if (onPageOf("pkg-build-config")) {
			refreshBuildLogs();
			renderPackagesView(null);
			renderImages(cache.images);
		}
		if (onPageOf("update"))
			refreshSoftwareReconcile();
		if (onPageOf("iso"))
			refreshIso();
	}

	/*
	 * On ARRIVAL only. renderCurrentView() also runs every poll, and
	 * forcing the tab there snapped the page back to its addressed tab
	 * roughly every two seconds -- clicking any other tab appeared to
	 * bounce. An address selects a tab on arrival; after that the tab
	 * bar owns it.
	 */
	if (lastRenderedRoute !== route.category)
		selectTabFor(route.category);
	lastRenderedRoute = route.category;
	renderTreeActive();
}

window.addEventListener("hashchange", () => {
	renderCurrentView();
	ensureActiveCategoryExpanded();
});

/* ---------- tree ---------- */

const TREE_COLLAPSE_KEY = "cix-tree-collapsed";

function loadCollapsedCategories() {
	try {
		const raw = storageGet(TREE_COLLAPSE_KEY);

		return raw ? new Set(JSON.parse(raw)) : new Set();
	} catch (e) {
		return new Set();
	}
}

function saveCollapsedCategories() {
	try {
		storageSet(TREE_COLLAPSE_KEY, JSON.stringify(Array.from(collapsedCategories)));
	} catch (e) {
		/* localStorage unavailable (private browsing, quota, ...) --
		 * collapse state just won't survive a reload. */
	}
}

const collapsedCategories = loadCollapsedCategories();

/*
 * A category and its own "representative" child deliberately share one
 * route hash (System and PKI and Root CA can all legitimately route to
 * "pki-ca") -- so route hash alone can never safely stand in for a
 * tree row's own identity: viewing "Servers" would otherwise also
 * light up "Records" (DNS's own aliased-to first child), since both
 * share hash "dns-records". Every row gets its own unique numeric id
 * instead, rebuilt fresh by renderTree() every poll:
 *   nodeParent[id]   -- the row's real structural parent id (or
 *                        undefined for a top-level row) -- unambiguous,
 *                        independent of whatever hash it happens to share.
 *   nodeDepth[id]    -- nesting depth, used to pick the most specific
 *                        of several rows that share one route hash.
 *   hashToNodeIds[h] -- every row whose own href is exactly hash h.
 *   nodeToggle[id] / nodeUl[id] -- direct references for
 *                        ensureActiveCategoryExpanded(), no re-querying.
 *   nodeAnchor[id]   -- direct reference for renderTreeActive().
 *   nodeHash[id]     -- the row's own route hash (for renderTreeActive()'s
 *                        own hash-based highlight walk -- see its own
 *                        comment for why that direction is fine to
 *                        share across aliased nodes).
 *   nodePath[id]     -- the row's own label-built path, needed to
 *                        update collapsedCategories (path-keyed, not
 *                        hash-keyed -- see buildTreeNode()'s own
 *                        comment for why; collapse state still needs
 *                        to persist across a renderTree() rebuild,
 *                        which node ids themselves don't).
 */
let nextNodeId = 0;
let nodeParent = {};
let nodeDepth = {};
let hashToNodeIds = {};
let nodeToggle = {};
let nodeUl = {};
let nodeAnchor = {};
let nodeHash = {};
let nodePath = {};

/*
 * The tree's icons, taken from the brand's own shipped set (#232):
 * docs/brand/cix-ui-svg-icons/icons/<name>.svg, inlined verbatim.
 *
 * Inlined rather than linked because the dashboard ships no dependencies
 * and makes no extra requests for chrome -- the same constraint that
 * shaped everything else here. What changes is the artwork, not the
 * mechanism: these were hand-drawn approximations of the same concepts,
 * which is exactly the divergence this issue was filed about.
 *
 * Kept in the shipped 24x24 viewBox at the shipped 1.75 stroke-width
 * rather than redrawn to the old 16x16/1.3: the two scale to the same
 * rendered weight at 14px, and re-tracing the paths would recreate the
 * drift. Every icon is stroke-on-currentColor, so they inherit the
 * palette rather than carrying colour of their own.
 *
 * Names map to the manifest's own categories, which is what settles the
 * ones that could go either way: a recipe is `source` (resources,
 * alongside compiler and package), not `file`; stats is `metrics`
 * (observability); pki is `certificate`.
 */
const TREE_ICONS = {
	containers:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m12 3 8 4.5v9L12 21l-8-4.5v-9L12 3Z"/><path d="m4.5 7.8 7.5 4.1 7.5-4.1M12 12v9"/></svg>',
	networks:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="5" r="2"/><circle cx="5" cy="18" r="2"/><circle cx="19" cy="18" r="2"/><path d="m10.7 6.6-4.4 9M13.3 6.6l4.4 9M7 18h10"/></svg>',
	images:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="8" cy="9" r="1.5"/><path d="m4 17 5-5 3 3 2-2 6 5"/></svg>',
	packages:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m12 3 8 4-8 4-8-4 8-4Z"/><path d="m4 7v10l8 4 8-4V7M12 11v10"/></svg>',
	recipes:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9 7-5 5 5 5M15 7l5 5-5 5M14 4l-4 16"/></svg>',
	system:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="4" width="18" height="16" rx="2"/><path d="M7 8h10M7 12h5M7 16h8"/></svg>',
	pki:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M7 3h8l4 4v8a6 6 0 0 1-12 0V3Z"/><path d="M15 3v5h4M9 12h6M9 15h4"/></svg>',
	dns:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a15 15 0 0 1 0 18M12 3a15 15 0 0 0 0 18"/></svg>',
	backup:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 3h13l3 3v15H4z"/><path d="M8 3v6h8V3M8 21v-7h8v7"/></svg>',
	devices:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="5" y="3" width="14" height="18" rx="2"/><path d="M8 7h8M8 11h8M8 15h5"/><circle cx="16" cy="16" r="1"/></svg>',
	update:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 7h11l-3-3M20 17H9l3 3"/><path d="m15 4 3 3-3 3M9 14l-3 3 3 3"/></svg>',
	stats:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 20V10M10 20V4M16 20v-7M22 20V7"/></svg>',
	storage:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="3"/><path d="M12 3v3M21 12h-3M12 21v-3M3 12h3"/></svg>',
	/*
	 * A volume is a cylinder, deliberately NOT the storage platter.
	 * `storage` is a disk -- concentric circles around a spindle, a
	 * physical device -- and a volume sits INSIDE one of those, so
	 * drawing it with the same glyph would make the two
	 * indistinguishable at exactly the point the tree is trying to
	 * show the relationship (#329). The stacked drum is the long-
	 * standing convention for "a body of persistent data" and reads
	 * as a different kind of thing at 14px.
	 */
	volume:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><ellipse cx="12" cy="6" rx="7" ry="3"/><path d="M5 6v12c0 1.66 3.13 3 7 3s7-1.34 7-3V6"/><path d="M19 12c0 1.66-3.13 3-7 3s-7-1.34-7-3"/></svg>',
	services:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="5" cy="12" r="2"/><circle cx="19" cy="6" r="2"/><circle cx="19" cy="18" r="2"/><path d="M7 12h5a4 4 0 0 0 4-4V6M12 12a4 4 0 0 1 4 4v2"/></svg>',
	ldap:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="9" cy="8" r="3"/><circle cx="17" cy="9" r="2.5"/><path d="M3 20a6 6 0 0 1 12 0M14 15a5 5 0 0 1 7 5"/></svg>',
	ntp:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></svg>',
	dhcp:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="7" width="18" height="10" rx="2"/><path d="M7 12h10M9 9v6M15 9v6"/></svg>',
	syslog:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5 4h14v16H5z"/><path d="M8 8h8M8 12h8M8 16h5"/></svg>',
	host:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="4" y="3" width="16" height="18" rx="2"/><path d="M8 7h8M8 11h8M8 15h5"/><circle cx="16.5" cy="15" r=".75" fill="currentColor" stroke="none"/></svg>',
	config:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="3"/><path d="M19 13.5v-3l-2-.7a7 7 0 0 0-.7-1.7l.9-1.9-2.1-2.1-1.9.9a7 7 0 0 0-1.7-.7L10.5 2h-3l-.7 2a7 7 0 0 0-1.7.7l-1.9-.9-2.1 2.1.9 1.9a7 7 0 0 0-.7 1.7L0 10.5v3l2 .7a7 7 0 0 0 .7 1.7l-.9 1.9 2.1 2.1 1.9-.9a7 7 0 0 0 1.7.7l.7 2h3l.7-2a7 7 0 0 0 1.7-.7l1.9.9 2.1-2.1-.9-1.9a7 7 0 0 0 .7-1.7l2-.7Z" transform="translate(1.5 -0.5) scale(.875)"/></svg>',
	software:
		'<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m12 3 8 4-8 4-8-4 8-4Z"/><path d="m4 7v10l8 4 8-4V7M12 11v10"/></svg>',};

/* colorClass tints the icon itself (via CSS "color", which the icon's
 * own stroke="currentColor" inherits) instead of a separate status dot
 * next to it -- one visual signal, not two. */
function treeIcon(name, colorClass) {
	const span = document.createElement("span");

	span.className = "tree-icon" + (colorClass ? " " + colorClass : "");
	span.innerHTML = TREE_ICONS[name] || "";
	return span;
}

/*
 * The row's text, wrapped so it can be styled on its own.
 *
 * The active row is marked with a copper underline, matching the tab
 * bars. The row is a full-width flex anchor, so a border-bottom on it
 * would draw a rule right across the panel under a word a third as
 * wide; and text-decoration set on a flex container does not reach the
 * anonymous item its bare text node becomes. A real element for the
 * label is the one thing that can carry the underline.
 */
function treeLabel(text) {
	const span = document.createElement("span");

	span.className = "tree-label";
	span.textContent = text;
	return span;
}

function treeLink(href, text, className, icon, iconColorClass) {
	const a = document.createElement("a");

	a.href = href;
	a.className = className;
	if (icon)
		a.appendChild(treeIcon(icon, iconColorClass));
	a.appendChild(treeLabel(text));
	return a;
}

/* Containers only -- the only resource with a real running/paused/
 * stopped/exited state (Container.status per the API, ADR-0045: the
 * full real set is these four, not just running/paused/"other" --
 * "stopped" is a deliberately-stopped-but-still-defined container,
 * "exited" is its own process dying unexpectedly, a genuinely
 * different, worth-a-different-color condition). Tints the container
 * icon itself instead of a separate dot next to it: green = running,
 * yellow = paused (cgroup-frozen), grey = stopped (deliberate, no
 * error), red = exited (unexpected -- the one state actually worth
 * flagging at a glance). */
function containerStatusColorClass(status) {
	if (status === "running")
		return "tree-icon-ok";
	if (status === "paused")
		return "tree-icon-paused";
	if (status === "exited")
		return "tree-icon-error";
	return "tree-icon-idle"; /* stopped */
}

function treeItemLinkWithStatus(href, label, status, icon) {
	const a = document.createElement("a");

	a.href = href;
	a.className = "tree-item";
	if (icon)
		a.appendChild(treeIcon(icon, containerStatusColorClass(status)));
	a.appendChild(treeLabel(label));
	return a;
}

/*
 * Genuinely recursive -- a node can have children that themselves have
 * children (System > PKI > Root CA), not just one fixed level. Any
 * node with children gets its own toggle; a childless node is a plain
 * leaf link. Devices stays childless deliberately: its own individual
 * named-mapping "leaves" used to resolve to the exact same page as the
 * Devices category root itself (no scroll, no highlight, nothing to
 * show a leaf click had done anything), so it's a single flat link
 * rather than a fake-looking expandable one. Depth-based indent is set
 * directly (inline style) rather than via nested-ul CSS cascading, so
 * it stays correct and explicit regardless of how deep a given branch
 * goes.
 */
function buildTreeNode(item, parentUl, parentId, depth, parentPath) {
	const nodeId = nextNodeId++;
	const path = (parentPath !== undefined ? parentPath + "/" : "") + item.label;

	nodeParent[nodeId] = parentId;
	nodeDepth[nodeId] = depth;
	(hashToNodeIds[item.hash] || (hashToNodeIds[item.hash] = [])).push(nodeId);

	/* Collapse state is keyed by this label-built path, not by
	 * item.hash: several groups deliberately alias their own hash to a
	 * child's for ROUTING purposes (System/PKI/Root CA all share
	 * "pki-ca") so highlighting/auto-expand treat them as one
	 * identity -- correct there, since they really do route to related
	 * pages. But when BOTH sides of such an alias have their own
	 * children (System and PKI both do), they're two genuinely
	 * separate, independently-clickable toggles, and hash-keying
	 * collapse state made clicking either one collapse both (a real,
	 * reported bug). A path built from each ancestor's own label is
	 * unique per row regardless of hash aliasing, and doesn't need to
	 * survive across renders as an id would -- it's recomputed
	 * identically every render since it only depends on each static
	 * item's own label. */
	const li = document.createElement("li");
	const row = document.createElement("div");
	const hasChildren = item.children && item.children.length > 0;

	row.className = "tree-cat-row";
	row.style.marginLeft = depth + "rem";

	const toggle = document.createElement("button");

	toggle.type = "button";
	if (hasChildren) {
		const collapsed = collapsedCategories.has(path);

		toggle.className = "tree-toggle";
		toggle.textContent = collapsed ? "▸" : "▾";
		toggle.setAttribute("aria-label", "Toggle " + item.label);
	} else {
		toggle.className = "tree-toggle no-children";
		toggle.tabIndex = -1;
	}
	row.appendChild(toggle);

	const anchor =
		item.status !== undefined
			? treeItemLinkWithStatus("#" + item.hash, item.label, item.status, item.icon)
			: treeLink(
					"#" + item.hash,
					item.label,
					hasChildren ? "tree-category" : "tree-item",
					item.icon,
					item.iconColor
			  );

	row.appendChild(anchor);
	li.appendChild(row);
	nodeToggle[nodeId] = hasChildren ? toggle : null;
	nodeAnchor[nodeId] = anchor;
	nodeHash[nodeId] = item.hash;
	nodePath[nodeId] = path;

	if (hasChildren) {
		const ul = document.createElement("ul");

		ul.hidden = collapsedCategories.has(path);
		for (const child of item.children)
			buildTreeNode(child, ul, nodeId, depth + 1, path);
		li.appendChild(ul);
		nodeUl[nodeId] = ul;

		const setCollapsed = (nowCollapsed) => {
			ul.hidden = nowCollapsed;
			toggle.textContent = nowCollapsed ? "▸" : "▾";
			if (nowCollapsed)
				collapsedCategories.add(path);
			else
				collapsedCategories.delete(path);
			saveCollapsedCategories();
		};

		toggle.addEventListener("click", (event) => {
			event.preventDefault();
			setCollapsed(!ul.hidden);
		});

		/* Double-clicking the row itself opens/closes it too, not just
		 * the little chevron. The chevron is a ~14px target and it is
		 * the only way to expand a group without also navigating to it,
		 * which is a lot of precision to demand for the most common
		 * thing anyone does to a tree. Bound to the label rather than
		 * the whole row so the chevron keeps its own single-click
		 * meaning intact: two clicks on the chevron already toggle
		 * twice, and a dblclick handler sitting over the top of that
		 * would make the outcome depend on where in the row you
		 * happened to land.
		 *
		 * The first click of the double still navigates -- deliberately.
		 * The label IS a link, and swallowing its click to wait and see
		 * whether a second one arrives would put a delay on every
		 * single-click navigation in the tree to serve the rarer
		 * gesture. Navigating to a group you just expanded is what
		 * clicking it does anyway. */
		anchor.addEventListener("dblclick", (event) => {
			event.preventDefault();
			setCollapsed(!ul.hidden);
		});
	}
	parentUl.appendChild(li);
}

/*
 * Which disk or partition a volume's data actually sits on.
 *
 * Derived rather than stored, because the volume record cannot answer
 * it: `disk` is only set when an operator placed it explicitly, and the
 * common case (no disk set, default placement) would otherwise have
 * nowhere to hang. The volume's own resolved host_path plus each
 * device's mount_path is enough -- the device whose mount point is the
 * longest prefix of the path is the one holding it, which is the same
 * longest-match walk disk.c uses server-side to find the OS disk.
 *
 * Longest match matters: /var/lib/cix/volumes/x sits under both "/"
 * and "/var/lib/cix" when both are mounted, and only the second is
 * the true answer.
 */
function deviceHoldingVolume(v) {
	let best = null;

	if (!v.host_path)
		return null;
	for (const d of cache.storage) {
		if (!d.mounted || !d.mount_path)
			continue;
		const base = d.mount_path === "/" ? "/" : d.mount_path + "/";

		if (v.host_path === d.mount_path || v.host_path.startsWith(base)) {
			if (best === null || d.mount_path.length > best.mount_path.length)
				best = d;
		}
	}
	return best;
}

/*
 * One disk's subtree: its partitions, and any volumes living on it.
 * A volume hangs off whichever device actually holds it, so a volume on
 * a partition appears under that partition rather than under the disk
 * as a whole.
 */
/*
 * Volumes whose holding device cannot be derived -- its disk is not
 * mounted, or the path does not sit under any mount point this daemon
 * reports. They hang directly under Disks rather than under a device.
 *
 * This is the safety net for dropping the flat Volumes list: a volume
 * that the derivation cannot place would otherwise appear nowhere at
 * all, and a volume you cannot see is one you cannot delete or reason
 * about. Not duplication -- a volume is in exactly one place either
 * way, this is just where it goes when the better answer is unknown.
 */
function orphanVolumes() {
	return (cache.volumes || [])
		.filter((v) => deviceHoldingVolume(v) === null)
		.map((v) => ({
			label: v.name,
			hash: "volumes/" + encodeURIComponent(v.name),
			icon: "volume",
			iconColor: "tree-icon-idle",
		}));
}

function diskTreeChildren(diskName) {
	const children = [];

	for (const v of cache.volumes || []) {
		const holder = deviceHoldingVolume(v);

		if (holder !== null && holder.name === diskName)
			children.push({
				label: v.name,
				hash: "volumes/" + encodeURIComponent(v.name),
				icon: "volume",
				iconColor: "tree-icon-ok",
			});
	}
	for (const p of partitionsOf(diskName)) {
		children.push({
			label: p.name,
			hash: "storage/" + encodeURIComponent(p.name),
			icon: "storage",
			iconColor: p.mounted ? "tree-icon-ok" : "tree-icon-idle",
			children: (cache.volumes || [])
				.filter((v) => {
					const holder = deviceHoldingVolume(v);

					return holder !== null && holder.name === p.name;
				})
				.map((v) => ({
					label: v.name,
					hash: "volumes/" + encodeURIComponent(v.name),
					icon: "volume",
					iconColor: "tree-icon-ok",
				})),
		});
	}
	return children;
}

function renderTree() {
	treeEl.textContent = "";
	nextNodeId = 0;
	nodeParent = {};
	nodeDepth = {};
	hashToNodeIds = {};
	nodeToggle = {};
	nodeUl = {};
	nodeAnchor = {};
	nodeHash = {};
	nodePath = {};
	const root = document.createElement("ul");

	/*
	 * The tree names the five lifecycle domains of ADR-0230 (#182).
	 *
	 * It used to hide all of them behind one leaf. "Software" was a
	 * single destination covering recipes, packages, images, build
	 * configuration, the artifact cache, repo sync and update policy
	 * -- the whole of what makes this platform self-hosting -- while
	 * "Devices" got a leaf of its own. A navigation tree is a claim
	 * about what a system is, and that one claimed Cix was a container
	 * host that also had some software on it.
	 *
	 * Those seven pages are tabs of one view (CATEGORY_VIEWS maps them
	 * all to view-recipes), which is why collapsing them to one leaf
	 * was tempting and why expanding them costs nothing: every hash
	 * here is an existing route that already deep-links to its own tab.
	 * The page keeps its tabs; the tree stops pretending they are one
	 * subject.
	 *
	 * What is deliberately NOT re-expanded: the host configuration
	 * forms. Those were nine leaves of single forms and were collapsed
	 * into one tabbed destination for good reason -- that reasoning
	 * still holds, and undoing it is not what this change is for.
	 */
	/*
	 * ADR-0258 follow-on: ten pages, and a page's children are its own
	 * tabs.
	 *
	 * The tree used to list 56 destinations that resolved to 18 views,
	 * so two thirds of its leaves were tabs on somebody else's page --
	 * and one page, `view-daemon-config`, held sixteen unrelated tabs
	 * including Routes, which is networking. A navigation tree is a
	 * claim about what a system is; that one claimed this platform was
	 * a container host with a settings drawer bolted to it.
	 *
	 * Every hash below is a real route that already deep-links to its
	 * own tab, so nothing here invents a destination.
	 */
	/*
	 * Five pages and two groups. The rule, and it is the whole design:
	 *
	 *   TABS LIVE ON THE PAGE. THE TREE'S CHILDREN ARE THE LIVE THINGS.
	 *
	 * Clicking Storage opens the Storage page with all six of its tabs;
	 * the tree underneath lists this box's actual disks, partitions and
	 * volumes. The tree answers "what have I got", the tab bar answers
	 * "what can I do with it", and neither repeats the other.
	 *
	 * The two groups are the exception, and are marked as such: Services
	 * and System hold pages that are genuinely separate subjects, so
	 * folding them into one tabbed view would rebuild the sixteen-tab
	 * drawer this whole rework removed.
	 */
	const topLevel = [
		{
			label: "Containers",
			hash: "containers",
			icon: "containers",
			children: cache.containers.map((c) => ({
				label: c.name,
				hash: "containers/" + encodeURIComponent(c.name),
				icon: "containers",
				/* green running, yellow paused, grey stopped, red
				 * exited -- the state is the point of the list. */
				iconColor: containerStatusColorClass(c.status),
			})),
		},
		{
			label: "Networks",
			hash: "networks",
			icon: "networks",
			children: cache.networks.map((n) => ({
				label: n.name,
				hash: "networks/" + encodeURIComponent(n.name),
				icon: "networks",
				iconColor: n.up === false ? "tree-icon-error" : "tree-icon-ok",
			})),
		},
		{
			label: "Storage",
			hash: "storage",
			icon: "storage",
			/*
			 * Disks, their partitions nested under them, and each
			 * volume under the device that actually holds it --
			 * diskTreeChildren() derives that from the mount points
			 * (#329). This used to append the volumes flat onto the
			 * disk list, which put jump-home level with vda and sda
			 * and said nothing about which partition it consumes;
			 * the placement logic existed the whole time and was
			 * simply never called.
			 *
			 * orphanVolumes() is the tail: a volume whose holder
			 * cannot be derived hangs here rather than vanishing.
			 */
			children: cache.storage
				.filter((d) => !d.is_partition)
				.map((d) => ({
					label: d.name,
					hash: "storage/" + encodeURIComponent(d.name),
					icon: "storage",
					iconColor: d.mounted ? "tree-icon-ok" : "tree-icon-idle",
					children: diskTreeChildren(d.name),
				}))
				.concat(orphanVolumes()),
		},
		{
			/*
			 * The pipeline is a group because its children are its
			 * STAGES, not instances of a thing -- the one place in this
			 * tree where a child is a subject rather than something
			 * that exists on the box. That is deliberate: an operator
			 * asking "where did this stop?" is asking about the stage,
			 * so the stages are what the tree should offer.
			 *
			 * Selectable like every other group: clicking Software
			 * opens its own overview.
			 *
			 * #323, from the owner. It was briefly "Materializer",
			 * named for what it DOES rather than for what it holds,
			 * and the owner rejected that: every other entry in this
			 * tree is a noun for a thing you manage (Containers,
			 * Networks, Storage), and an agent-noun was the only one
			 * that read as a tool you launch. "Software" is what the
			 * icon has said since before either label existed.
			 *
			 * The children stay Catalogue/Build/Deployment rather
			 * than becoming agent-nouns for the same reason, and
			 * "Deployer" would additionally collide with the
			 * /deployments API resource, which is container recipes
			 * (ADR-0151) -- a different thing entirely.
			 *
			 * The hash stays "pipeline"; this is a label change, not
			 * a route change, and the pipeline model underneath keeps
			 * its name.
			 */
			label: "Software",
			group: true,
			hash: "pipeline",
			icon: "software",
			children: [
				{ label: "Catalogue", hash: "pkg-recipes", icon: "recipes" },
				{ label: "Build", hash: "build-overview", icon: "packages" },
				{ label: "Deployment", hash: "update", icon: "update" },
			],
		},
		{
			/*
			 * The group is selectable and lands on its own page --
			 * the health of every registered service, which is the
			 * overview for this group rather than a sibling of the
			 * five services it summarises.
			 */
			label: "Services",
			group: true,
			hash: "server-health",
			icon: "pki",
			children: [
				{ label: "PKI", hash: "pki-ca", icon: "pki" },
				{ label: "DNS", hash: "dns-records", icon: "dns" },
				{ label: "LDAP", hash: "ldap-servers", icon: "ldap" },
				{ label: "NTP", hash: "ntp-config", icon: "ntp" },
				{ label: "DHCP", hash: "dhcp-servers", icon: "dhcp" },
				{ label: "Syslog", hash: "syslog-targets", icon: "syslog" },
			],
		},
		{
			/*
			 * Selectable, like every other node: clicking Host opens
			 * the host's own page. Control Plane, Devices and Kernel
			 * are the machine's other subjects, not siblings of the
			 * host itself -- which is why there is no "Host > Host".
			 */
			label: "Host",
			group: true,
			hash: "host-stats",
			icon: "system",
			children: [
				{ label: "Control Plane", hash: "tls-throttle", icon: "system" },
				{ label: "Devices", hash: "devicemaps", icon: "devices" },
				{ label: "Kernel", hash: "kernel-policy", icon: "system" },
				{ label: "Bootloader", hash: "esp", icon: "system" },
			],
		},
	];




	for (const item of topLevel)
		buildTreeNode(item, root, undefined, 0);

	treeEl.appendChild(root);
	renderTreeActive();
}

/* Finds the single tree row that most specifically represents the
 * current route. Several rows can share one href (a category and its
 * own "representative" child both route to the same hash -- System,
 * PKI, and Root CA can all be "#pki-ca"), so route hash alone never
 * uniquely identifies "the thing actually being viewed" -- the
 * DEEPEST of the candidates sharing that hash always does, since a
 * more deeply nested row is always the more specific one. Falls back
 * to the route's own top-level segment when the exact route has no
 * tree row of its own at all (e.g. a specific image's detail page --
 * only "images" itself, Software's own child, is a real tree row). */
function findCurrentNodeId() {
	const current = location.hash.replace(/^#/, "") || "containers";
	const topSegment = current.split("/")[0];
	const candidates = hashToNodeIds[current] || hashToNodeIds[topSegment];

	if (!candidates || candidates.length === 0)
		return undefined;

	let best = candidates[0];

	for (const id of candidates)
		if (nodeDepth[id] > nodeDepth[best])
			best = id;
	return best;
}

/* Highlights the current row plus every one of its real structural
 * ancestors (walking nodeParent, never route-hash matching -- see
 * findCurrentNodeId()'s own comment for why hash matching alone would
 * incorrectly light up an unrelated sibling that merely happens to
 * share an address). */
/*
 * Two different facts, two different marks.
 *
 * `active` is "on the path to what you are looking at" -- every
 * ancestor of the current row, which is what keeps the branch you came
 * through legible. `current` is the one row you are actually on.
 *
 * They were one class until the active row gained a copper underline:
 * an underline means "you are here", and drawing it on Host as well as
 * Control Plane said it twice, about two different rows. Bold still
 * marks the path; the underline marks the destination.
 */
function renderTreeActive() {
	const owning = new Set();
	const current = findCurrentNodeId();

	for (let id = current; id !== undefined; id = nodeParent[id])
		owning.add(id);

	for (const [id, anchor] of Object.entries(nodeAnchor)) {
		anchor.classList.toggle("active", owning.has(Number(id)));
		anchor.classList.toggle("current", Number(id) === current);
	}
}

/* Only called on real navigation (hashchange + the very first render),
 * never from the periodic poll -- otherwise a category the user
 * deliberately collapsed while staying on one of its own pages would
 * silently snap back open every 2s. Expands every real structural
 * ancestor of the current row (System > PKI > Root CA needs System's
 * own toggle AND PKI's own toggle both expanded, not just the
 * immediate parent's) -- not the current row's own toggle, only what
 * needs opening to reveal it. */
function ensureActiveCategoryExpanded() {
	let changed = false;

	for (let id = nodeParent[findCurrentNodeId()]; id !== undefined; id = nodeParent[id]) {
		const toggle = nodeToggle[id];
		const ul = nodeUl[id];

		if (toggle && ul && ul.hidden) {
			ul.hidden = false;
			toggle.textContent = "▾";
			collapsedCategories.delete(nodePath[id]);
			changed = true;
		}
	}
	if (changed)
		saveCollapsedCategories();
}

/* ---------- Containers ---------- */

function renderContainers(containers) {
	const body = document.getElementById("containers-body");

	body.textContent = "";
	if (containers.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 5;
		cell.className = "empty";
		cell.textContent = "No containers";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const c of containers) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.appendChild(treeLink("#containers/" + encodeURIComponent(c.name), c.name, ""));
		row.appendChild(nameCell);

		const statusCell = document.createElement("td");
		const statusSpan = document.createElement("span");

		statusSpan.textContent = c.status;
		statusSpan.className =
			statusBadge(c.status === "running" ? "ok" : c.status === "paused" ? "paused" : "unknown");
		statusCell.appendChild(statusSpan);
		row.appendChild(statusCell);

		const imageCell = document.createElement("td");
		imageCell.textContent = c.image;
		row.appendChild(imageCell);

		const ipCell = document.createElement("td");
		ipCell.textContent =
			c.networks && c.networks.length > 0 ? c.networks.map((n) => n.name + ":" + n.ip).join(", ") : "-";
		row.appendChild(ipCell);

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeContainer(c.name));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

/*
 * #426: whether to show the platform's own build containers. Remembered
 * per browser, default off -- the owner asked for off by default, and
 * on a box with ten chain slots that is the difference between a dozen
 * rows and twenty-two.
 *
 * localStorage in a try/catch because a private window or blocked site
 * data makes the accessor itself throw, and the list must still render.
 */
function containersShowInternal() {
	const box = document.getElementById("containers-show-internal");

	if (box !== null)
		return box.checked;
	try {
		return storageGet("cix-containers-show-internal") === "1";
	} catch (e) {
		return false;
	}
}

async function refreshContainers() {
	const showInternal = containersShowInternal();
	/* The daemon filters, not this function -- so `cixctl container ls`
	 * sees exactly the same set and there is one definition of
	 * "internal" rather than two (#426). */
	const data = await apiRequest("GET", CIX_API.listContainers() +
	    (showInternal ? "?include_internal=1" : ""));
	const note = document.getElementById("containers-hidden-note");

	cache.containers = data.containers;
	/*
	 * Say how many were left out. An operator watching a build who
	 * cannot find __pkgbuild-0 should be told it is hidden, not left to
	 * conclude the container is gone -- which is the wrong conclusion
	 * to draw while debugging a build.
	 */
	if (note !== null) {
		const hidden = Number(data.hidden_internal) || 0;

		note.textContent = hidden > 0
		    ? hidden + (hidden === 1 ? " platform container hidden" : " platform containers hidden")
		    : "";
	}
	renderContainers(cache.containers);
}

async function removeContainer(name) {
	try {
		await apiRequest("DELETE", CIX_API.deleteContainer(name));
		clearStatus();
		if (onPageOf("containers") && parseHash().name === name)
			location.hash = "#containers";
		await refreshContainers();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove " + name + ": " + e.message, true);
	}
}

async function stopContainer(name) {
	try {
		await apiRequest("POST", CIX_API.stopContainer(name));
		clearStatus();
		await refreshContainers();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to stop " + name + ": " + e.message, true);
	}
}

async function startContainer(name) {
	try {
		await apiRequest("POST", CIX_API.startContainer(name));
		clearStatus();
		await refreshContainers();
		renderTree();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to start " + name + ": " + e.message, true);
	}
}

async function pauseContainer(name) {
	try {
		await apiRequest("POST", CIX_API.pauseContainer(name));
		clearStatus();
		await refreshContainers();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to pause " + name + ": " + e.message, true);
	}
}

async function unpauseContainer(name) {
	try {
		await apiRequest("POST", CIX_API.unpauseContainer(name));
		clearStatus();
		await refreshContainers();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to unpause " + name + ": " + e.message, true);
	}
}

/*
 * One labelled value, as a table row: <th> for the label, <td> for the
 * value. Every detail panel on every page is built from this one
 * helper -- 65 call sites across containers, networks, disks, zswap,
 * kernel policy, LDAP, packages, volumes and the rest -- so the panels
 * cannot drift apart from each other, and changing how a labelled
 * value looks is a change in exactly one place.
 *
 * A row per value rather than a responsive grid of cells: a grid
 * reflows into a different shape at every window width, which means
 * nothing sits where it did a moment ago and there is no line to
 * follow from a label to its value. Rows always read the same way, and
 * being rows they can be banded -- which is what makes a panel of
 * twenty values scannable rather than a wall.
 *
 * <th> and not a styled <td>, because that is what these are: the
 * label is the header for its row. Screen readers announce it as one.
 */
/*
 * ADR-0260: services. The create form takes one service per line,
 *   NAME=/path args | after=a,b | ready=tcp:22 | type=oneshot | on-exit=stop
 * -- the same shape cixctl's --service=/--after=/--ready=/--on-exit=
 * flags take, so an operator learns one notation. Returns the array
 * for the request body, or null after showing what was wrong.
 */
function parseServicesText(text) {
	const out = [];
	const lines = text.split("\n").map((l) => l.trim()).filter((l) => l.length > 0);

	for (const line of lines) {
		const parts = line.split("|").map((p) => p.trim());
		const eq = parts[0].indexOf("=");

		if (eq <= 0) {
			showStatus("Service line needs NAME=/path args: " + line, true);
			return null;
		}
		const svc = {
			name: parts[0].slice(0, eq).trim(),
			cmd: parts[0].slice(eq + 1).trim().split(/\s+/).filter((s) => s.length > 0),
		};
		if (svc.cmd.length === 0 || svc.cmd[0][0] !== "/") {
			showStatus("Service " + svc.name + " needs an absolute program path", true);
			return null;
		}
		for (const attr of parts.slice(1)) {
			const aeq = attr.indexOf("=");
			const key = aeq > 0 ? attr.slice(0, aeq).trim() : attr;
			const val = aeq > 0 ? attr.slice(aeq + 1).trim() : "";

			if (key === "after") {
				svc.after = val.split(",").map((s) => s.trim()).filter((s) => s.length > 0);
			} else if (key === "type") {
				svc.type = val;
			} else if (key === "on-exit") {
				svc.on_exit = val;
			} else if (key === "ready") {
				if (val.startsWith("tcp:"))
					svc.ready = { tcp_port: parseInt(val.slice(4), 10) };
				else if (val.startsWith("socket:"))
					svc.ready = { socket: val.slice(7) };
				else if (val.startsWith("command:"))
					svc.ready = { command: val.slice(8).split(/\s+/).filter((s) => s.length > 0) };
				else {
					showStatus("Service " + svc.name + ": ready wants tcp:PORT, socket:/path or command:/path", true);
					return null;
				}
			} else {
				showStatus("Service " + svc.name + ": unknown attribute '" + key + "'", true);
				return null;
			}
		}
		out.push(svc);
	}
	if (out.length === 0) {
		showStatus("A container declares at least one service", true);
		return null;
	}
	return out;
}

function summarizeServices(c) {
	const svcs = Array.isArray(c.services) ? c.services : [];

	if (svcs.length === 0)
		return "-";
	return svcs.map((s) => s.name + ":" + (s.state || "?")).join(", ");
}

function formatServiceExit(s) {
	if (!s.last_exit)
		return "-";
	if (s.last_exit.kind === "exited")
		return "exit " + s.last_exit.status;
	return s.last_exit.kind + " by signal " + s.last_exit.signal;
}

async function containerServiceAction(name, service, action) {
	const path = action === "start" ? CIX_API.startContainerService(name, service)
	           : action === "stop" ? CIX_API.stopContainerService(name, service)
	           : CIX_API.restartContainerService(name, service);

	try {
		await apiRequest("POST", path);
		clearStatus();
		await refreshContainers();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to " + action + " " + service + " in " + name + ": " + e.message, true);
	}
}

/* The services panel: declaration and live state per row, with the
 * three operations the API offers on one service. */
function renderServicesTable(c) {
	const tbody = document.querySelector("#cd-config-services tbody");
	const svcs = Array.isArray(c.services) ? c.services : [];

	tbody.textContent = "";
	if (svcs.length === 0) {
		const tr = document.createElement("tr");
		const td = document.createElement("td");

		td.colSpan = 8;
		td.textContent = "This container declares no service.";
		tr.appendChild(td);
		tbody.appendChild(tr);
		return;
	}
	for (const s of svcs) {
		const tr = document.createElement("tr");
		const cells = [s.name, s.type || "daemon", s.state || "?", s.pid ? String(s.pid) : "-",
		               String(s.restarts || 0), formatServiceExit(s), (s.cmd || []).join(" ")];

		for (const text of cells) {
			const td = document.createElement("td");

			td.textContent = text;
			tr.appendChild(td);
		}
		const actions = document.createElement("td");

		for (const action of ["start", "stop", "restart"]) {
			const b = document.createElement("button");

			b.type = "button";
			b.className = "secondary";
			b.textContent = action;
			b.disabled = c.status !== "running";
			b.addEventListener("click", () => containerServiceAction(c.name, s.name, action));
			actions.appendChild(b);
		}
		tr.appendChild(actions);
		tbody.appendChild(tr);
	}
}

function fieldBlock(label, value) {
	const row = document.createElement("tr");
	const labelCell = document.createElement("th");
	const valueCell = document.createElement("td");

	labelCell.className = "field-label";
	labelCell.scope = "row";
	labelCell.textContent = label;
	row.appendChild(labelCell);
	valueCell.textContent = value;
	row.appendChild(valueCell);
	return row;
}

function simpleTableRows(bodyEl, columns, colCount, emptyText) {
	bodyEl.textContent = "";
	if (columns.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = colCount;
		cell.className = "empty";
		cell.textContent = emptyText;
		row.appendChild(cell);
		bodyEl.appendChild(row);
		return;
	}
	for (const cols of columns) {
		const row = document.createElement("tr");

		for (const col of cols) {
			const cell = document.createElement("td");

			cell.textContent = col;
			row.appendChild(cell);
		}
		bodyEl.appendChild(row);
	}
}

let consoleWs = null;
let consoleTerminal = null;
let consoleContainerName = null;
let currentContainerDetailName = null;

/*
 * Which console session is the CURRENT one.
 *
 * A WebSocket does not stop delivering the moment close() is called:
 * frames already in flight still arrive, and onclose fires later still.
 * Every handler below used to write through the module-level
 * `consoleTerminal` and the shared status element -- so switching from
 * container A to container B left A's socket briefly alive while those
 * names already pointed at B, and A's remaining output was rendered
 * into B's terminal while A's onclose stamped "session ended" over B's
 * "connected".
 *
 * That is the whole of "the console doesn't switch when I change
 * container": it had switched, and was being written to by the previous
 * container.
 *
 * Each session takes a number on open. A handler that finds its number
 * is no longer the current one returns without touching anything.
 * Closing bumps the counter too, so a late frame after a plain close
 * has nothing to land on either.
 */
let consoleSession = 0;

function closeConsole() {
	{
		const detail = document.getElementById("view-container-detail");

		if (detail !== null)
			detail.classList.remove("console-fills");
	}
	consoleSession++;
	if (consoleWs !== null) {
		consoleWs.close();
		consoleWs = null;
	}
	if (consoleTerminal !== null) {
		/* The terminal owns real listeners and a ResizeObserver on the
		 * output element, which outlives every session -- without this
		 * they accumulate one set per console opened. */
		consoleTerminal.dispose();
	}
	consoleTerminal = null;
	consoleContainerName = null;
}

/*
 * Which of a container's declared consoles the operator is looking at
 * (issue #248). Held across re-renders so a poll-driven refresh does not
 * silently drop the selection back to the first one.
 */
let consoleSelected = null;

/*
 * Show what this container OFFERS as a console, and say plainly when it
 * offers nothing (issue #248).
 *
 * A container that declares no console used to get the same terminal
 * pane as any other, which then attached to a hardcoded /usr/bin/bash
 * and died instantly on an image without one. Presenting a control that
 * cannot work is worse than presenting none.
 */
function renderConsolePicker(c) {
	const pick = document.getElementById("cd-console-pick");
	const label = document.getElementById("cd-console-pick-label");
	const status = document.getElementById("cd-console-status");
	const output = document.getElementById("cd-console-output");
	const consoles = Array.isArray(c.consoles) ? c.consoles : [];

	if (consoles.length === 0) {
		pick.hidden = true;
		label.hidden = true;
		consoleSelected = null;
		/*
		 * No DECLARED console is not the same as no console. An
		 * explicit command still works on such a container -- the
		 * daemon says so -- so the terminal stays available and only
		 * the picker goes away. Hiding the pane outright, as this did
		 * before the Run box existed, would now be hiding a control
		 * that works.
		 */
		output.hidden = false;
		status.textContent = "This container declares no console -- use Run with an absolute path.";
		return;
	}
	output.hidden = false;

	/* Keep the operator's choice across the 2s detail-view re-render;
	 * fall back to the first only when the selection no longer exists. */
	if (!consoles.some((x) => x.name === consoleSelected))
		consoleSelected = consoles[0].name;

	/* One console is not a choice -- a select with a single option is
	 * noise, so only show the picker when there is something to pick. */
	const many = consoles.length > 1;
	pick.hidden = !many;
	label.hidden = !many;
	if (many) {
		const want = consoles.map((x) => x.name).join("\u0000");
		if (pick.dataset.names !== want) {
			pick.textContent = "";
			for (const entry of consoles) {
				const opt = document.createElement("option");

				opt.value = entry.name;
				opt.textContent = entry.name;
				pick.appendChild(opt);
			}
			pick.dataset.names = want;
		}
		pick.value = consoleSelected;
	}
}

/*
 * Switching console closes the current session and opens the chosen one.
 * Wired once: openConsole()'s own "already connected to this container"
 * guard would otherwise refuse to reconnect, so the close is what makes
 * the change take effect.
 */
/*
 * The popout button. Wired once, alongside the picker, and for the same
 * reason: this pane is re-rendered every two seconds by the poll loop,
 * so anything that adds a listener here must be idempotent or it
 * accumulates one per refresh.
 */
function wireConsolePopout() {
	const btn = document.getElementById("cd-console-popout");

	if (btn === null || btn.dataset.wired === "1")
		return;
	btn.dataset.wired = "1";
	btn.addEventListener("click", () => {
		const name = consoleContainerName || currentContainerDetailName;

		if (name === null)
			return;
		/*
		 * The container AND the chosen console both go in the URL, so
		 * the new window attaches to what this pane was showing rather
		 * than to whatever the container happens to declare first.
		 */
		let route = "#console-popout/" + encodeURIComponent(name);

		if (consoleSelected)
			route += "/" + encodeURIComponent(consoleSelected);
		/*
		 * A named window per container+console, so clicking twice
		 * raises the existing window instead of opening a second
		 * session against the same pty. The name is sanitised because
		 * a window name may not contain spaces or punctuation that the
		 * browser treats as a feature separator.
		 */
		const winName = ("cix-console-" + name + "-" + (consoleSelected || "default"))
			.replace(/[^A-Za-z0-9_-]/g, "_");

		window.open(location.pathname + route, winName,
		            "width=960,height=640,menubar=no,toolbar=no,location=no,status=no");
	});
}

function wireConsolePicker() {
	const pick = document.getElementById("cd-console-pick");

	wireConsolePopout();
	if (pick === null || pick.dataset.wired === "1")
		return;
	pick.dataset.wired = "1";
	pick.addEventListener("change", () => {
		const name = consoleContainerName;

		consoleSelected = pick.value;
		if (name !== null) {
			closeConsole();
			openConsole(name);
		}
	});

}

function openConsole(name) {
	wireConsolePicker();
	if (consoleContainerName === name && consoleWs !== null)
		return; /* already connected to this exact container -- a poll-driven
		         * re-render of the same detail view must never reopen this,
		         * or every keystroke/prompt would reset every 2s */

	closeConsole();
	{
		const detail = document.getElementById("view-container-detail");

		if (detail !== null)
			detail.classList.add("console-fills");
	}
	/* closeConsole() bumped the counter; this session claims the value
	 * it left behind, and every handler below compares against it. */
	const mySession = consoleSession;

	consoleContainerName = name;

	const outputEl = document.getElementById("cd-console-output");
	const statusEl = document.getElementById("cd-console-status");

	outputEl.textContent = "";
	statusEl.textContent = "connecting…";

	const encoder = new TextEncoder();

	/*
	 * The terminal is created before the socket, because its own size is
	 * what the URL below has to carry: ADR-0242 takes the geometry as a
	 * query parameter precisely because a browser's WebSocket
	 * constructor cannot set request headers, and there is no second
	 * chance to get it right at attach time -- ncurses reads the size
	 * once, at startup.
	 */
	consoleTerminal = createVT(outputEl, {
		onInput: (data) => {
			if (consoleWs !== null && consoleWs.readyState === WebSocket.OPEN)
				consoleWs.send(encoder.encode(data));
		},
		onResize: (c, r) => {
			/* ADR-0242's control channel: a TEXT frame is a message
			 * about the session, a BINARY frame is input. */
			if (consoleWs !== null && consoleWs.readyState === WebSocket.OPEN)
				consoleWs.send(JSON.stringify({ type: "resize", cols: c, rows: r }));
		}
	});
	consoleTerminal.fit();

	/* This session's own terminal, captured rather than read back
	 * through the module-level name: by the time a frame arrives that
	 * name may belong to a different container's session. */
	const term = consoleTerminal;

	const size = consoleTerminal.size();
	const params = [];

	/* #248: attach to the console the operator picked. Omitted entirely
	 * when nothing is picked, so the daemon applies its own "first
	 * declared" rule rather than this client duplicating it.
	 *
	 * A console NAME is the only thing that can be sent here (ADR-0261).
	 * This used to be the second arm of an if/else whose first arm sent
	 * a free-text cmd=, and removing that arm left the else behind --
	 * a syntax error that took the whole dashboard down. */
	if (consoleSelected)
		params.push("console=" + encodeURIComponent(consoleSelected));
	/* xterm-256color rather than anything more exotic: it is what this
	 * emulator actually implements, and naming a terminfo entry the
	 * renderer cannot honour would be a worse answer than naming a
	 * modest one it can. */
	params.push("term=xterm-256color");
	params.push("cols=" + size.cols);
	params.push("rows=" + size.rows);

	const proto = location.protocol === "https:" ? "wss:" : "ws:";
	const query = "?" + params.join("&");
	const ws = new WebSocket(proto + "//" + location.host + CIX_API.consoleContainer(name) + query);

	ws.binaryType = "arraybuffer";
	const decoder = new TextDecoder();

	ws.onopen = () => {
		if (mySession !== consoleSession)
			return;
		statusEl.textContent = "connected";
		/* preventScroll: the console pane fills the content area, and a
		 * plain focus() scrolls it into view -- carrying the tab bar and
		 * the container's own action buttons off the top of the page. */
		outputEl.focus({ preventScroll: true });
		/* Remeasure once the pane is definitely laid out. A fit taken
		 * while the tab was still hidden measures a zero-sized element,
		 * and the daemon would then hold a size the operator never
		 * had. */
		consoleTerminal.fit();
	};
	ws.onmessage = (event) => {
		const bytes = new Uint8Array(event.data);

		/* A frame from a session that has been superseded belongs to a
		 * container the operator has already navigated away from. It is
		 * dropped rather than rendered: writing it would put one
		 * container's output in another's terminal. */
		if (mySession !== consoleSession || term === null)
			return;
		/* stream: true matters -- a UTF-8 character can be split across
		 * two WebSocket frames, and decoding each frame independently
		 * turns a box-drawing glyph into two replacement characters. */
		term.feed(decoder.decode(bytes, { stream: true }));
	};
	ws.onclose = () => {
		if (mySession !== consoleSession)
			return; /* the previous container's socket closing, after
			         * this pane already belongs to another one */
		statusEl.textContent = "session ended";
	};
	ws.onerror = () => {
		if (mySession !== consoleSession)
			return;
		statusEl.textContent = "connection error";
	};

	consoleWs = ws;
}

/*
 * ---------- Container stats (ADR-0054) ----------
 * The daemon returns raw, point-in-time counters only (no rate, no
 * history) -- this module owns turning that into something graphable:
 * a small client-side rolling window kept only while the Stats tab is
 * open, and the CPU%/network-rate math (a delta between consecutive
 * raw samples divided by the real wall-clock time between them).
 */

const STATS_HISTORY_MAX = 60;

let statsTimer = null;
let statsContainerName = null;
let statsHistory = [];

/* cpu_max is the raw cgroup v2 "QUOTA PERIOD" pass-through text (see
 * ADR-0165) -- shown as a computed percentage-of-one-core alongside the
 * raw pair, since "50000 100000" means nothing to an operator at a
 * glance the way "50%" does. */
function formatCpuMax(cpuMax) {
	if (!cpuMax) return "-";
	const parts = cpuMax.split(" ");
	if (parts.length !== 2) return cpuMax;
	const quota = Number(parts[0]);
	const period = Number(parts[1]);
	if (!period || Number.isNaN(quota) || Number.isNaN(period)) return cpuMax;
	return ((quota / period) * 100).toFixed(0) + "% (" + cpuMax + ")";
}

function formatBytes(n) {
	const units = ["B", "KiB", "MiB", "GiB", "TiB"];
	let v = n;
	let i = 0;

	while (Math.abs(v) >= 1024 && i < units.length - 1) {
		v /= 1024;
		i++;
	}
	return v.toFixed(i === 0 ? 0 : 1) + " " + units[i];
}

/* "__host" is a reserved owner sentinel (daemon/src/main.c's
 * reissue_host_pki_cert()/reconcile_instance_dns_record(), ADR-0128) --
 * the daemon's own auto-issued host cert / auto-maintained instance
 * DNS record are never owned by a container, so a plain, real
 * container name would be misleading here. Rendered distinctly rather
 * than shown as the raw sentinel string. */
function formatOwner(owner) {
	if (!owner) return "-";
	if (owner === "__host") return "host (this daemon)";
	return owner;
}

/* "-1:58" for anything a minute or more old, "-12s" otherwise --
 * relative to the chart's own newest sample ("now"), not wall-clock
 * real time, since what matters here is how far back the window
 * reaches, not an absolute clock reading. */
function formatRelativeTime(deltaMs) {
	const totalSec = Math.round(Math.abs(deltaMs) / 1000);
	const m = Math.floor(totalSec / 60);
	const s = totalSec % 60;

	return m > 0 ? "-" + m + ":" + String(s).padStart(2, "0") : "-" + s + "s";
}

/*
 * Minimal hand-rolled line chart with real axes -- no external
 * dependency, same "build it ourselves" posture already established
 * for the console's own terminal renderer and WebSocket codec.
 *
 * series: array of {values, color}, all sharing one Y scale.
 * opts.times: epoch-ms per sample, same length/order as each series'
 * own values -- drives the X-axis (oldest sample vs. "now").
 * opts.formatY: number -> label string for the Y-axis ticks.
 * opts.maxY: fixes the Y scale's ceiling (e.g. 100 for a percentage,
 * or a container's own configured memory limit) instead of
 * autoscaling to the largest value currently on screen -- makes a
 * flat-looking metric (e.g. 2% CPU) still readable at its own real
 * scale, and shows real headroom against a known limit. Omit to
 * autoscale to max(all values currently plotted).
 */
/*
 * A data-series colour, resolved from the stylesheet so the palette has
 * one definition and follows the theme (#232). Canvas cannot take a
 * var(), which is why the old code inlined hex -- and why those hexes
 * drifted to Apple system colours nobody chose.
 */
function seriesColor(n) {
	const v = getComputedStyle(document.body).getPropertyValue("--series-" + n).trim();

	return v || "#5fa8d3";
}

function drawChart(canvas, series, opts) {
	const ctx = canvas.getContext("2d");
	const w = canvas.width;
	const h = canvas.height;
	const style = getComputedStyle(document.body);
	const textColor = style.getPropertyValue("--muted").trim() || "#888";
	const gridColor = style.getPropertyValue("--border").trim() || "#ccc";
	const marginBottom = 16;
	const marginTop = 6;
	const marginRight = 6;

	ctx.clearRect(0, 0, w, h);

	let maxV = opts.maxY || 0;

	if (!maxV) {
		for (const s of series)
			for (const v of s.values)
				if (v > maxV) maxV = v;
	}
	if (maxV <= 0) maxV = 1;

	ctx.font = "10px -apple-system, BlinkMacSystemFont, sans-serif";

	/*
	 * The left margin is measured, not assumed. A fixed 46px fitted
	 * "100%" and "5.1 KiB/s" and silently clipped the leading digit off
	 * anything wider -- which is how a per-network traffic chart came
	 * to label its top gridline "0.1 KiB/s" when the real value was
	 * 10.1: not a chart that looked cramped, a chart that read wrong by
	 * a factor of a hundred.
	 */
	const marginLeft = Math.max(
		...[0, 0.5, 1].map((frac) => ctx.measureText(opts.formatY(frac * maxV)).width)
	) + 9;
	const plotW = w - marginLeft - marginRight;
	const plotH = h - marginTop - marginBottom;
	ctx.strokeStyle = gridColor;
	ctx.fillStyle = textColor;
	ctx.lineWidth = 1;

	/* Y axis: gridlines + labels at 0/half/max. */
	for (const frac of [0, 0.5, 1]) {
		const y = marginTop + plotH - frac * plotH;

		ctx.beginPath();
		ctx.moveTo(marginLeft, Math.round(y) + 0.5);
		ctx.lineTo(marginLeft + plotW, Math.round(y) + 0.5);
		ctx.stroke();
		ctx.textAlign = "right";
		ctx.textBaseline = frac === 0 ? "bottom" : frac === 1 ? "top" : "middle";
		ctx.fillText(opts.formatY(frac * maxV), marginLeft - 5, y);
	}
	ctx.beginPath();
	ctx.moveTo(marginLeft + 0.5, marginTop);
	ctx.lineTo(marginLeft + 0.5, marginTop + plotH);
	ctx.stroke();

	/* X axis: how far back the window reaches vs. "now" -- only
	 * meaningful once there are at least two distinct sample times. */
	if (opts.times && opts.times.length >= 2) {
		const now = opts.times[opts.times.length - 1];

		ctx.textAlign = "left";
		ctx.textBaseline = "top";
		ctx.fillText(formatRelativeTime(opts.times[0] - now), marginLeft, marginTop + plotH + 3);
		ctx.textAlign = "right";
		ctx.fillText("now", marginLeft + plotW, marginTop + plotH + 3);
	}

	for (const s of series) {
		const vals = s.values;

		if (vals.length < 2)
			continue;
		ctx.strokeStyle = s.color;
		ctx.lineWidth = 1.5;
		ctx.beginPath();
		vals.forEach((v, i) => {
			const x = marginLeft + (i / (vals.length - 1)) * plotW;
			const y = marginTop + plotH - (v / maxV) * plotH;

			if (i === 0)
				ctx.moveTo(x, y);
			else
				ctx.lineTo(x, y);
		});
		ctx.stroke();
	}
}

function stopStatsPolling() {
	if (statsTimer !== null) {
		clearInterval(statsTimer);
		statsTimer = null;
	}
	statsContainerName = null;
	statsHistory = [];
}

function renderStatsCharts() {
	const h = statsHistory;

	if (h.length === 0)
		return;

	/* First sample alone has no prior point to diff a rate against --
	 * these charts (and their labels) simply stay at "…" until the
	 * second real poll tick lands. Rate series (CPU%, network) are one
	 * shorter than h itself; each rate's own timestamp is the *later*
	 * of the two raw samples it was computed from -- the point in time
	 * that delta had fully accumulated by. */
	const rateTimes = h.slice(1).map((s) => s.t);
	const gaugeTimes = h.map((s) => s.t);

	const cpuPercents = [];

	for (let i = 1; i < h.length; i++) {
		const dUsec = h[i].cpuUsageUsec - h[i - 1].cpuUsageUsec;
		const dMs = h[i].t - h[i - 1].t;

		cpuPercents.push(dMs > 0 ? Math.max(0, (dUsec / 1000 / dMs) * 100) : 0);
	}
	drawChart(document.getElementById("cd-stats-cpu"), [{ values: cpuPercents, color: seriesColor(1) }], {
		times: rateTimes,
		maxY: 100,
		formatY: (v) => v.toFixed(0) + "%",
	});
	document.getElementById("cd-stats-cpu-label").textContent =
		cpuPercents.length > 0 ? cpuPercents[cpuPercents.length - 1].toFixed(1) + "% (1 core = 100%)" : "…";

	const memValues = h.map((s) => s.memCurrent);
	const memMax = h[h.length - 1].memMax;

	drawChart(document.getElementById("cd-stats-mem"), [{ values: memValues, color: seriesColor(1) }], {
		times: gaugeTimes,
		maxY: memMax || 0,
		formatY: formatBytes,
	});
	{
		const last = h[h.length - 1];
		const limit = last.memMax === null ? "unlimited" : formatBytes(last.memMax);

		document.getElementById("cd-stats-mem-label").textContent =
			formatBytes(last.memCurrent) + " (limit " + limit + ")";
	}

	const diskValues = h.map((s) => s.diskBytes);

	drawChart(document.getElementById("cd-stats-disk"), [{ values: diskValues, color: seriesColor(1) }], {
		times: gaugeTimes,
		formatY: formatBytes,
	});
	document.getElementById("cd-stats-disk-label").textContent =
		formatBytes(h[h.length - 1].diskBytes) + " (overlay diff)";

	const rxRates = [];
	const txRates = [];

	for (let i = 1; i < h.length; i++) {
		const dMs = h[i].t - h[i - 1].t;
		const dRx = h[i].netRx - h[i - 1].netRx;
		const dTx = h[i].netTx - h[i - 1].netTx;

		rxRates.push(dMs > 0 ? Math.max(0, (dRx / dMs) * 1000) : 0);
		txRates.push(dMs > 0 ? Math.max(0, (dTx / dMs) * 1000) : 0);
	}
	drawChart(
		document.getElementById("cd-stats-net"),
		[
			{ values: rxRates, color: seriesColor(1) },
			{ values: txRates, color: seriesColor(1) },
		],
		{ times: rateTimes, formatY: (v) => formatBytes(v) + "/s" }
	);
	document.getElementById("cd-stats-net-label").textContent =
		rxRates.length > 0
			? formatBytes(rxRates[rxRates.length - 1]) + "/s ↓  " + formatBytes(txRates[txRates.length - 1]) + "/s ↑"
			: "…";

	/* PSI avg10 is already a 0-100 rolling percentage straight from the
	 * kernel (cpu.pressure/memory.pressure "some" line) -- a gauge like
	 * memory usage, not a counter to diff like cpuPercents above. */
	const cpuPressureValues = h.map((s) => s.cpuPressure);

	drawChart(document.getElementById("cd-stats-cpu-pressure"), [{ values: cpuPressureValues, color: seriesColor(1) }], {
		times: gaugeTimes,
		maxY: 100,
		formatY: (v) => v.toFixed(0) + "%",
	});
	document.getElementById("cd-stats-cpu-pressure-label").textContent =
		cpuPressureValues[cpuPressureValues.length - 1].toFixed(1) + "% (some, avg10)";

	const memPressureValues = h.map((s) => s.memPressure);

	drawChart(document.getElementById("cd-stats-mem-pressure"), [{ values: memPressureValues, color: seriesColor(1) }], {
		times: gaugeTimes,
		maxY: 100,
		formatY: (v) => v.toFixed(0) + "%",
	});
	document.getElementById("cd-stats-mem-pressure-label").textContent =
		memPressureValues[memPressureValues.length - 1].toFixed(1) + "% (some, avg10)";
}

async function pollStatsOnce(name) {
	let stats;

	try {
		stats = await apiRequest("GET", CIX_API.getContainerStats(name));
	} catch (e) {
		return; /* container may have just exited/been removed -- the next
		         * tick (or leaving the tab) resolves it; no need to
		         * surface a transient error here */
	}
	if (statsContainerName !== name)
		return; /* the tab moved on to a different container mid-request */

	const netRx = (stats.networks || []).reduce((sum, n) => sum + n.rx_bytes, 0);
	const netTx = (stats.networks || []).reduce((sum, n) => sum + n.tx_bytes, 0);

	statsHistory.push({
		t: Date.now(),
		cpuUsageUsec: stats.cpu.usage_usec,
		memCurrent: stats.memory.current,
		memMax: stats.memory.max,
		diskBytes: stats.disk.upper_bytes,
		netRx: netRx,
		netTx: netTx,
		cpuPressure: stats.cpu.pressure.some.avg10,
		memPressure: stats.memory.pressure.some.avg10,
		/* #279: the workload parent's own memory, which is a different
		 * question from the host's and was the one nobody could see.
		 * Absent on a host that has never started a workload. */
		workloadUsed: stats.workload_memory ? stats.workload_memory.usage_bytes : null,
		workloadLimit: stats.workload_memory ? stats.workload_memory.limit_bytes : null,
		workloadPressure: stats.workload_memory
			? stats.workload_memory.pressure.some.avg10
			: null,
	});
	if (statsHistory.length > STATS_HISTORY_MAX)
		statsHistory.shift();

	renderStatsCharts();
}

function startStatsPolling(name) {
	if (statsContainerName === name && statsTimer !== null)
		return; /* already polling this exact container -- a poll-driven
		         * re-render of the same detail view must not reset history */

	stopStatsPolling();
	statsContainerName = name;
	pollStatsOnce(name);
	statsTimer = setInterval(() => pollStatsOnce(name), POLL_INTERVAL_MS);
}

/*
 * ---------- Host stats (ADR-0073/ADR-0130) ----------
 * Same shape as container stats above (a client-side rolling window,
 * only kept while this page is open, drawChart() reused verbatim) --
 * GET /v1/system/stats is the host-wide counterpart, mirroring
 * GET /containers/{name}/stats' own "raw counters only, caller
 * computes rates" convention. Two real differences from the container
 * case: CPU here comes as cumulative /proc/stat jiffies (not a cgroup
 * usage_usec), needing the classic idle-delta/total-delta CPU% formula
 * instead of a straight usec/ms ratio; and memory/disk are already
 * gauges (current free/avail bytes), not something to diff against a
 * prior sample the way a cgroup's own cumulative usage counters are.
 */
const HOST_STATS_HISTORY_MAX = 60;

let hostStatsTimer = null;
let hostStatsHistory = [];

function stopHostStatsPolling() {
	if (hostStatsTimer !== null) {
		clearInterval(hostStatsTimer);
		hostStatsTimer = null;
	}
	hostStatsHistory = [];
}

function renderHostStatsCharts() {
	const h = hostStatsHistory;

	if (h.length === 0)
		return;

	const rateTimes = h.slice(1).map((s) => s.t);
	const gaugeTimes = h.map((s) => s.t);

	/* Classic /proc/stat CPU% -- idle (only, not iowait -- the same
	 * "busy" definition top(1) uses by default) as a fraction of total
	 * jiffies elapsed between two samples. */
	const cpuPercents = [];

	for (let i = 1; i < h.length; i++) {
		const dTotal = h[i].cpuTotal - h[i - 1].cpuTotal;
		const dIdle = h[i].cpuIdle - h[i - 1].cpuIdle;

		cpuPercents.push(dTotal > 0 ? Math.max(0, (1 - dIdle / dTotal) * 100) : 0);
	}
	drawChart(document.getElementById("hs-stats-cpu"), [{ values: cpuPercents, color: seriesColor(1) }], {
		times: rateTimes,
		maxY: 100,
		formatY: (v) => v.toFixed(0) + "%",
	});
	document.getElementById("hs-stats-cpu-label").textContent =
		cpuPercents.length > 0 ? cpuPercents[cpuPercents.length - 1].toFixed(1) + "%" : "…";

	const memValues = h.map((s) => s.memUsed);
	const memTotal = h[h.length - 1].memTotal;

	drawChart(document.getElementById("hs-stats-mem"), [{ values: memValues, color: seriesColor(1) }], {
		times: gaugeTimes,
		maxY: memTotal || 0,
		formatY: formatBytes,
	});
	/*
	 * Workload memory, charted against its own ceiling rather than the
	 * host's (#279).
	 *
	 * Scaling this to host total would hide exactly what it exists to
	 * show: a build pinned at a 2 GiB cgroup limit is a flat line near
	 * the bottom of an 8 GiB axis, which is what the host memory chart
	 * already looked like while that build was OOM-killing. Against its
	 * own limit it is a line at the top.
	 *
	 * With no limit set (-1) the axis falls back to host total, since
	 * an unlimited workload really is bounded by the machine.
	 */
	{
		const wcard = document.getElementById("hs-stats-workload-card");
		const last = h[h.length - 1];

		if (last.workloadUsed === null || last.workloadUsed === undefined) {
			wcard.hidden = true;
		} else {
			const wvalues = h.map((s) => (s.workloadUsed === null ? 0 : s.workloadUsed));
			const limited = last.workloadLimit !== null && last.workloadLimit >= 0;

			wcard.hidden = false;
			drawChart(document.getElementById("hs-stats-workload"),
			          [{ values: wvalues, color: seriesColor(2) }], {
				times: gaugeTimes,
				maxY: limited ? last.workloadLimit : (last.memTotal || 0),
				formatY: formatBytes,
			});
			document.getElementById("hs-stats-workload-label").textContent =
				formatBytes(last.workloadUsed) +
				(limited
					? " of " + formatBytes(last.workloadLimit) + " limit"
					: " (no limit set)") +
				(last.workloadPressure ? " — pressure " + last.workloadPressure.toFixed(1) + "%" : "");
		}
	}

	{
		const last = h[h.length - 1];

		document.getElementById("hs-stats-mem-label").textContent =
			formatBytes(last.memUsed) + " (total " + formatBytes(last.memTotal) + ")";
	}

	const diskValues = h.map((s) => s.diskUsed);
	const diskTotal = h[h.length - 1].diskTotal;

	drawChart(document.getElementById("hs-stats-disk"), [{ values: diskValues, color: seriesColor(1) }], {
		times: gaugeTimes,
		maxY: diskTotal || 0,
		formatY: formatBytes,
	});
	{
		const last = h[h.length - 1];

		document.getElementById("hs-stats-disk-label").textContent =
			formatBytes(last.diskUsed) + " (total " + formatBytes(last.diskTotal) + ")";
	}

	const rxRates = [];
	const txRates = [];

	for (let i = 1; i < h.length; i++) {
		const dMs = h[i].t - h[i - 1].t;
		const dRx = h[i].netRx - h[i - 1].netRx;
		const dTx = h[i].netTx - h[i - 1].netTx;

		rxRates.push(dMs > 0 ? Math.max(0, (dRx / dMs) * 1000) : 0);
		txRates.push(dMs > 0 ? Math.max(0, (dTx / dMs) * 1000) : 0);
	}
	drawChart(
		document.getElementById("hs-stats-net"),
		[
			{ values: rxRates, color: seriesColor(1) },
			{ values: txRates, color: seriesColor(1) },
		],
		{ times: rateTimes, formatY: (v) => formatBytes(v) + "/s" }
	);
	document.getElementById("hs-stats-net-label").textContent =
		rxRates.length > 0
			? formatBytes(rxRates[rxRates.length - 1]) + "/s ↓  " + formatBytes(txRates[txRates.length - 1]) + "/s ↑"
			: "…";

	const last = h[h.length - 1];

	document.getElementById("hs-stats-load").textContent =
		"Load average: " + last.load1.toFixed(2) + " (1m)  " + last.load5.toFixed(2) + " (5m)  " +
		last.load15.toFixed(2) + " (15m)";

	/* PSI avg10 is already a 0-100 rolling percentage straight from the
	 * kernel (cgroup-root cpu.pressure/memory.pressure "some" line) --
	 * a gauge, not a counter to diff. */
	const cpuPressureValues = h.map((s) => s.cpuPressure);

	drawChart(document.getElementById("hs-stats-cpu-pressure"), [{ values: cpuPressureValues, color: seriesColor(1) }], {
		times: gaugeTimes,
		maxY: 100,
		formatY: (v) => v.toFixed(0) + "%",
	});
	document.getElementById("hs-stats-cpu-pressure-label").textContent =
		cpuPressureValues[cpuPressureValues.length - 1].toFixed(1) + "% (some, avg10)";

	const memPressureValues = h.map((s) => s.memPressure);

	drawChart(document.getElementById("hs-stats-mem-pressure"), [{ values: memPressureValues, color: seriesColor(1) }], {
		times: gaugeTimes,
		maxY: 100,
		formatY: (v) => v.toFixed(0) + "%",
	});
	document.getElementById("hs-stats-mem-pressure-label").textContent =
		memPressureValues[memPressureValues.length - 1].toFixed(1) + "% (some, avg10)";
}

async function pollHostStatsOnce() {
	let stats;

	try {
		stats = await apiRequest("GET", CIX_API.getSystemStats());
	} catch (e) {
		return; /* transient -- the next tick resolves it */
	}
	if (hostStatsTimer === null)
		return; /* the page moved on before this request resolved */

	const netRx = (stats.networks || []).reduce((sum, n) => sum + n.rx_bytes, 0);
	const netTx = (stats.networks || []).reduce((sum, n) => sum + n.tx_bytes, 0);
	const c = stats.cpu;
	const cpuTotal = c.user_jiffies + c.nice_jiffies + c.system_jiffies + c.idle_jiffies +
	                  c.iowait_jiffies + c.irq_jiffies + c.softirq_jiffies + c.steal_jiffies;

	hostStatsHistory.push({
		t: Date.now(),
		cpuTotal: cpuTotal,
		cpuIdle: c.idle_jiffies,
		memUsed: stats.memory.total_bytes - stats.memory.available_bytes,
		memTotal: stats.memory.total_bytes,
		diskUsed: stats.disk.total_bytes - stats.disk.avail_bytes,
		diskTotal: stats.disk.total_bytes,
		netRx: netRx,
		netTx: netTx,
		load1: stats.load.load1,
		load5: stats.load.load5,
		load15: stats.load.load15,
		cpuPressure: stats.cpu.pressure.some.avg10,
		memPressure: stats.memory.pressure.some.avg10,
	});
	if (hostStatsHistory.length > HOST_STATS_HISTORY_MAX)
		hostStatsHistory.shift();

	renderHostStatsCharts();
}

function startHostStatsPolling() {
	if (hostStatsTimer !== null)
		return; /* already polling -- a poll-driven re-render of the same
		         * page must not reset history */

	pollHostStatsOnce();
	hostStatsTimer = setInterval(pollHostStatsOnce, POLL_INTERVAL_MS);
}

/*
 * ---------- Host processes (ADR-0131) ----------
 * Fetch-on-demand, not folded into the global 2s poll() loop the way
 * most category views are -- a real host's process table can be large
 * and churns constantly (pids come and go every fraction of a second),
 * so a full-table re-render every 2s would be visually noisy for a
 * view an operator opens to inspect a point-in-time snapshot, not
 * watch scroll by live. Same "no reason to keep re-polling while
 * nobody's looking" reasoning the old fetch-on-demand Logs page had.
 */
/*
 * Rebuild a panel only when its data actually changed.
 *
 * Every view renderer runs on the poll loop, and most of them cleared
 * their table and rebuilt it from scratch each time -- identical rows,
 * every couple of seconds. That is not free: it drops text selection,
 * resets any scroll inside the table, restarts the browser's own
 * hover state, and makes a page with a lot of rows visibly churn.
 * Processes and Server Health were the two where it made the page
 * unusable rather than merely busy.
 *
 * Comparing the DATA is the right test rather than diffing the DOM: it
 * is one JSON.stringify against a value we already have, and a page
 * whose data has not changed has nothing to say.
 */
const renderSignatures = Object.create(null);

function dataChanged(key, data) {
	const sig = JSON.stringify(data);

	if (renderSignatures[key] === sig)
		return false;
	renderSignatures[key] = sig;
	return true;
}

/*
 * True when a panel can be left exactly as it is: the payload is
 * identical to what it was last rendered from, AND it actually has
 * rows to keep (#432).
 *
 * Both halves matter. Without the payload check a refresh tick blanks
 * and rebuilds a table that has not changed, which costs the reader
 * their scroll position -- the page shrinks to a fraction of its
 * height while the tables are empty, the browser clamps scrollTop to
 * the new maximum, and the rebuilt-taller table cannot give the offset
 * back. Opening the build log made that obvious because the log box is
 * tall, but every table on a polled page was doing it.
 *
 * Without the has-rows check the guard has the opposite failure: a
 * panel whose DOM is empty but whose payload matches a previous render
 * would be skipped and stay empty. That is the same distinction
 * showLoadingIfEmpty() below already draws, for the same reason.
 *
 * dataChanged() records the signature as a side effect, so this must
 * be called once per render attempt and its answer acted on.
 */
function unchangedAndRendered(tbody, key, data)
{
	const changed = dataChanged(key, data);

	return !changed && tbody !== null && tbody.querySelector("tr") !== null;
}

/*
 * "Loading" belongs to a panel that has never had content, not to
 * every refresh of one that has. Blanking a populated table before a
 * fetch is what made Processes flash empty twice a second.
 */
function showLoadingIfEmpty(tbody, colspan) {
	if (tbody.querySelector("tr") !== null)
		return;
	tbody.innerHTML = '<tr><td colspan="' + colspan +
	                  '" class="empty">Loading&hellip;</td></tr>';
}

async function refreshProcesses() {
	const tbody = document.getElementById("processes-body");

	showLoadingIfEmpty(tbody, 7);
	try {
		const procs = await apiRequest("GET", CIX_API.listSystemProcesses());

		if (dataChanged("processes", procs))
			renderProcessesTable(procs);
	} catch (e) {
		if (tbody.querySelector("tr td.empty") !== null || tbody.querySelector("tr") === null)
			tbody.innerHTML = '<tr><td colspan="7" class="empty">Failed to load.</td></tr>';
	}
}

function renderProcessesTable(procs) {
	const tbody = document.getElementById("processes-body");

	tbody.textContent = "";
	if (!procs || procs.length === 0) {
		tbody.innerHTML = '<tr><td colspan="7" class="empty">No processes.</td></tr>';
		return;
	}
	for (const p of procs) {
		const row = document.createElement("tr");
		const pidCell = document.createElement("td");
		const ppidCell = document.createElement("td");
		const uidCell = document.createElement("td");
		const gidCell = document.createElement("td");
		const containerCell = document.createElement("td");
		const cmdCell = document.createElement("td");
		const actionCell = document.createElement("td");
		const killButton = document.createElement("button");

		pidCell.textContent = p.pid;
		ppidCell.textContent = p.ppid;
		uidCell.textContent = p.user_id;
		gidCell.textContent = p.group_id;
		if (p.container) {
			const link = document.createElement("a");

			link.href = "#containers/" + encodeURIComponent(p.container);
			link.textContent = p.container;
			containerCell.appendChild(link);
		} else {
			containerCell.textContent = "-";
		}
		cmdCell.textContent = p.command_line;
		cmdCell.className = "processes-cmdline";
		/* The cell shows one line; the whole command is still here. */
		cmdCell.title = p.command_line;

		killButton.textContent = "Kill";
		killButton.className = "button-danger button-small";
		killButton.addEventListener("click", () => killProcess(p.pid, p.command_line));
		actionCell.appendChild(killButton);

		row.append(pidCell, ppidCell, uidCell, gidCell, containerCell, cmdCell, actionCell);
		tbody.appendChild(row);
	}
}

async function killProcess(pid, cmdline) {
	if (!confirm("Kill pid " + pid + " (" + cmdline + ")? This is an immediate SIGKILL, no confirmation from the process itself."))
		return;
	try {
		await apiRequest("DELETE", CIX_API.killSystemProcess(pid));
		clearStatus();
		showStatus("Killed pid " + pid, false);
		await refreshProcesses();
	} catch (e) {
		showStatus("Failed to kill pid " + pid + ": " + e.message, true);
	}
}

function renderProcessesList() {
	refreshProcesses();
}

document.getElementById("proc-refresh").addEventListener("click", () => {
	refreshProcesses();
});

for (const tabButton of document.querySelectorAll(".tab-bar .tab-button")) {
	tabButton.addEventListener("click", () => {
		const tabBar = tabButton.parentElement;
		const tabName = tabButton.dataset.tab;

		for (const btn of tabBar.querySelectorAll(".tab-button"))
			btn.classList.toggle("active", btn === tabButton);
		/* Only this bar's OWN panels. querySelectorAll would also match
		 * panels belonging to a nested tab bar further down, so an outer
		 * tab click would hide the inner page's content -- which is
		 * exactly what happens once one tabbed page contains another. */
		for (const panel of tabBar.parentElement.querySelectorAll(":scope > .tab-panel"))
			panel.hidden = panel.dataset.tab !== tabName;

		/*
		 * Leaving the Console tab ends the session immediately rather
		 * than on the next poll. Two reasons, and the second is the one
		 * that shows: a pty held open behind a hidden tab is a process
		 * in the container nobody is watching, and closeConsole() is
		 * also what drops the "console-fills" class -- without this the
		 * detail view stayed a full-height flex column for up to two
		 * seconds while another tab's content was already in it.
		 */
		if (tabBar.parentElement.id === "view-container-detail" && tabName !== "console" &&
		    consoleContainerName !== null)
			closeConsole();

		/*
		 * Every tab on a collapsed page is named after the address it
		 * replaced, so a tab that IS an address navigates to it rather
		 * than only revealing its panel. Without this the panel came up
		 * holding whatever it was built with -- "Loading…", for a page
		 * whose render only ever ran on arrival at its own old address
		 * (confirmed: Host > Routes showed Loading… indefinitely). It
		 * also makes each tab bookmarkable, which is what those
		 * addresses were for in the first place.
		 *
		 * Tabs that are not addresses -- a container detail's own
		 * Summary/Hardware/Options/Console, the Catalogue's cat-* tabs
		 * (that page renders all of its content for any of its own
		 * addresses) -- are left alone.
		 */
		/*
		 * A container detail's tabs are per-container panels, never
		 * addresses, so they are exempt wholesale rather than by name
		 * (#425). "packages" collides with the top-level #packages
		 * route, so the rule above rewrote the location and the router
		 * obliged -- clicking Packages on a container landed on the
		 * global Packages view, and loadContainerPackages() right
		 * below never got the chance to matter. Exempting the tab bar
		 * rather than blacklisting the word is what stops the next
		 * tab whose name happens to be a route from doing it again.
		 */
		if (tabBar.parentElement.id !== "view-container-detail" &&
		    tabButtonFor(tabName) !== null && parseHash().category !== tabName)
			location.hash = "#" + tabName;
		if (tabName === "console") {
			/*
			 * Connect HERE rather than when the container view rendered.
			 * The console panel is hidden until this click, and a hidden
			 * element has no layout -- so a terminal fitted before it
			 * measures nothing and falls back to 80x24, and the size in
			 * the upgrade URL is then a number the pane never had.
			 * ADR-0242 exists because a program reads its size once at
			 * startup; correcting it a moment later with a resize
			 * message is exactly the "size the program already missed"
			 * this platform just finished fixing one layer down.
			 *
			 * Connecting on tab open also stops a pty being spawned in
			 * every container an operator merely glances at.
			 */
			if (currentContainerDetailName !== null)
				openConsole(currentContainerDetailName);
			document.getElementById("cd-console-output").focus({ preventScroll: true });
		}
		/*
		 * The recipe is fetched on tab open, not on every detail
		 * render. The detail view re-renders on a two-second poll and a
		 * recipe does not change under a running container, so fetching
		 * it there would be a request every two seconds for identical
		 * text -- on the same event loop this project has twice had to
		 * rescue from polled endpoints.
		 */
		/* Fetch for the page this tab belongs to right away -- waiting
		 * for the next poll is what makes a freshly-clicked tab read
		 * as "Loading" for a couple of seconds. */
		{
			const owner = tabBar.closest(".view");

			if (owner !== null)
				runRefreshers(refreshersForView(owner.id));
		}
		if (tabName === "config" && currentContainerDetailName !== null)
			loadContainerRecipe(currentContainerDetailName);
		/* #398: same on-open-only rule as the recipe above -- a
		 * version's manifest is immutable, so re-fetching it on the
		 * two-second detail poll would be a request every two seconds
		 * for bytes that cannot have changed. */
		if (tabName === "packages" && currentContainerDetailName !== null)
			loadContainerPackages(currentContainerDetailName);
		if (tabButton.id === "cd-tab-summary" && currentContainerDetailName !== null)
			startStatsPolling(currentContainerDetailName);
		else if (statsContainerName !== null)
			stopStatsPolling();
	});
}

/* Issue #78: render exit_status together with term_signal so a signal death
   is never shown as (or mistaken for) a plain exit code of the same number. */
function formatExitStatus(c) {
	if (c.exit_status === null || c.exit_status === undefined)
		return "-";
	if (c.term_signal)
		return "killed by signal " + c.term_signal;
	return "exit code " + c.exit_status;
}

/*
 * The container's own recipe, for the Configuration tab.
 *
 * Shown because the recipe and the live definition above it are
 * different things that drift apart: the recipe is the SOURCE, what
 * someone wrote and applied, while the definition is what the
 * container actually became. `jump` declared linux-pam 1.6.1-2 in its
 * image manifest while running 1.6.1-6, because an ad-hoc install
 * moves the package and leaves the declaration behind, and nothing
 * anywhere showed the disagreement.
 *
 * A container created straight through POST /v1/containers has no
 * recipe at all. That is a real answer about how it came to exist, not
 * an error, and it is said in those words -- which is why apiRequest()
 * now carries the HTTP status on the errors it throws.
 *
 * Fetched through loadContainerRecipeContent(), the same accessor the
 * recipes page uses, rather than a second copy of the request: it also
 * caches by name, so switching tabs back and forth costs nothing.
 *
 * The text goes into a <pre> via textContent. The daemon returns it
 * UNSUBSTITUTED, so {{SECRET:...}} and {{LDAP:BIND_PASSWORD}} appear as
 * their tokens and no credential reaches this page.
 */
async function loadContainerRecipe(name) {
	const statusEl = document.getElementById("cd-config-recipe-status");
	const textEl = document.getElementById("cd-config-recipe");

	if (statusEl === null || textEl === null)
		return;
	statusEl.textContent = "Loading\u2026";
	textEl.hidden = true;
	try {
		const content = await loadContainerRecipeContent(name);

		if (!content) {
			statusEl.textContent = "This container has a recipe, but it is empty.";
			return;
		}
		statusEl.textContent =
			"Applied from this recipe. Secret tokens are shown unsubstituted \u2014 no value reaches this page.";
		textEl.textContent = content;
		textEl.hidden = false;
	} catch (err) {
		statusEl.textContent = err && err.status === 404
			? "No recipe \u2014 this container was created directly through the API, not applied from one."
			: "Could not load the recipe: " + (err && err.message ? err.message : "unknown error");
	}
}

/*
 * #398: what this container's image version holds.
 *
 * Read from GET /v1/images/{name}/versions/{version}/manifest, keyed on
 * the container's own pinned image_version -- never the image's current
 * declared manifest. Those diverge two ways: the declaration moves
 * whenever an operator edits it, and a `rolling` entry's version there
 * is a floor rather than a fact. Rendering either as "this container's
 * packages" would be wrong in exactly the way that is hardest to
 * notice: a confident, plausible, up-to-date-looking list.
 *
 * What it still cannot say is whether the bytes REACHED this container.
 * An installed record is not a filesystem reading, and that distinction
 * cost a full debugging round in #395 -- fastfetch read `installed`,
 * the versions matched, and the binary was not in the container at all
 * -- so the page points at the console for that question rather than
 * implying it answers it.
 */
async function loadContainerPackages(name) {
	const statusEl = document.getElementById("cd-packages-status");
	const tbody = document.querySelector("#cd-packages tbody");
	const c = cache.containers.find((x) => x.name === name);

	if (statusEl === null || tbody === null)
		return;
	tbody.textContent = "";
	if (!c || !c.image) {
		statusEl.textContent = "This container has no image.";
		return;
	}
	if (!c.image_version) {
		statusEl.textContent =
			"This container records no image version, so there is no manifest to read. It has "
			+ "not been started yet, or predates pinned image versions.";
		return;
	}
	{
		const link = document.getElementById("cd-packages-console-link");

		if (link !== null)
			link.href = "#containers/" + encodeURIComponent(name);
	}
	statusEl.textContent = "Loading\u2026";
	try {
		const d = await apiRequest("GET",
		                           CIX_API.getImageVersionManifest(c.image, c.image_version));
		const rows = Array.isArray(d.manifest) ? d.manifest : [];

		statusEl.textContent = "Installed into image " + c.image + " at version "
			+ c.image_version + " \u2014 the version this container is pinned to and runs from.";
		if (rows.length === 0) {
			const tr = document.createElement("tr");
			const td = document.createElement("td");

			td.colSpan = 2;
			td.textContent = "This image version holds no packages.";
			tr.appendChild(td);
			tbody.appendChild(tr);
			return;
		}
		rows.forEach((r) => {
			const tr = document.createElement("tr");

			[r.package, r.version].forEach((v) => {
				const td = document.createElement("td");

				td.textContent = v === undefined || v === null ? "-" : String(v);
				tr.appendChild(td);
			});
			tbody.appendChild(tr);
		});
	} catch (err) {
		statusEl.textContent = err && err.status === 404
			? "No manifest was recorded for version " + c.image_version + ". Versions produced "
				+ "before per-version manifests have none, and the image's CURRENT manifest is "
				+ "deliberately not shown in its place \u2014 it would describe what the image "
				+ "holds now, which is a different question."
			: "Could not load the manifest: " + (err && err.message ? err.message : "unknown error");
	}
}

function renderContainerDetail(name) {
	const c = cache.containers.find((x) => x.name === name);
	const fields = document.getElementById("cd-fields");

	currentContainerDetailName = name;

	if (!c) {
		fields.textContent = name + " \u2014 not found.";
		closeConsole();
		stopStatsPolling();
		return;
	}

	renderConsolePicker(c);
	{
		/* Only when the Console tab is actually the one on screen --
		 * see the tab handler's own note. A re-render while the tab IS
		 * open must still reconnect, which is why this is a visibility
		 * test rather than a one-shot. */
		const panel = document.getElementById("cd-panel-console");
		const visible = panel !== null && !panel.hidden;

		if (visible && Array.isArray(c.consoles) && c.consoles.length > 0)
			openConsole(name);
		else
			closeConsole();
	}
	/* Keep stats polling pointed at whatever container this view is
	 * actually showing right now: without this, switching containers
	 * while the Summary tab (which now carries the live stats charts
	 * directly, ADR-0138) stays the active one left the poll loop
	 * silently stuck on the previously-viewed container's own name
	 * (startStatsPolling()/stopStatsPolling() otherwise only ever fire
	 * from an explicit tab click) -- the graphs looked "stuck" because
	 * they genuinely were still polling someone else's stats. */
	if (document.getElementById("cd-tab-summary").classList.contains("active"))
		startStatsPolling(name);
	else if (statsContainerName !== null)
		stopStatsPolling();

	/* Summary -- identity/runtime status only. */
	fields.textContent = "";
	fields.appendChild(fieldBlock("Status", c.status));
	fields.appendChild(fieldBlock("Image", c.image));
	fields.appendChild(fieldBlock("PID", c.pid === null || c.pid === undefined ? "-" : String(c.pid)));
	/*
	 * Issue #78/#77: exit_status alone is ambiguous -- a container killed by
	 * a signal reports that SIGNAL NUMBER here, indistinguishable from a
	 * real exit code of the same value (a stop/delete SIGKILL shows 9, which
	 * is not "exit code 9"). term_signal is what disambiguates it, so say
	 * which one this actually is rather than printing a bare number.
	 */
	fields.appendChild(fieldBlock("Exit status", formatExitStatus(c)));
	if (c.exit_reason)
		fields.appendChild(fieldBlock("Exit reason", c.exit_reason));
	fields.appendChild(fieldBlock("Services", summarizeServices(c)));
	fields.appendChild(fieldBlock("Ready", c.ready ? "yes" : "no"));

	/* Resource limits -- moved onto the Hardware tab (issue #69,
	 * user-requested): the full resource envelope lives with the rest
	 * of the hardware-shaped facts (devices/interfaces/networks), not
	 * scattered on the overview. Read live from the real cgroup by the
	 * daemon (ADR-0165); cpuset/disk-quota read-back landed with issue
	 * #49 (they were write-only before -- combined with silent
	 * unknown-field dropping, #68, a typo'd limit was undetectable). */
	{
		const hw = document.getElementById("cd-hardware-resources");

		hw.textContent = "";
		hw.appendChild(
			fieldBlock("Memory limit", c.memory_max === null || c.memory_max === undefined ? "unlimited" : formatBytes(c.memory_max))
		);
		hw.appendChild(fieldBlock("CPU limit", formatCpuMax(c.cpu_max) === "-" ? "unlimited" : formatCpuMax(c.cpu_max)));
		hw.appendChild(
			fieldBlock("Process limit", c.pids_max === null || c.pids_max === undefined ? "unlimited" : String(c.pids_max))
		);
		hw.appendChild(
			fieldBlock("CPU pinning", c.cpuset_cpus === null || c.cpuset_cpus === undefined ? "unrestricted" : "cpus " + c.cpuset_cpus)
		);
		hw.appendChild(
			fieldBlock("Disk quota", c.disk_quota_bytes === null || c.disk_quota_bytes === undefined ? "unlimited" : formatBytes(c.disk_quota_bytes))
		);
		hw.appendChild(
			fieldBlock("Extra capabilities", (c.cap_add && c.cap_add.length > 0) ? c.cap_add.join(", ") : "none")
		);
	}

	/* Hardware -- devices/interfaces/network attachments granted at creation.
	 * Links back to Devices when the granted id has a real "exact" name
	 * mapping ("vendor_model" isn't shown here either, same "only exact
	 * pins to one row" convention renderDevices() already established). */
	{
		const devicesBody = document.querySelector("#cd-devices tbody");
		const devices = c.devices || [];
		const mappedNames = exactMappedNames();

		devicesBody.textContent = "";
		if (devices.length === 0) {
			const row = document.createElement("tr");
			const cell = document.createElement("td");

			cell.colSpan = 2;
			cell.className = "empty";
			cell.textContent = "No devices granted";
			row.appendChild(cell);
			devicesBody.appendChild(row);
		} else {
			for (const d of devices) {
				const row = document.createElement("tr");
				const idCell = document.createElement("td");
				const pathCell = document.createElement("td");

				if (mappedNames[d.id])
					idCell.appendChild(treeLink("#devices", d.id + " (" + mappedNames[d.id] + ")", ""));
				else
					idCell.textContent = d.id;
				pathCell.textContent = d.dev_path;
				row.appendChild(idCell);
				row.appendChild(pathCell);
				devicesBody.appendChild(row);
			}
		}
	}
	simpleTableRows(
		document.querySelector("#cd-interfaces tbody"),
		(c.interfaces || []).map((ifname) => [ifname]),
		1,
		"No host interfaces attached"
	);
	simpleTableRows(
		document.querySelector("#cd-networks tbody"),
		(c.networks || []).map((n) => [n.name, n.ip]),
		2,
		"Not attached to any network"
	);

	/* Volumes (issue #88). Deliberately shows the volume NAME as a link
	 * rather than the resolved host path: the name is the thing an
	 * operator acts on, and the link matters because a volume is not
	 * owned by this container -- it can outlive it and be mounted
	 * elsewhere, which the Volumes page is where you actually see. */
	{
		const volBody = document.querySelector("#cd-volumes tbody");
		const vols = c.volumes || [];

		volBody.textContent = "";
		if (vols.length === 0) {
			const row = document.createElement("tr");
			const cell = document.createElement("td");

			cell.colSpan = 4;
			cell.className = "empty";
			cell.textContent = "No volumes -- everything this container writes is lost when it is deleted";
			row.appendChild(cell);
			volBody.appendChild(row);
		} else {
			for (const v of vols) {
				const row = document.createElement("tr");
				const nameCell = document.createElement("td");
				const pathCell = document.createElement("td");
				const modeCell = document.createElement("td");

				nameCell.appendChild(treeLink("#volumes/" + encodeURIComponent(v.name), v.name, ""));
				pathCell.textContent = v.path;
				modeCell.textContent = v.read_only ? "read-only" : "read-write";
				const backupCell = document.createElement("td");

				/*
				 * Issue #96: a container shows the backups of the
				 * volumes it mounts, derived rather than stored. The
				 * policy belongs to the volume -- two containers can
				 * mount the same one, and hanging it on the container
				 * would mean two policies over one set of bytes -- so
				 * this reads the volume's own answer and links to it.
				 */
				fillVolumeBackupCell(v.name, backupCell);

				row.appendChild(nameCell);
				row.appendChild(pathCell);
				row.appendChild(modeCell);
				row.appendChild(backupCell);
				volBody.appendChild(row);
			}
		}
	}

	/*
	 * Configuration -- what this container IS, as opposed to what it is
	 * doing.
	 *
	 * Only the fields no other tab shows. Summary, Hardware and Options
	 * between them already render most of the definition, and repeating
	 * any of it here would create a second place to read the same fact
	 * and a second place for it to go stale.
	 *
	 * `cmd` is the one that mattered: the command the container
	 * actually runs was not visible anywhere in this dashboard.
	 */
	{
		const configFields = document.getElementById("cd-config-fields");

		configFields.textContent = "";
		configFields.appendChild(
			fieldBlock("Added capabilities",
			           Array.isArray(c.cap_add) && c.cap_add.length > 0 ? c.cap_add.join(", ")
			                                                           : "none (the default set)"));
		configFields.appendChild(fieldBlock("User namespace", c.userns ? "yes" : "no"));
		configFields.appendChild(fieldBlock("Capture output", c.captured_output ? "yes" : "no"));
		configFields.appendChild(fieldBlock("Image", c.image || "?"));

		renderServicesTable(c);

		/* Its own table, like env/sysctls/files: fieldBlock renders with
		 * textContent, so a newline-joined list would collapse to one
		 * run-on line in HTML. */
		simpleTableRows(
			document.querySelector("#cd-config-consoles tbody"),
			(c.consoles || []).map((x) => [x.name, (x.cmd || []).join(" ")]),
			2,
			"This container declares no console."
		);
	}

	/* Options -- lifecycle/behavior configuration. */
	const optionsFields = document.getElementById("cd-options-fields");

	optionsFields.textContent = "";
	optionsFields.appendChild(fieldBlock("IP forwarding", c.ip_forward ? "yes" : "no"));
	optionsFields.appendChild(fieldBlock("Restart policy", c.restart || "no"));
	optionsFields.appendChild(
		fieldBlock(
			"Restart delay",
			c.restart_delay_seconds === null || c.restart_delay_seconds === undefined
				? "-"
				: c.restart_delay_seconds + "s"
		)
	);
	optionsFields.appendChild(fieldBlock("Follow rolling image", c.follow_rolling ? "yes" : "no"));
	optionsFields.appendChild(
		fieldBlock(
			"Follow-rolling jitter override",
			c.follow_rolling_jitter_seconds === null || c.follow_rolling_jitter_seconds === undefined
				? "(daemon default)"
				: c.follow_rolling_jitter_seconds + "s"
		)
	);
	optionsFields.appendChild(fieldBlock("Pinned image version", c.image_version || "-"));
	optionsFields.appendChild(fieldBlock("Depends on", (c.depends_on || []).join(", ") || "-"));
	optionsFields.appendChild(fieldBlock("DNS servers", (c.dns_servers || []).join(", ") || "-"));
	simpleTableRows(
		document.querySelector("#cd-sysctls tbody"),
		Object.entries(c.sysctls || {}).map(([k, v]) => [k, v]),
		2,
		"No sysctls set"
	);
	simpleTableRows(
		document.querySelector("#cd-env tbody"),
		Object.entries(c.env || {}).map(([k, v]) => [k, v]),
		2,
		"No environment variables set"
	);
	simpleTableRows(
		document.querySelector("#cd-files tbody"),
		(c.files || []).map((f) => [f]),
		1,
		"No files staged"
	);

	renderContainerStorage(name);
	refreshContainerStorageMigrate(name);

	document.getElementById("cd-start").onclick = () => startContainer(c.name);
	document.getElementById("cd-pause").onclick = () => pauseContainer(c.name);
	document.getElementById("cd-unpause").onclick = () => unpauseContainer(c.name);
	document.getElementById("cd-stop").onclick = () => stopContainer(c.name);
	document.getElementById("cd-remove").onclick = () => removeContainer(c.name);

	/* Only the action(s) valid for c.status are shown -- e.g. "Pause" on
	 * an already-stopped container isn't just a no-op, it's a 404 (see
	 * ADR-0045), so it's hidden rather than left clickable-but-broken. */
	document.getElementById("cd-start").hidden = c.status !== "stopped";
	document.getElementById("cd-pause").hidden = c.status !== "running";
	document.getElementById("cd-unpause").hidden = c.status !== "paused";
	document.getElementById("cd-stop").hidden = c.status === "stopped";
}

/* ---------- Networks ---------- */

function renderNetworks(networks) {
	const body = document.getElementById("networks-body");

	body.textContent = "";
	if (networks.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 5;
		cell.className = "empty";
		cell.textContent = "No networks";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const n of networks) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.appendChild(treeLink("#networks/" + encodeURIComponent(n.name), n.name, ""));
		row.appendChild(nameCell);

		const subnetCell = document.createElement("td");
		subnetCell.textContent = n.subnet;
		row.appendChild(subnetCell);

		const prefixCell = document.createElement("td");
		prefixCell.textContent = n.prefix_len;
		row.appendChild(prefixCell);

		const addressCell = document.createElement("td");
		addressCell.textContent = n.has_address ? n.address : "(none)";
		row.appendChild(addressCell);

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeNetwork(n.name));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshNetworks() {
	const data = await apiRequest("GET", CIX_API.listNetworks());
	cache.networks = data.networks;
	renderNetworks(cache.networks);
}

async function removeNetwork(name) {
	try {
		await apiRequest("DELETE", CIX_API.deleteNetwork(name));
		clearStatus();
		if (onPageOf("networks") && parseHash().name === name)
			location.hash = "#networks";
		await refreshNetworks();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove network " + name + ": " + e.message, true);
	}
}

/* ---------- DHCP: a self-contained service (ADR-0197) ----------
 *
 * Its own servers, its own ranges, its own reservations, every request
 * under /v1/dhcp. Nothing here reads or writes another service's
 * state, which is what has to be true for it to become a loadable
 * plugin later rather than a thing tangled through everything.
 *
 * Creating happens from the Services menu and listing happens on the
 * page, the same division the rest of the dashboard uses. The network
 * page shows leases on that network and nothing else -- one place to
 * configure a range, not two that can disagree.
 */
let dhcpCache = { servers: [], networks: [], static: [] };

function dhcpFillTable(tbody, rows, columns, emptyText) {
	tbody.textContent = "";
	if (rows.length === 0) {
		const tr = document.createElement("tr");
		const td = document.createElement("td");

		td.colSpan = columns;
		td.className = "empty";
		td.textContent = emptyText;
		tr.appendChild(td);
		tbody.appendChild(tr);
		return false;
	}
	return true;
}

function renderDhcpServers(servers) {
	const body = document.querySelector("#dhcp-servers-table tbody");

	if (!dhcpFillTable(body, servers, 4,
	                    "No DHCP server registered — register one from the Services menu"))
		return;
	for (const srv of servers) {
		const row = document.createElement("tr");
		const actions = document.createElement("td");
		const del = document.createElement("button");

		for (const text of [srv.container, srv.running ? "running" : "not running",
		                     srv.resolves_leases ? "yes" : "no — not a DNS server"]) {
			const td = document.createElement("td");

			td.textContent = text;
			row.appendChild(td);
		}
		del.type = "button";
		del.className = "button-small";
		del.textContent = "Unregister";
		del.addEventListener("click", async () => {
			try {
				await apiRequest("DELETE", CIX_API.unregisterDhcpServer(srv.container));
				showStatus("Unregistered — any range that named it has been re-split.", false);
				refreshDhcp();
			} catch (e) {
				showStatus("Failed to unregister: " + e.message, true);
			}
		});
		actions.appendChild(del);
		row.appendChild(actions);
		body.appendChild(row);
	}
}

function renderDhcpRanges(networks) {
	const body = document.querySelector("#dhcp-ranges-table tbody");
	const configured = networks.filter((n) => n.enabled || n.server_count !== 0 || n.range_start !== null);

	if (!dhcpFillTable(body, configured, 6,
	                    "No range configured — add one from the Services menu"))
		return;
	for (const n of configured) {
		const row = document.createElement("tr");
		const nameCell = document.createElement("td");
		const actions = document.createElement("td");
		const edit = document.createElement("button");

		nameCell.appendChild(treeLink("#networks/" + encodeURIComponent(n.network), n.network, ""));
		row.appendChild(nameCell);
		for (const text of [n.enabled ? "enabled" : "disabled",
		                     n.range_start === null ? "-" : n.range_start + " – " + n.range_end,
		                     n.lease_seconds + "s",
		                     /* The split, not the count: two servers are
		                      * redundant only because their pools are
		                      * disjoint, and that is the thing worth seeing. */
		                     (n.slices || []).length === 0
		                         ? "-"
		                         : (n.slices || [])
		                               .map((s) => s.server + " " + s.range_start + "–" + s.range_end)
		                               .join(", ")]) {
			const td = document.createElement("td");

			td.textContent = text;
			row.appendChild(td);
		}
		edit.type = "button";
		edit.className = "button-small";
		edit.textContent = "Edit";
		edit.addEventListener("click", () => openDhcpRangeModal(n.network));
		actions.appendChild(edit);
		{
			/*
			 * Remove deletes the configuration outright, which is not
			 * what unticking "Serve DHCP" does -- that disables it and
			 * keeps the range. Until now the config could be created
			 * and edited here but never taken back from either channel
			 * (ADR-0218 layer 2 turned that from an accident into a
			 * visible gap).
			 */
			const remove = document.createElement("button");

			remove.type = "button";
			remove.className = "button-small";
			remove.textContent = "Remove";
			remove.addEventListener("click", async () => {
				if (!confirm('Remove the DHCP configuration for "' + n.network +
				             '"? The range and its server assignments are deleted. To stop ' +
				             'serving but keep them, edit the range and untick "Serve DHCP".'))
					return;
				try {
					await apiRequest("DELETE", CIX_API.deleteDhcpNetwork(n.network));
					showStatus("DHCP configuration removed for " + n.network + ".", false);
					refreshDhcp();
				} catch (e) {
					showStatus("Failed to remove the configuration: " + e.message, true);
				}
			});
			actions.appendChild(remove);
		}
		row.appendChild(actions);
		body.appendChild(row);
	}
}

function renderDhcpStatic(entries) {
	const body = document.querySelector("#dhcp-static-table tbody");

	if (!dhcpFillTable(body, entries, 4,
	                    "No reservations — every client gets an address from the range"))
		return;
	for (const e of entries) {
		const row = document.createElement("tr");
		const actions = document.createElement("td");
		const del = document.createElement("button");

		for (const text of [e.mac, e.ip, e.hostname === null ? "-" : e.hostname]) {
			const td = document.createElement("td");

			td.textContent = text;
			row.appendChild(td);
		}
		del.type = "button";
		del.className = "button-small";
		del.textContent = "Remove";
		del.addEventListener("click", async () => {
			try {
				await apiRequest("DELETE", CIX_API.deleteDhcpReservation(e.mac));
				showStatus("Reservation removed.", false);
				refreshDhcp();
			} catch (err) {
				showStatus("Failed to remove: " + err.message, true);
			}
		});
		actions.appendChild(del);
		row.appendChild(actions);
		body.appendChild(row);
	}
}

function renderLeaseRows(tbody, leases) {
	if (!dhcpFillTable(tbody, leases, 5, "No leases — nothing has asked for an address yet"))
		return;
	for (const l of leases) {
		const row = document.createElement("tr");

		for (const text of [l.mac, l.ip, l.hostname === null ? "-" : l.hostname,
		                     new Date(l.expires_at * 1000).toLocaleString(), l.server]) {
			const td = document.createElement("td");

			td.textContent = text;
			row.appendChild(td);
		}
		tbody.appendChild(row);
	}
}

async function refreshDhcp() {
	try {
		const [all, servers, leases] = await Promise.all([
			apiRequest("GET", CIX_API.getDhcp()),
			apiRequest("GET", CIX_API.getDhcpServers()),
			apiRequest("GET", CIX_API.getDhcpLeases()),
		]);

		dhcpCache = {
			servers: servers.servers || [],
			networks: all.networks || [],
			static: all.static || [],
		};
		renderDhcpServers(dhcpCache.servers);
		renderDhcpRanges(dhcpCache.networks);
		renderDhcpStatic(dhcpCache.static);
		renderLeaseRows(document.querySelector("#dhcp-leases-table tbody"), leases.leases || []);
	} catch (e) {
		/* Best-effort, same as every other page here. */
	}
}

/* The network page's own DHCP tab: leases on THIS network and nothing
 * else. Filtered by whether the leased address is one this network's
 * own range hands out, since a lease carries no network of its own --
 * the server does not record one, and inventing a field for it here
 * would be storing a second answer to a question the range already
 * answers. */
async function refreshNetworkDhcp(name) {
	if (name === null || name === undefined)
		return;
	try {
		const [all, leases] = await Promise.all([
			apiRequest("GET", CIX_API.getDhcp()),
			apiRequest("GET", CIX_API.getDhcpLeases()),
		]);
		const cfg = (all.networks || []).find((n) => n.network === name);
		const servers = cfg === undefined ? [] : (cfg.slices || []).map((s) => s.server);
		const mine = (leases.leases || []).filter((l) => servers.indexOf(l.server) !== -1);

		renderLeaseRows(document.querySelector("#nd-dhcp-leases tbody"), mine);
	} catch (e) {
		/* Best-effort. */
	}
}

/* ---- Creating things: from the Services menu, never from the page ---- */

function openDhcpRangeModal(network) {
	const netSelect = document.getElementById("drf-network");
	const srvSelect = document.getElementById("drf-servers");
	const cfg = dhcpCache.networks.find((n) => n.network === network);

	netSelect.textContent = "";
	for (const n of cache.networks || []) {
		const opt = document.createElement("option");

		opt.value = n.name;
		opt.textContent = n.name;
		netSelect.appendChild(opt);
	}
	if (network !== undefined && network !== null)
		netSelect.value = network;

	/* Only registered DHCP servers, because only they can serve one.
	 * Offering anything else would offer a choice the daemon refuses. */
	srvSelect.textContent = "";
	for (const srv of dhcpCache.servers) {
		const opt = document.createElement("option");

		opt.value = srv.container;
		opt.textContent = srv.container + (srv.resolves_leases ? "" : " (leases will not resolve)");
		opt.selected = cfg !== undefined && (cfg.servers || []).indexOf(srv.container) !== -1;
		srvSelect.appendChild(opt);
	}
	document.getElementById("drf-enabled").checked = cfg !== undefined && cfg.enabled;
	document.getElementById("drf-start").value = cfg === undefined ? "" : cfg.range_start || "";
	document.getElementById("drf-end").value = cfg === undefined ? "" : cfg.range_end || "";
	document.getElementById("drf-lease").value = cfg === undefined ? 3600 : cfg.lease_seconds;
	document.getElementById("drf-router").value = cfg === undefined ? "" : cfg.router || "";
	openModal("dhcp-range-form", "DHCP range");
}

document.getElementById("dhcp-range-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	const network = document.getElementById("drf-network").value;
	const router = document.getElementById("drf-router").value.trim();
	const body = {
		enabled: document.getElementById("drf-enabled").checked,
		range_start: document.getElementById("drf-start").value.trim(),
		range_end: document.getElementById("drf-end").value.trim(),
		lease_seconds: parseInt(document.getElementById("drf-lease").value, 10),
		servers: Array.from(document.getElementById("drf-servers").selectedOptions).map((o) => o.value),
	};

	if (router !== "")
		body.router = router;
	try {
		await apiRequest("PUT", CIX_API.setDhcpNetwork(network), body);
		closeModal();
		/* Said every time rather than only when a range really changed:
		 * the daemon decides that, and promising "no restart" from here
		 * would be this page guessing at it. */
		showStatus("Range saved — if it changed, the servers serving it are being restarted.",
		           false);
		refreshDhcp();
	} catch (e) {
		showStatus("Failed to save the range: " + e.message, true);
	}
});

document.getElementById("dhcp-server-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	try {
		await apiRequest("POST", CIX_API.registerDhcpServer(), {
			container: document.getElementById("dsv-container").value.trim(),
		});
		closeModal();
		document.getElementById("dsv-container").value = "";
		showStatus("Registered.", false);
		refreshDhcp();
	} catch (e) {
		showStatus("Failed to register: " + e.message, true);
	}
});

document.getElementById("dhcp-static-modal-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	const hostname = document.getElementById("dsm-hostname").value.trim();
	const body = {
		mac: document.getElementById("dsm-mac").value.trim(),
		ip: document.getElementById("dsm-ip").value.trim(),
	};

	if (hostname !== "")
		body.hostname = hostname;
	try {
		await apiRequest("POST", CIX_API.addDhcpReservation(), body);
		closeModal();
		showStatus("Reserved.", false);
		document.getElementById("dsm-mac").value = "";
		document.getElementById("dsm-ip").value = "";
		document.getElementById("dsm-hostname").value = "";
		refreshDhcp();
	} catch (e) {
		showStatus("Failed to reserve: " + e.message, true);
	}
});

/*
 * ---------- Issue #26: per-network switch panel ----------
 *
 * Same shape as the container/host stats windows above: a client-side
 * rolling window kept only while this page is open, raw counters from
 * the daemon, rates computed here. GET /networks/{name}/ports is the
 * per-port counterpart of GET /system/stats, and drawChart() is reused
 * verbatim for the aggregate.
 */
const NET_PORTS_HISTORY_MAX = 60;

let netPortsTimer = null;
let netPortsName = null;
let netPortsHistory = [];

function stopNetPortsPolling() {
	if (netPortsTimer !== null) {
		clearInterval(netPortsTimer);
		netPortsTimer = null;
	}
	netPortsName = null;
	netPortsHistory = [];
}

/* Bytes per second between two samples of one port's counters. Returns
 * null rather than 0 when there is no prior sample to diff against, or
 * when the port was not there last time -- a port that has just
 * appeared has no rate yet, and showing 0 would be a measurement it
 * has not made. */
function portRate(prev, cur, ifname, field, dtMs) {
	const before = prev === undefined ? undefined : prev.byIf[ifname];

	if (before === undefined || dtMs <= 0)
		return null;
	const delta = cur.byIf[ifname][field] - before[field];

	/* A counter that went backwards means the interface was recreated
	 * under the same name (a container restarting into the same pid is
	 * rare but real). Not a negative rate -- no measurement. */
	if (delta < 0)
		return null;
	return (delta * 1000) / dtMs;
}

function renderNetPorts() {
	const panel = document.getElementById("nd-ports");
	const h = netPortsHistory;

	if (h.length === 0)
		return;
	const cur = h[h.length - 1];
	const prev = h.length > 1 ? h[h.length - 2] : undefined;
	const dtMs = prev === undefined ? 0 : cur.t - prev.t;

	panel.textContent = "";
	if (!cur.bridgePresent) {
		const p = document.createElement("p");

		p.className = "hint";
		p.textContent = "No bridge for this network on this host — nothing is plugged in.";
		panel.appendChild(p);
		return;
	}
	if (cur.ports.length === 0) {
		const p = document.createElement("p");

		p.className = "hint";
		p.textContent = "The bridge exists, but nothing is on it.";
		panel.appendChild(p);
		return;
	}

	cur.ports.forEach((port, i) => {
		const box = document.createElement("div");
		const head = document.createElement("div");
		const jack = document.createElement("span");
		const num = document.createElement("span");
		const ifn = document.createElement("span");
		const attached = document.createElement("div");
		const rate = document.createElement("div");

		/*
		 * Only "up" is up and only a real down state is down. An
		 * operstate of "unknown" is what a driver reports when it does
		 * not track carrier at all (a dummy or tap interface), and
		 * colouring that red would invent a fault out of a driver's
		 * silence.
		 */
		box.className = "switch-port " +
			(port.link === "up"
				? "switch-port-up"
				: port.link === "down" || port.link === "lowerlayerdown"
				  ? "switch-port-down"
				  : "") +
			(port.kind === "unattributed" ? " switch-port-unattributed" : "");
		/* The number is this panel's own position, not a port identity:
		 * ports come and go with containers. The title carries what
		 * actually names it. */
		box.title = port.ifname + " — " + port.kind +
			(port.link !== null ? ", link " + port.link : ", link unknown");

		jack.className = "switch-port-jack";
		num.className = "switch-port-num";
		num.textContent = String(i + 1);
		ifn.className = "switch-port-if";
		ifn.textContent = port.ifname;
		head.className = "switch-port-head";
		head.appendChild(jack);
		head.appendChild(num);
		head.appendChild(ifn);

		/*
		 * TX/RX lamps, lit when this port's own counters moved since
		 * the previous poll. Deliberately per-port and counter-driven,
		 * unlike the status bar's pair, which reports this page's own
		 * requests: here the light means real traffic crossed this
		 * port. Both are named from the port's own side, matching the
		 * rate line -- RX is what reached the switch.
		 */
		const leds = document.createElement("div");

		leds.className = "switch-port-leds";
		leds.title = "RX and TX lamps — lit while this port's own counters are moving";
		for (const [cls, field] of [["switch-port-led-rx", "rx_bytes"],
		                             ["switch-port-led-tx", "tx_bytes"]]) {
			const led = document.createElement("span");
			const moved = portRate(prev, cur, port.ifname, field, dtMs);

			led.className = "switch-port-led " + cls +
				(moved !== null && moved > 0 ? " switch-port-led-on" : "");
			leds.appendChild(led);
		}
		head.appendChild(leds);

		attached.className = "switch-port-attached";
		if (port.container !== null) {
			attached.appendChild(treeLink("#containers/" + encodeURIComponent(port.container),
			                               port.container, ""));
			if (port.ip !== null)
				attached.appendChild(document.createTextNode(" " + port.ip));
		} else if (port.kind === "uplink") {
			attached.textContent = "uplink" + (port.vlan_id ? " · vlan " + port.vlan_id : "");
		} else {
			attached.textContent = "not accounted for";
		}

		const rxRate = portRate(prev, cur, port.ifname, "rx_bytes", dtMs);
		const txRate = portRate(prev, cur, port.ifname, "tx_bytes", dtMs);

		rate.className = "switch-port-rate";
		rate.textContent = rxRate === null
			? "in … · out …"
			: "in " + formatBytes(rxRate) + "/s · out " + formatBytes(txRate) + "/s";

		box.appendChild(head);
		box.appendChild(attached);
		box.appendChild(rate);
		panel.appendChild(box);
	});

	/* The aggregate: every port summed. Uplink and container ports both
	 * count, and the same byte crossing the switch is seen on two of
	 * them -- so this is switch throughput, not the network's traffic
	 * with the outside world, and the label says so. */
	const rateTimes = [];
	const rxValues = [];
	const txValues = [];

	for (let i = 1; i < h.length; i++) {
		const dt = h[i].t - h[i - 1].t;
		let rx = 0;
		let tx = 0;

		for (const port of h[i].ports) {
			const r = portRate(h[i - 1], h[i], port.ifname, "rx_bytes", dt);
			const t = portRate(h[i - 1], h[i], port.ifname, "tx_bytes", dt);

			rx += r === null ? 0 : r;
			tx += t === null ? 0 : t;
		}
		rateTimes.push(h[i].t);
		rxValues.push(rx);
		txValues.push(tx);
	}
	drawChart(
		document.getElementById("nd-traffic"),
		[{ values: rxValues, color: seriesColor(1) }, { values: txValues, color: seriesColor(1) }],
		{ times: rateTimes, formatY: (v) => formatBytes(v) + "/s" }
	);
	/* The caveat about double-counting lives in the panel's own hint
	 * text, not here: a label under a small chart is read at a glance,
	 * and three lines of explanation under it is not a glance. */
	document.getElementById("nd-traffic-label").textContent =
		rxValues.length === 0
			? "…"
			: "in " + formatBytes(rxValues[rxValues.length - 1]) + "/s · out " +
			  formatBytes(txValues[txValues.length - 1]) + "/s";
}

async function pollNetPortsOnce(name) {
	try {
		const data = await apiRequest("GET", CIX_API.getNetworkPorts(name));
		const byIf = {};

		for (const p of data.ports || [])
			byIf[p.ifname] = p;
		netPortsHistory.push({
			t: Date.now(),
			bridgePresent: data.bridge_present,
			ports: data.ports || [],
			byIf: byIf,
		});
		if (netPortsHistory.length > NET_PORTS_HISTORY_MAX)
			netPortsHistory.shift();
		renderNetPorts();
	} catch (e) {
		/* Best-effort; the last drawn panel stays put rather than
		 * blanking on one failed poll. */
	}
}

function startNetPortsPolling(name) {
	if (netPortsName === name && netPortsTimer !== null)
		return; /* a poll-driven re-render of the same network must not
		         * reset the window it has been building */
	stopNetPortsPolling();
	netPortsName = name;
	pollNetPortsOnce(name);
	netPortsTimer = setInterval(() => pollNetPortsOnce(name), POLL_INTERVAL_MS);
}

function renderNetworkDetail(name) {
	const n = cache.networks.find((x) => x.name === name);
	const fields = document.getElementById("nd-fields");

	if (!n) {
		fields.textContent = name + " \u2014 not found.";
		stopNetPortsPolling();
		return;
	}

	startNetPortsPolling(n.name);
	refreshNetworkDhcp(n.name);
	fields.textContent = "";
	fields.appendChild(fieldBlock("Subnet", n.subnet));
	fields.appendChild(fieldBlock("Prefix length", String(n.prefix_len)));
	fields.appendChild(fieldBlock("Address", n.has_address ? n.address : "(none -- pure L2)"));

	/* Containers on this network -- the reverse of what a container's
	 * own Hardware tab already shows (network name + IP, from its own
	 * `networks[]`). Purely a client-side cross-reference against the
	 * already-cached container list; no dedicated API field for it
	 * exists or is needed (GET /networks/{name} has no per-container
	 * view of its own -- see #804's own investigation). */
	const containersBody = document.querySelector("#nd-containers tbody");
	const attachedContainers = cache.containers
		.map((c) => ({ container: c, attachment: (c.networks || []).find((att) => att.name === n.name) }))
		.filter((x) => x.attachment !== undefined);

	containersBody.textContent = "";
	if (attachedContainers.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 3;
		cell.className = "empty";
		cell.textContent = "No containers on this network";
		row.appendChild(cell);
		containersBody.appendChild(row);
	} else {
		for (const { container: c, attachment: att } of attachedContainers) {
			const row = document.createElement("tr");
			const nameCell = document.createElement("td");
			const ipCell = document.createElement("td");
			const statusCell = document.createElement("td");

			nameCell.appendChild(treeLink("#containers/" + encodeURIComponent(c.name), c.name, ""));
			ipCell.textContent = att.ip;
			statusCell.textContent = c.status;
			row.appendChild(nameCell);
			row.appendChild(ipCell);
			row.appendChild(statusCell);
			containersBody.appendChild(row);
		}
	}

	const ifBody = document.querySelector("#nd-interfaces tbody");

	ifBody.textContent = "";
	const attached = n.interfaces || [];

	if (attached.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 3;
		cell.className = "empty";
		cell.textContent = "No interfaces attached";
		row.appendChild(cell);
		ifBody.appendChild(row);
	} else {
		for (const att of attached) {
			const row = document.createElement("tr");
			const ifCell = document.createElement("td");

			ifCell.textContent = att.ifname;
			row.appendChild(ifCell);
			const vlanCell = document.createElement("td");

			vlanCell.textContent = att.vlan_id ? "VLAN " + att.vlan_id : "untagged";
			row.appendChild(vlanCell);
			const actionCell = document.createElement("td");
			const rmButton = document.createElement("button");

			rmButton.textContent = "Detach";
			rmButton.className = "button-danger button-small";
			rmButton.addEventListener("click", () => detachInterface(n.name, att.ifname));
			actionCell.appendChild(rmButton);
			row.appendChild(actionCell);
			ifBody.appendChild(row);
		}
	}

	const select = document.getElementById("nd-attach-ifname");

	select.textContent = "";
	const assignableNets = cache.devices.filter((d) => d.bus === "net" && d.assignable);

	for (const d of assignableNets) {
		const opt = document.createElement("option");

		opt.value = d.id.replace(/^net:/, "");
		opt.textContent = opt.value;
		select.appendChild(opt);
	}

	const routeBody = document.querySelector("#nd-routes tbody");
	const netRoutes = cache.routes.filter((r) => r.interface === n.name);

	routeBody.textContent = "";
	if (netRoutes.length === 0) {
		routeBody.innerHTML = '<tr><td colspan="4" class="empty">No routes on this network</td></tr>';
	} else {
		for (const route of netRoutes)
			appendRouteRow(routeBody, route);
	}

	document.getElementById("nd-remove").onclick = () => removeNetwork(n.name);
}

async function attachInterface(networkName, ifname, vlanId) {
	try {
		const body = { ifname: ifname };

		if (vlanId)
			body.vlan_id = vlanId;
		await apiRequest("POST", CIX_API.attachNetworkInterface(networkName), body);
		clearStatus();
		await refreshNetworks();
		await refreshDevices();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to attach " + ifname + " to " + networkName + ": " + e.message, true);
	}
}

async function detachInterface(networkName, ifname) {
	try {
		await apiRequest("DELETE", CIX_API.detachNetworkInterface(networkName, ifname));
		clearStatus();
		await refreshNetworks();
		await refreshDevices();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to detach " + ifname + ": " + e.message, true);
	}
}

/* ---------- Images ---------- */

function renderImages(images) {
	const body = document.getElementById("images-body");

	/* #432: shares the pkg-build-config refresh tick with the build-log
	 * table, so guarding only that one still collapsed the page height
	 * and still lost the reader's place. */
	if (unchangedAndRendered(body, "images", images))
		return;
	body.textContent = "";
	if (images.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 2;
		cell.className = "empty";
		cell.textContent = "No images";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const img of images) {
		const row = document.createElement("tr");
		const nameCell = document.createElement("td");

		nameCell.appendChild(treeLink("#images/" + encodeURIComponent(img.name), img.name, ""));
		row.appendChild(nameCell);
		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeImage(img.name));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);
		body.appendChild(row);
	}
}

async function refreshImages() {
	const data = await apiRequest("GET", CIX_API.listImages());
	cache.images = data.images;
	renderImages(cache.images);
}

/*
 * ADR-0209: image version collection.
 *
 * Preview and reclaim are the same endpoint, differing only by
 * dry_run. The preview exists because the reclaim is irreversible and
 * the operator has no other way to see which versions are unreferenced
 * -- the Versions tab shows what an image HAS, not what is holding it.
 */
async function runImageGc(dryRun) {
	const box = document.getElementById("images-gc-status");

	/* Hidden until it has something to say -- an always-present empty
	 * panel reads as a broken element rather than an idle one. */
	box.hidden = false;
	box.textContent = dryRun ? "Checking…" : "Reclaiming…";
	try {
		/* measure:false deliberately -- sizing walks every collectable
		 * version and the daemon is single-threaded; on a real box with
		 * 80 of them that blocked the whole API for nearly two minutes.
		 * The dashboard shows what would go, not how big it is. */
		const res = await apiRequest("POST", CIX_API.imagesGc(), { dry_run: dryRun, measure: false });
		const list = res.reclaimed || [];

		box.textContent = "";
		if (list.length === 0) {
			const p = document.createElement("p");

			p.textContent = "Nothing to reclaim — every image version is referenced ("
				+ res.kept + " kept).";
			box.appendChild(p);
		} else {
			for (const e of list) {
				const p = document.createElement("p");

				p.textContent = (dryRun ? "Would remove " : "Removed ")
					+ e.image + "@" + String(e.version).slice(0, 12)
					+ (e.apparent_bytes === null || e.apparent_bytes === undefined
						? "" : " — " + formatBytes(e.apparent_bytes));
				box.appendChild(p);
			}
			const sum = document.createElement("p");

			/* "apparent" is load-bearing, not a hedge: on btrfs a version is a
			 * snapshot sharing extents with its neighbours, so the space
			 * actually returned is usually well below this figure. */
			sum.textContent = (dryRun ? "Would reclaim " : "Reclaimed ") + list.length
				+ " version(s)"
				+ (res.apparent_bytes_total === null || res.apparent_bytes_total === undefined
					? "" : ", " + formatBytes(res.apparent_bytes_total) + " apparent")
				+ "; " + res.kept + " kept"
				+ (res.failed > 0 ? ", " + res.failed + " could not be removed" : "") + ".";
			box.appendChild(sum);
		}
		if (!dryRun)
			await refreshImages();
	} catch (e) {
		box.hidden = true;
		box.textContent = "";
		showStatus("Image reclaim failed: " + e.message, true);
	}
}

async function removeImage(name) {
	try {
		await apiRequest("DELETE", CIX_API.deleteImage(name));
		clearStatus();
		if (onPageOf("images") && parseHash().name === name)
			location.hash = "#images";
		await refreshImages();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove image " + name + ": " + e.message, true);
	}
}

function renderImageDetail(name) {
	const usingContainers = cache.containers.filter((c) => c.image === name);
	const containersBody = document.querySelector("#imgd-containers tbody");

	containersBody.textContent = "";
	if (usingContainers.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 2;
		cell.className = "empty";
		cell.textContent = "No containers use this image";
		row.appendChild(cell);
		containersBody.appendChild(row);
	} else {
		for (const c of usingContainers) {
			const row = document.createElement("tr");
			const nameCell = document.createElement("td");
			const statusCell = document.createElement("td");

			nameCell.appendChild(treeLink("#containers/" + encodeURIComponent(c.name), c.name, ""));
			statusCell.textContent = c.status;
			row.appendChild(nameCell);
			row.appendChild(statusCell);
			containersBody.appendChild(row);
		}
	}

	renderImageDetailPackages(name);
	renderImageDetailRecipes(name);
	refreshImageDetailVersioning(name);
	renderImageRecipeTab(name);

	document.getElementById("imgd-remove").onclick = () => removeImage(name);
	document.getElementById("imgd-add-recipe").onclick = () => openModal("pkg-recipe-form", "Add or update a recipe");
}

/* ---- ADR-0123: image recipe tab (declarative package-list definition) ---- */

let imageRecipeContentCache = { name: null, content: null };

async function loadImageRecipeContent(name) {
	if (imageRecipeContentCache.name === name)
		return imageRecipeContentCache.content;
	const data = await apiRequest("GET", CIX_API.getImageRecipe(name));

	imageRecipeContentCache = { name: name, content: data.content };
	return imageRecipeContentCache.content;
}

function renderImageRecipeTab(name) {
	const missingEl = document.getElementById("imgd-recipe-missing");
	const presentEl = document.getElementById("imgd-recipe-present");
	const contentEl = document.getElementById("imgd-recipe-content");

	loadImageRecipeContent(name)
		.then((content) => {
			if (onPageOf("images") && parseHash().name === name) {
				missingEl.hidden = true;
				presentEl.hidden = false;
				contentEl.textContent = content;
			}
		})
		.catch(() => {
			if (onPageOf("images") && parseHash().name === name) {
				missingEl.hidden = false;
				presentEl.hidden = true;
			}
		});

	document.getElementById("imgd-edit-recipe").onclick = async () => {
		let content = "";

		try {
			content = await loadImageRecipeContent(name);
		} catch (e) {
			/* No recipe yet -- start from an empty template. */
		}
		openModal("image-recipe-form", "Edit image recipe");
		document.getElementById("irf-name").value = name;
		document.getElementById("irf-name").readOnly = true;
		document.getElementById("irf-content").value = content;
	};
	document.getElementById("imgd-remove-recipe").onclick = async () => {
		try {
			await apiRequest("DELETE", CIX_API.deleteImageRecipe(name));
			clearStatus();
			imageRecipeContentCache = { name: null, content: null };
			renderImageRecipeTab(name);
		} catch (e) {
			showStatus("Failed to remove image recipe for " + name + ": " + e.message, true);
		}
	};
	document.getElementById("imgd-apply-recipe").onclick = async () => {
		try {
			const r = await apiRequest("POST", CIX_API.applyImageRecipe(name));

			clearStatus();
			showStatus(
				r && r.state === "running"
					? "Recipe apply started for " + name + " (async artifact fetch)"
					: "Recipe applied for " + name,
				false
			);
			await refreshImageDetailVersioning(name);
		} catch (e) {
			showStatus("Failed to apply recipe for " + name + ": " + e.message, true);
		}
	};
}


document.getElementById("image-recipe-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("irf-name").value.trim();
	const content = document.getElementById("irf-content").value;

	if (name === "" || content.trim() === "")
		return;

	try {
		await apiRequest("POST", CIX_API.addImageRecipe(), { name: name, content: content });
		clearStatus();
		document.getElementById("image-recipe-form").reset();
		closeModal();
		imageRecipeContentCache = { name: null, content: null };
		if (onPageOf("images") && parseHash().name === name)
			renderImageRecipeTab(name);
		await refreshImageRecipesList();
	} catch (e) {
		showStatus("Failed to save image recipe for " + name + ": " + e.message, true);
	}
});

/* GET /v1/images/{name} isn't part of the images-list cache (that only
 * ever carries {"name":...} per entry, deliberately minimal) -- the
 * manifest/current_version/versions fields (ADR-0107/0108, task #721)
 * are fetched fresh each time the detail view opens. */
async function refreshImageDetailVersioning(name) {
	try {
		const data = await apiRequest("GET", CIX_API.getImage(name));

		renderImageDetailManifest(name, data);
		renderImageDetailVersions(name, data);
	} catch (e) {
		showStatus("Failed to load image details for " + name + ": " + e.message, true);
	}
}

/* Declared package intent (ADR-0107) -- {package, mode, version}
 * entries an operator has set via POST .../manifest, each removable
 * via DELETE .../manifest/{package}; the form below upserts a new or
 * existing entry through that same POST. */
function renderImageDetailManifest(name, data) {
	const body = document.querySelector("#imgd-manifest tbody");
	const manifest = Array.isArray(data.manifest) ? data.manifest : [];

	body.textContent = "";
	if (manifest.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 4;
		cell.className = "empty";
		cell.textContent = "No manifest entries -- this image has no declared package intent";
		row.appendChild(cell);
		body.appendChild(row);
	} else {
		for (const entry of manifest) {
			const row = document.createElement("tr");

			const pkgCell = document.createElement("td");
			pkgCell.textContent = entry.package;
			row.appendChild(pkgCell);

			const modeCell = document.createElement("td");
			modeCell.textContent = entry.mode;
			row.appendChild(modeCell);

			const versionCell = document.createElement("td");
			versionCell.textContent = entry.version;
			row.appendChild(versionCell);

			const actionCell = document.createElement("td");
			const rmButton = document.createElement("button");

			rmButton.textContent = "Remove";
			rmButton.className = "button-danger";
			rmButton.addEventListener("click", async () => {
				try {
					await apiRequest("DELETE", CIX_API.unsetImageManifestEntry(name, entry.package));
					clearStatus();
					refreshImageDetailVersioning(name);
				} catch (e) {
					showStatus("Failed to remove manifest entry: " + e.message, true);
				}
			});
			actionCell.appendChild(rmButton);
			row.appendChild(actionCell);

			body.appendChild(row);
		}
	}

	const form = document.getElementById("imgd-manifest-form");

	form.onsubmit = async (ev) => {
		ev.preventDefault();
		const pkg = document.getElementById("imgd-manifest-package").value.trim();
		const mode = document.getElementById("imgd-manifest-mode").value;
		const version = document.getElementById("imgd-manifest-version").value.trim();

		try {
			await apiRequest("POST", CIX_API.setImageManifestEntry(name), {
				package: pkg,
				mode: mode,
				version: version,
			});
			clearStatus();
			form.reset();
			refreshImageDetailVersioning(name);
		} catch (e) {
			showStatus("Failed to set manifest entry: " + e.message, true);
		}
	};
}

/* Full immutable version history (ADR-0107/0108) -- newest first,
 * current_version highlighted. Read-only: a version is only ever
 * produced by an install/upgrade/delete or the rolling auto-rebuild
 * trigger (task #720), never created or removed directly here. */
function renderImageDetailVersions(name, data) {
	const body = document.querySelector("#imgd-versions tbody");
	const versions = Array.isArray(data.versions) ? data.versions : [];
	const currentVersion = data.current_version || "";

	body.textContent = "";
	if (versions.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 3;
		cell.className = "empty";
		cell.textContent = "No version history";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const v of versions) {
		const row = document.createElement("tr");
		const isCurrent = v.version === currentVersion;

		const versionCell = document.createElement("td");
		versionCell.textContent = v.version;
		row.appendChild(versionCell);

		const createdCell = document.createElement("td");
		createdCell.textContent = v.created_at ? new Date(v.created_at * 1000).toLocaleString() : "-";
		row.appendChild(createdCell);

		const statusCell = document.createElement("td");
		if (isCurrent) {
			const badge = document.createElement("strong");

			badge.textContent = "current";
			statusCell.appendChild(badge);
		}
		row.appendChild(statusCell);

		body.appendChild(row);
	}
}

/* Installed-on-this-image packages, with a per-row Remove -- same
 * DELETE /v1/pkg/{name@image} removePkg() already uses from the
 * Packages section, just scoped to whichever image is on screen. */
function renderImageDetailPackages(name) {
	const body = document.querySelector("#imgd-packages tbody");
	const pkgs = cache.pkgList.filter((p) => p.image === name);

	body.textContent = "";
	if (pkgs.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 4;
		cell.className = "empty";
		cell.textContent = "No packages installed into this image";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const pkg of pkgs) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.appendChild(treeLink("#packages/" + encodeURIComponent(pkg.name), pkg.name, ""));
		row.appendChild(nameCell);

		const versionCell = document.createElement("td");
		versionCell.textContent = pkg.version;
		row.appendChild(versionCell);

		const stateCell = document.createElement("td");
		/* Issue #101: a package that could not be REACHED and one that
		 * failed to BUILD both read "failed", and they call for
		 * opposite responses -- retry the first, fix the second. The
		 * kind sits next to the state; the message is the hover, so the
		 * table stays readable. */
		stateCell.textContent = pkg.stage ? pkg.state + " (" + pkg.stage + "/" + pkg.status + ")"
		                                          : pkg.state;
		if (pkg.error)
			stateCell.title = pkg.error;
		row.appendChild(stateCell);

		const actionCell = document.createElement("td");
		if (pkg.state === "installed") {
			const rmButton = document.createElement("button");

			rmButton.textContent = "Remove";
			rmButton.className = "button-danger";
			rmButton.addEventListener("click", async () => {
				await removePkg(pkg.name, pkg.image);
				renderImageDetailPackages(name);
				renderImageDetailRecipes(name);
			});
			actionCell.appendChild(rmButton);
		}
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

/* The shared recipe catalog (ADR-0040), rendered here with a per-row
 * Install/Remove scoped to whichever image is on screen -- the "add/
 * delete recipes from images" convenience the user asked for, without
 * turning recipes into a per-image concept: POST /v1/pkg/install and
 * DELETE /v1/pkg/{name@image} already take an image, this just supplies
 * it for the recipe row being clicked instead of making the user type
 * a name/image pair into the separate Packages-section modal. */
function renderImageDetailRecipes(name) {
	const body = document.querySelector("#imgd-recipes tbody");

	body.textContent = "";
	if (cache.pkgRecipes.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 4;
		cell.className = "empty";
		cell.textContent = "No recipes found";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	const installedNames = new Set(
		cache.pkgList.filter((p) => p.image === name && p.state === "installed").map((p) => p.name)
	);

	for (const r of cache.pkgRecipes) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.textContent = r.name;
		row.appendChild(nameCell);

		const versionCell = document.createElement("td");
		versionCell.textContent = r.version;
		row.appendChild(versionCell);

		const dependsCell = document.createElement("td");
		dependsCell.textContent = r.depends || "-";
		row.appendChild(dependsCell);

		const actionCell = document.createElement("td");
		if (installedNames.has(r.name)) {
			const rmButton = document.createElement("button");

			rmButton.textContent = "Remove from image";
			rmButton.className = "button-danger";
			rmButton.addEventListener("click", async () => {
				await removePkg(r.name, name);
				await refreshPkgList();
				renderImageDetailPackages(name);
				renderImageDetailRecipes(name);
			});
			actionCell.appendChild(rmButton);
		} else {
			const installButton = document.createElement("button");

			installButton.textContent = "Install onto this image";
			installButton.addEventListener("click", async () => {
				try {
					await apiRequest("POST", CIX_API.pkgInstall(), { name: r.name, image: name });
					clearStatus();
					await refreshPkgList();
					renderImageDetailPackages(name);
					renderImageDetailRecipes(name);
				} catch (e) {
					showStatus("Failed to install " + r.name + " onto " + name + ": " + e.message, true);
				}
			});
			actionCell.appendChild(installButton);
		}
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

/* ---------- Devices ---------- */

async function refreshDevices() {
	const data = await apiRequest("GET", CIX_API.listDevices());
	cache.devices = data.devices;
	if (onPageOf("devices"))
		renderDevices();
	populateContainerFormDeviceLists();
}

async function refreshDeviceMaps() {
	const data = await apiRequest("GET", CIX_API.listDeviceMaps());
	cache.deviceMaps = data.devicemaps;
	if (onPageOf("devices"))
		renderDevices();
}

const DEVICE_BUS_BODIES = { usb: "dev-usb-body", pci: "dev-pci-body", net: "dev-net-body", gpu: "dev-gpu-body" };

/* Only "exact" mappings can be shown inline against the one device row
 * they pin to; a "vendor_model" mapping's own resolved_ids can span
 * several rows (or move between polls), so it's only ever shown in
 * the Named mappings table, never annotated per-row on a specific
 * device. Shared by the Devices page's own per-bus tables and a
 * container's own Hardware tab (its device grants use the same raw
 * ids these mappings pin to). */
function exactMappedNames() {
	const mappedNames = {};

	for (const m of cache.deviceMaps)
		if (m.kind === "exact") mappedNames[m.selector] = m.name;
	return mappedNames;
}

function renderDevices() {
	renderDeviceMapsTable();

	const mappedNames = exactMappedNames();

	for (const bus of Object.keys(DEVICE_BUS_BODIES)) {
		const tbody = document.getElementById(DEVICE_BUS_BODIES[bus]);
		const entries = cache.devices.filter((d) => d.bus === bus);

		tbody.textContent = "";
		if (entries.length === 0) {
			const row = document.createElement("tr");
			const cell = document.createElement("td");

			cell.colSpan = 6;
			cell.className = "empty";
			cell.textContent = "No devices discovered on this bus";
			row.appendChild(cell);
			tbody.appendChild(row);
			continue;
		}

		if (bus === "gpu") {
			/* Group member nodes (gpu:N:card0, gpu:N:renderD128, ...)
			 * under their synthesized gpu:N prefix -- mirroring exactly
			 * what device_find_group() does server-side for a
			 * container's own devices:["gpu:0"] grant, so this view's
			 * grouping matches what attaching the group id actually
			 * grants. */
			const groups = {};

			for (const d of entries) {
				const parts = d.id.split(":");
				const groupId = parts[0] + ":" + parts[1];

				if (!groups[groupId])
					groups[groupId] = [];
				groups[groupId].push(d);
			}
			for (const groupId of Object.keys(groups).sort()) {
				const groupRow = document.createElement("tr");
				const groupCell = document.createElement("td");

				groupCell.colSpan = 6;
				groupCell.textContent = groupId + " (grant this whole id to a container)";
				groupCell.style.fontWeight = "600";
				groupRow.appendChild(groupCell);
				tbody.appendChild(groupRow);
				for (const d of groups[groupId])
					tbody.appendChild(deviceRow(d, mappedNames[d.id]));
			}
		} else {
			for (const d of entries)
				tbody.appendChild(deviceRow(d, mappedNames[d.id]));
		}
	}
}

function deviceRow(d, mappedName) {
	const row = document.createElement("tr");
	const idCell = document.createElement("td");

	idCell.textContent = d.id;
	row.appendChild(idCell);
	const descCell = document.createElement("td");

	descCell.textContent = d.description || "-";
	row.appendChild(descCell);
	const driverCell = document.createElement("td");

	driverCell.textContent = d.driver || "-";
	row.appendChild(driverCell);
	const assignableCell = document.createElement("td");
	const badge = document.createElement("span");

	badge.className = statusBadge(d.assignable ? "ok" : "unknown");
	badge.textContent = d.assignable ? "yes" : "no";
	assignableCell.appendChild(badge);
	row.appendChild(assignableCell);

	const nameCell = document.createElement("td");

	nameCell.textContent = mappedName || "-";
	row.appendChild(nameCell);

	const actionCell = document.createElement("td");

	if (!mappedName) {
		const btn = document.createElement("button");

		btn.type = "button";
		btn.textContent = "Name…";
		btn.addEventListener("click", () => {
			document.getElementById("dmf-name").value = "";
			document.getElementById("dmf-kind").value = "exact";
			document.getElementById("dmf-selector").value = d.id;
			openModal("devicemap-form", "Name a device");
		});
		actionCell.appendChild(btn);
	}
	row.appendChild(actionCell);
	return row;
}

/* Every container currently holding a device whose id is among
 * resolvedIds -- a mapping's own "present" status only says the
 * underlying hardware exists, not whether anything's actually using
 * it right now; this is the one piece client-side cross-referencing
 * (cache.containers[].devices[].id vs. cache.deviceMaps[].resolved_ids,
 * both already loaded) can answer that neither list showed on its
 * own before. */
function containersUsingDeviceIds(resolvedIds) {
	const idSet = new Set(resolvedIds);

	return cache.containers.filter((c) => (c.devices || []).some((d) => idSet.has(d.id))).map((c) => c.name);
}

function renderDeviceMapsTable() {
	const body = document.getElementById("devicemaps-body");

	body.textContent = "";
	if (cache.deviceMaps.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 6;
		cell.className = "empty";
		cell.textContent = "No named devices yet";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}
	for (const m of cache.deviceMaps) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");

		nameCell.textContent = m.name;
		row.appendChild(nameCell);
		const kindCell = document.createElement("td");

		kindCell.textContent = m.kind === "exact" ? "Exact" : "Vendor/model";
		row.appendChild(kindCell);
		const selectorCell = document.createElement("td");

		selectorCell.textContent = m.selector;
		row.appendChild(selectorCell);
		const statusCell = document.createElement("td");
		const badge = document.createElement("span");

		badge.className = statusBadge(m.present ? "ok" : "unknown");
		badge.textContent = m.present ? "present (" + m.resolved_ids.length + ")" : "not present";
		statusCell.appendChild(badge);
		row.appendChild(statusCell);

		const usedByCell = document.createElement("td");
		const usingNames = containersUsingDeviceIds(m.resolved_ids);

		if (usingNames.length === 0) {
			usedByCell.textContent = "-";
		} else {
			usingNames.forEach((cname, i) => {
				if (i > 0)
					usedByCell.appendChild(document.createTextNode(", "));
				usedByCell.appendChild(treeLink("#containers/" + encodeURIComponent(cname), cname, ""));
			});
		}
		row.appendChild(usedByCell);

		const actionCell = document.createElement("td");
		const removeBtn = document.createElement("button");

		removeBtn.type = "button";
		removeBtn.className = "button-danger";
		removeBtn.textContent = "Remove";
		removeBtn.addEventListener("click", () => removeDeviceMap(m.name));
		actionCell.appendChild(removeBtn);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function removeDeviceMap(name) {
	try {
		await apiRequest("DELETE", CIX_API.deleteDeviceMap(name));
		clearStatus();
		await refreshDeviceMaps();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove device mapping " + name + ": " + e.message, true);
	}
}

/* ---------- Disks (multi-disk management: ADR-0071/ADR-0102/ADR-0104) ---------- */

async function refreshDisks() {
	const data = await apiRequest("GET", CIX_API.listStorage());

	cache.storage = data.storage;
	if (onPageOf("storage"))
		renderDisks();
	/* Keeps the "Disks > Assign Disk Role" modal's own disk select current
	 * even when opened from the header dropdown rather than a specific
	 * disk row's own "Assign role…" shortcut (which sets a value
	 * afterward, same populate-then-select order populateContainerForm
	 * DeviceLists()/refreshDevices() already establishes). */
	populateDiskRoleSelect();
}

async function refreshDiskRoles() {
	const data = await apiRequest("GET", CIX_API.listStorageRoles());

	cache.storageRoles = data.storage_roles;
	if (onPageOf("storage"))
		renderDisks();
	populateDiskRoleSelect();
}

function storageRoleFor(diskName) {
	return cache.storageRoles.find((r) => r.disk_name === diskName) || null;
}

/*
 * Format status is per-disk (GET /disks/{name}/format), unlike every
 * other cached resource here which is one list call -- fetched only
 * while the Disks page is actually showing (the same "only while this
 * page is open" guard the other per-page refreshers use), and only for
 * disks that could ever have a job at all
 * (role-assigned, non-OS disks) rather than every disk on the box, to
 * keep this bounded regardless of how many disks exist.
 */
async function refreshDiskFormatStatuses() {
	if (!onPageOf("storage"))
		return;

	const candidates = cache.storage.filter((d) => !d.protected && storageRoleFor(d.name) !== null);

	for (const d of candidates) {
		try {
			cache.diskFormatStatus[d.name] = await apiRequest("GET", CIX_API.getStorageFormatStatus(d.name));
		} catch (e) {
			/* Transient -- next poll tick tries again; the row just keeps
			 * showing whatever status it last had. */
		}
	}
	if (onPageOf("storage"))
		renderDisks();
}

/* ---------- Storage placement: state (ADR-0141 Phase 2) + logs (Phase 3) ----------
 * Both kinds share an identical shape (one daemon-side storage_kind
 * each, its own independent migration job slot) -- a single
 * kind-parameterized set of functions here, matching cli/src/main.c's
 * own cmd_storage_kind() refactor, rather than duplicating this block
 * a second time for "logs". */

/*
 * ADR-0218: each kind carries its two CONTRACT paths rather than a URL
 * segment to interpolate. It used to hold endpoint: "state-storage" and
 * build "/v1/system/" + k.endpoint -- a path no generated helper covers,
 * which would 404 silently the day an endpoint is renamed. `role` stays
 * a plain string: it is a disk-role name sent in a body, not a path.
 */
const STORAGE_KINDS = {
	logs: { showPath: CIX_API.getLogStorage, migratePath: CIX_API.migrateLogStorage,
	        migrateStatusPath: CIX_API.getLogStorageMigrateStatus,
	        cacheKey: "logStorage", statusCacheKey: "logStorageMigrate",
	        currentId: "ls-current", statusId: "ls-migrate-status", selectId: "ls-target-disk",
	        formId: "ls-migrate-form", label: "Log storage", role: "log-storage" },
	rebuildable: { showPath: CIX_API.getRebuildableStorage,
	               migrateStatusPath: CIX_API.getRebuildableStorageMigrateStatus,
	               migratePath: CIX_API.migrateRebuildableStorage, cacheKey: "rebuildableStorage",
	               statusCacheKey: "rebuildableStorageMigrate", currentId: "rs-current",
	               statusId: "rs-migrate-status", selectId: "rs-target-disk", formId: "rs-migrate-form",
	               label: "Rebuildable storage", role: "rebuildable-storage" },
};

async function refreshStoragePlacement(kind) {
	const k = STORAGE_KINDS[kind];

	cache[k.cacheKey] = await apiRequest("GET", k.showPath());
	if (onPageOf("storage"))
		renderStoragePlacement(kind);
}

async function refreshStoragePlacementMigrate(kind) {
	const k = STORAGE_KINDS[kind];

	if (!onPageOf("storage"))
		return;
	try {
		cache[k.statusCacheKey] = await apiRequest("GET", k.migrateStatusPath());
	} catch (e) {
		/* Transient -- next poll tick tries again. */
	}
	if (onPageOf("storage"))
		renderStoragePlacement(kind);
}

function renderStoragePlacement(kind) {
	const k = STORAGE_KINDS[kind];
	const current = document.getElementById(k.currentId);

	current.textContent = k.label + ": " + (cache[k.cacheKey].disk || "default OS-disk placement");

	const select = document.getElementById(k.selectId);
	const prevValue = select.value;

	select.textContent = "";
	{
		const opt = document.createElement("option");

		opt.value = "";
		opt.textContent = "(default OS-disk placement)";
		select.appendChild(opt);
	}
	for (const d of cache.storage) {
		const role = storageRoleFor(d.name);

		if (d.protected || role === null || role.role !== k.role)
			continue;
		const opt = document.createElement("option");

		opt.value = d.name;
		opt.textContent = d.name + (d.model ? " (" + d.model + ")" : "");
		select.appendChild(opt);
	}
	select.value = prevValue;

	const statusP = document.getElementById(k.statusId);
	const m = cache[k.statusCacheKey];

	if (!m || m.state === "none") {
		statusP.hidden = true;
	} else {
		statusP.hidden = false;
		statusP.textContent =
			"Migration: " +
			m.state +
			(m.disk ? " (" + m.disk + ")" : "") +
			(m.state === "failed" && m.error ? " -- " + m.error : "");
	}
}

function renderAllStoragePlacements() {
	renderStoragePlacement("logs");
	renderStoragePlacement("rebuildable");
}

for (const kind of Object.keys(STORAGE_KINDS)) {
	const k = STORAGE_KINDS[kind];

	document.getElementById(k.formId).addEventListener("submit", async (event) => {
		event.preventDefault();

		const disk = document.getElementById(k.selectId).value;

		try {
			cache[k.statusCacheKey] = await apiRequest("POST", k.migratePath(), {
				disk: disk === "" ? null : disk,
			});
			clearStatus();
			renderStoragePlacement(kind);
		} catch (e) {
			showStatus("Failed to start " + k.label.toLowerCase() + " migration: " + e.message, true);
		}
	});
}

/*
 * ADR-0142 Section 4: one container's own overlay-storage placement --
 * same shape as STORAGE_KINDS above (current placement text, migrate
 * form populated from container-storage-role disks, inline migration
 * status), but keyed by whichever container's detail view is
 * currently open rather than a fixed daemon-wide slot, since cache.storage/
 * storageRoleFor() are already kept fresh by the global poll loop
 * regardless of which view is active.
 */
async function refreshContainerStorageMigrate(name) {
	if (currentContainerDetailName !== name)
		return;
	try {
		cache.containerStorageMigrate = await apiRequest("GET", CIX_API.getContainerStorageMigrateStatus(name));
	} catch (e) {
		cache.containerStorageMigrate = null;
	}
	if (currentContainerDetailName === name)
		renderContainerStorage(name);
}

function renderContainerStorage(name) {
	const c = cache.containers.find((x) => x.name === name);
	const current = document.getElementById("cd-storage-current");

	if (!c)
		return;
	current.textContent = "Storage: " + (c.disk || "default OS-disk placement");

	const select = document.getElementById("cd-storage-target-disk");
	const prevValue = select.value;

	select.textContent = "";
	{
		const opt = document.createElement("option");

		opt.value = "";
		opt.textContent = "(default OS-disk placement)";
		select.appendChild(opt);
	}
	for (const d of cache.storage) {
		const role = storageRoleFor(d.name);

		if (d.protected || role === null || role.role !== "container-storage")
			continue;
		const opt = document.createElement("option");

		opt.value = d.name;
		opt.textContent = d.name + (d.model ? " (" + d.model + ")" : "");
		select.appendChild(opt);
	}
	select.value = prevValue;

	const statusP = document.getElementById("cd-storage-migrate-status");
	const m = cache.containerStorageMigrate;

	if (!m || m.state === "none") {
		statusP.hidden = true;
	} else {
		statusP.hidden = false;
		statusP.textContent =
			"Migration: " +
			m.state +
			(m.disk ? " (" + m.disk + ")" : "") +
			(m.state === "failed" && m.error ? " -- " + m.error : "");
	}
}

document.getElementById("cd-storage-migrate-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	if (currentContainerDetailName === null)
		return;

	const name = currentContainerDetailName;
	const disk = document.getElementById("cd-storage-target-disk").value;

	try {
		cache.containerStorageMigrate = await apiRequest("POST", CIX_API.migrateContainerStorage(name),
			{ disk: disk === "" ? null : disk }
		);
		clearStatus();
		renderContainerStorage(name);
	} catch (e) {
		showStatus("Failed to start storage migration for " + name + ": " + e.message, true);
	}
});

/*
 * Whole disks only. Partitions used to be listed here as peers of the
 * disks they belong to, which is what made vda, vda1..vda5, sda and sr0
 * read as one flat set of eight equivalent things. A partition now
 * appears on its parent disk's own page, where it can actually be acted
 * on.
 */
function wholeDisks() {
	return cache.storage.filter((d) => !d.is_partition);
}

function partitionsOf(diskName) {
	return cache.storage.filter((d) => d.is_partition && d.parent_disk === diskName);
}

function renderDisks() {
	const body = document.getElementById("disks-body");
	const disks = wholeDisks();

	body.textContent = "";
	if (disks.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 9;
		cell.className = "empty";
		cell.textContent = "No disks found";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}
	for (const d of disks)
		body.appendChild(diskListRow(d));
}

/* One row in the disks list: identity and state only. Every action
 * lives on the disk's own page, so a destructive button is never one
 * stray click away in a list. */
function diskListRow(d) {
	const row = document.createElement("tr");
	const role = storageRoleFor(d.name);
	const parts = partitionsOf(d.name);
	const nameCell = document.createElement("td");
	const link = document.createElement("a");

	link.href = "#storage/" + encodeURIComponent(d.name);
	link.textContent = d.name;
	nameCell.appendChild(link);
	if (parts.length > 0) {
		const note = document.createElement("span");

		note.className = "hint";
		note.textContent = " (" + parts.length + " partition" + (parts.length === 1 ? "" : "s") + ")";
		nameCell.appendChild(note);
	}
	row.appendChild(nameCell);

	for (const text of [d.model || "-", formatBytes(d.size_bytes)]) {
		const td = document.createElement("td");

		td.textContent = text;
		row.appendChild(td);
	}

	const osCell = document.createElement("td");

	if (d.is_os_disk) {
		const badge = document.createElement("span");

		badge.className = statusBadge("paused");
		badge.textContent = "OS disk";
		osCell.appendChild(badge);
	} else {
		osCell.textContent = "-";
	}
	row.appendChild(osCell);

	const roleCell = document.createElement("td");

	roleCell.textContent = role ? role.role : d.part_label || "-";
	row.appendChild(roleCell);

	/* Issue #90: read from the device's own superblock, so it is
	 * answered for an unroled disk too. Empty means genuinely no
	 * recognised filesystem, which is different from unknown. */
	const fsCell = document.createElement("td");

	fsCell.textContent = d.fs_type || (d.has_mounted_partition ? "(partitioned)" : "none");
	row.appendChild(fsCell);

	const mountedCell = document.createElement("td");

	if (d.mounted)
		mountedCell.textContent = d.mount_path;
	else if (d.has_mounted_partition)
		mountedCell.textContent = "partition mounted";
	else
		mountedCell.textContent = "-";
	row.appendChild(mountedCell);

	/* ADR-0142: real statvfs(2) usage, only meaningful while mounted. */
	const usageCell = document.createElement("td");

	usageCell.textContent = d.mounted
		? formatBytes(d.used_bytes) + " used / " + formatBytes(d.free_bytes) + " free"
		: "-";
	row.appendChild(usageCell);

	const ioCell = document.createElement("td");

	ioCell.textContent = "r=" + d.reads_completed + " w=" + d.writes_completed;
	row.appendChild(ioCell);

	return row;
}

function diskRow(d) {
	const row = document.createElement("tr");

	const nameCell = document.createElement("td");

	nameCell.textContent = d.name;
	row.appendChild(nameCell);

	const modelCell = document.createElement("td");

	modelCell.textContent = d.model || "-";
	row.appendChild(modelCell);

	const sizeCell = document.createElement("td");

	sizeCell.textContent = formatBytes(d.size_bytes);
	row.appendChild(sizeCell);

	const osCell = document.createElement("td");

	if (d.is_os_disk) {
		const badge = document.createElement("span");

		badge.className = statusBadge("unknown");
		badge.textContent = "OS disk";
		osCell.appendChild(badge);
	} else {
		osCell.textContent = "-";
	}
	row.appendChild(osCell);

	const mountedCell = document.createElement("td");
	const mountedBadge = document.createElement("span");

	mountedBadge.className = statusBadge(d.mounted ? "ok" : "unknown");
	mountedBadge.textContent = d.mounted ? d.mount_path : "not mounted";
	mountedCell.appendChild(mountedBadge);
	row.appendChild(mountedCell);

	/* ADR-0142: real statvfs(2) usage, only meaningful while mounted. */
	const usageCell = document.createElement("td");

	usageCell.textContent = d.mounted
		? formatBytes(d.used_bytes) + " used / " + formatBytes(d.free_bytes) + " free"
		: "-";
	row.appendChild(usageCell);

	/* ADR-0142: real, live, monotonically-increasing counters straight
	 * from /sys/block/<name>/stat -- not a rate (no client-side delta
	 * tracking here, each poll just shows the current lifetime totals,
	 * matching this table's own "state right now" convention). */
	const ioCell = document.createElement("td");

	ioCell.textContent = "r=" + d.reads_completed + " w=" + d.writes_completed + " busy=" + d.io_time_ms + "ms";
	row.appendChild(ioCell);

	const role = storageRoleFor(d.name);
	const roleCell = document.createElement("td");

	roleCell.textContent = role ? role.role : d.part_label || "-";
	row.appendChild(roleCell);

	const formatStatus = cache.diskFormatStatus[d.name];
	const formatCell = document.createElement("td");

	if (formatStatus && formatStatus.state !== "none") {
		const badge = document.createElement("span");

		badge.className =
			statusBadge(formatStatus.state === "ready" ? "ok" : formatStatus.state === "failed" ? "error" : "paused");
		badge.textContent = formatStatus.state === "failed" ? "failed: " + formatStatus.error : formatStatus.state;
		formatCell.appendChild(badge);
	} else {
		formatCell.textContent = "-";
	}
	row.appendChild(formatCell);

	const actionCell = document.createElement("td");

	if (d.protected) {
		/* Never a role/format candidate -- nothing to offer. */
	} else if (!role) {
		const assignBtn = document.createElement("button");

		assignBtn.type = "button";
		assignBtn.textContent = "Assign role…";
		assignBtn.addEventListener("click", () => {
			populateDiskRoleSelect(true);
			document.getElementById("drf-disk-name").value = d.name;
			document.getElementById("drf-role").value = "container-storage";
			openModal("diskrole-form", "Assign disk role");
		});
		actionCell.appendChild(assignBtn);
	} else if (formatStatus && formatStatus.state === "running") {
		actionCell.appendChild(document.createTextNode("(formatting…)"));
	} else {
		const removeRoleBtn = document.createElement("button");

		removeRoleBtn.type = "button";
		removeRoleBtn.className = "button-small";
		removeRoleBtn.textContent = "Remove role";
		removeRoleBtn.addEventListener("click", () => removeDiskRole(d.name));
		actionCell.appendChild(removeRoleBtn);

		const fsSelect = buildFsSelect(d.name);
		const formatBtn = document.createElement("button");

		formatBtn.type = "button";
		formatBtn.className = "button-danger button-small";
		formatBtn.textContent = "Format…";
		formatBtn.addEventListener("click", () => formatDisk(d.name, fsSelect.value));

		actionCell.appendChild(fsSelect);
		actionCell.appendChild(formatBtn);
	}
	row.appendChild(actionCell);

	return row;
}

/* ---- One block device's page: a whole disk or a partition ---- */

let currentDiskDetailName = null;

/*
 * Deliberately one page for both kinds. A partition is a block device
 * with its own role, filesystem, mount and usage -- which is exactly
 * why ADR-0158 could reuse diskrole/diskformat for partitions with no
 * changes -- so giving it a lesser page than a disk would be an
 * inconsistency in the UI that does not exist in the system. The
 * Partitions tab is the one thing only a whole disk has, because only a
 * whole disk has a partition table.
 */
function renderDiskDetail(name) {
	const d = cache.storage.find((x) => x.name === name);
	const subtitle = document.getElementById("dd-subtitle");
	const fields = document.getElementById("dd-fields");

	currentDiskDetailName = name;
	if (!d) {
		subtitle.textContent = name + " \u2014 not found.";
		fields.textContent = "";
		return;
	}

	const role = storageRoleFor(d.name);
	const parts = d.is_partition ? [] : partitionsOf(d.name);

	subtitle.textContent = d.is_partition
		? "Partition of " + d.parent_disk +
		  (d.protected ? " — part of the fixed OS layout" : d.is_os_disk ? " — on the OS disk" : "")
		: (d.model || "Block device") + (d.is_os_disk ? " — the OS disk" : "");

	/* Only a whole disk has a partition table, so a partition simply
	 * does not get that tab rather than getting an empty one. */
	document.getElementById("dd-tab-partitions").hidden = d.is_partition;
	if (d.is_partition)
		selectDiskTabIfActive("dd-partitions", "dd-overview");

	renderDiskUsage(d);

	fields.textContent = "";
	fields.appendChild(fieldBlock("Device", d.dev_path));
	fields.appendChild(fieldBlock("Size", formatBytes(d.size_bytes)));
	fields.appendChild(fieldBlock("Kind", d.is_partition ? "partition of " + d.parent_disk : "whole disk"));
	fields.appendChild(fieldBlock("Role", role ? role.role : "none"));
	fields.appendChild(
		fieldBlock("Filesystem", d.fs_type || (parts.length > 0 ? "(partitioned)" : "unformatted"))
	);
	fields.appendChild(
		fieldBlock("Mounted at", d.mounted ? d.mount_path : d.has_mounted_partition ? "(a partition of it is)" : "not mounted")
	);
	if (!d.is_partition)
		fields.appendChild(fieldBlock("Partitions", parts.length === 0 ? "none" : String(parts.length)));
	fields.appendChild(fieldBlock("Removable", d.removable ? "yes" : "no"));
	fields.appendChild(
		fieldBlock(
			"OS disk",
			d.protected
				? "yes — structural, never modified"
				: d.is_os_disk
				  ? "yes — but this partition is ordinary space"
				  : "no",
		),
	);
	fields.appendChild(
		fieldBlock("I/O", "reads " + d.reads_completed + " / writes " + d.writes_completed + " / busy " + d.io_time_ms + "ms")
	);
	{
		/* Which volumes live on this device -- the same derivation the
		 * tree uses, shown here so the page answers it too. */
		const vols = (cache.volumes || []).filter((v) => {
			const holder = deviceHoldingVolume(v);

			return holder !== null && holder.name === d.name;
		});

		fields.appendChild(fieldBlock("Volumes on it", vols.length === 0 ? "none" : vols.map((v) => v.name).join(", ")));
	}

	renderDiskRoleTab(d, role);
	if (!d.is_partition) {
		renderDiskPartitions(d, parts);
		refreshDiskFreeSpace(d);
	}
}

/* Switches away from a tab that has just been hidden, so a partition
 * never lands on the Partitions tab it does not have. */
function selectDiskTabIfActive(hiddenTab, fallbackTab) {
	const view = document.getElementById("view-storage-detail");
	const active = view.querySelector(".tab-bar .tab-button.active");

	if (active && active.dataset.tab === hiddenTab)
		selectDiskTab(fallbackTab);
}

function selectDiskTab(tabName) {
	const view = document.getElementById("view-storage-detail");

	for (const btn of view.querySelectorAll(".tab-bar .tab-button"))
		btn.classList.toggle("active", btn.dataset.tab === tabName);
	for (const panel of view.querySelectorAll(".tab-panel"))
		panel.hidden = panel.dataset.tab !== tabName;
}

/*
 * A real usage gauge, not a pair of numbers. Only meaningful while the
 * device is mounted -- statvfs(2) needs a live mount -- so an unmounted
 * device says why it has no figure rather than showing an empty bar
 * that reads as "0% used".
 */
function renderDiskUsage(d) {
	const host = document.getElementById("dd-usage");

	host.textContent = "";
	if (!d.mounted || d.used_bytes + d.free_bytes === 0) {
		const p = document.createElement("p");

		p.className = "hint";
		p.textContent = d.mounted
			? "Mounted, but the filesystem reported no capacity."
			: "Usage is only known while the device is mounted.";
		host.appendChild(p);
		return;
	}
	const total = d.used_bytes + d.free_bytes;
	const pct = Math.round((d.used_bytes / total) * 100);
	const wrap = document.createElement("div");
	const bar = document.createElement("div");
	const fill = document.createElement("div");
	const label = document.createElement("p");

	wrap.className = "usage-gauge";
	bar.className = "usage-gauge-track";
	fill.className = "usage-gauge-fill" + (pct >= 90 ? " usage-gauge-critical" : pct >= 75 ? " usage-gauge-warn" : "");
	fill.style.width = pct + "%";
	bar.appendChild(fill);
	label.className = "hint";
	label.textContent =
		formatBytes(d.used_bytes) + " used of " + formatBytes(total) + " (" + pct + "%) — " + formatBytes(d.free_bytes) + " free";
	wrap.appendChild(bar);
	wrap.appendChild(label);
	host.appendChild(wrap);
}

/*
 * Role and format, for whichever device this page is showing. This is
 * the tab that answers "what is this for, and how do I change it" --
 * previously scattered between a list row and a whole-disk tab that a
 * partition never had.
 */
/*
 * The single source of truth for which actions a disk or partition
 * offers. Read by BOTH the detail page (renderDiskRoleTab) and the
 * tree's right-click menu (contextMenuItemsFor), so the two can never
 * drift apart again -- which is exactly what happened when the online
 * data-directory grow (#94) landed in one and not the other. Every flag
 * mirrors a rule the daemon actually enforces; the UI never offers an
 * action the daemon would 409.
 */
function diskActionEligibility(d) {
	const e = {
		addPartition: false,
		assignRole: false,
		removeRole: false,
		format: false,
		unmount: false,
		grow: false,
		del: false,
		delDisabled: false,
	};

	if (!d)
		return e;
	/*
	 * The OS disk itself: appending a partition into its free space is
	 * safe and is the point of #140, while a role, a format and a table
	 * rewrite are all refused -- which is why the daemon reports it
	 * protected. Answered before the protected gate so the one legal
	 * action is not lost with the refused ones.
	 */
	if (d.is_os_disk && !d.is_partition) {
		e.addPartition = true;
		return e;
	}
	/* The four structural OS partitions: nothing at all (#140). */
	if (d.protected)
		return e;
	/*
	 * A partitioned whole disk's space belongs to its partitions -- give
	 * THOSE roles and formats on their own pages; the disk itself only
	 * takes new partitions into any remaining free space.
	 */
	if (!d.is_partition && partitionsOf(d.name).length > 0) {
		e.addPartition = true;
		return e;
	}
	/* An unpartitioned whole non-OS disk, or an ordinary partition. */
	const role = storageRoleFor(d.name);

	e.assignRole = !role;
	e.removeRole = !!role;
	e.format = !!role;
	e.unmount = !!d.mounted;
	/*
	 * Grow (partitions only): ext4, btrfs and unformatted are all grown
	 * (#163); a mounted partition is offered only when it is btrfs,
	 * which grows online in place -- the one path to extending the data
	 * directory (#94). A mounted ext4 would be refused.
	 */
	e.grow =
		!!d.is_partition &&
		(d.fs_type === "" || d.fs_type === "ext4" || d.fs_type === "btrfs") &&
		(!d.mounted || d.fs_type === "btrfs");
	/*
	 * Delete (partitions only). A mounted partition would 409, so it is
	 * offered disabled with the reason rather than hidden -- naming why
	 * beats a silently missing action.
	 */
	e.del = !!d.is_partition;
	e.delDisabled = !!d.mounted;
	return e;
}

function renderDiskRoleTab(d, role) {
	const note = document.getElementById("dd-role-note");
	const current = document.getElementById("dd-role-current");
	const actions = document.getElementById("dd-role-actions");
	const formatStatus = cache.diskFormatStatus[d.name];

	current.textContent = "";
	actions.textContent = "";

	if (d.protected) {
		note.textContent =
			"This is one of the four structural partitions of the OS layout \u2014 the ESP, both root slots and /config. It is never given a role, formatted or unmounted from here.";
		return;
	}
	if (!d.is_partition && partitionsOf(d.name).length > 0) {
		note.textContent =
			"This disk is partitioned, so its space belongs to its partitions — give those roles and formats on their own pages. To use the whole device directly, delete every partition first.";
		return;
	}
	note.textContent = role
		? "Assigning a role is non-destructive and reversible. Formatting wipes every byte on this device."
		: "A role says what this device is for. It has to be assigned before the device can be formatted or mounted, and assigning one changes nothing on disk.";

	const summary = document.createElement("table");
	const summaryBody = document.createElement("tbody");

	summary.className = "detail-table";
	summary.appendChild(summaryBody);
	summaryBody.appendChild(fieldBlock("Role", role ? role.role : "none"));
	summaryBody.appendChild(fieldBlock("Filesystem", d.fs_type || "unformatted"));
	summaryBody.appendChild(fieldBlock("Mounted at", d.mounted ? d.mount_path : "not mounted"));
	if (formatStatus && formatStatus.state !== "none")
		summaryBody.appendChild(
			fieldBlock("Last format", formatStatus.state === "failed" ? "failed: " + formatStatus.error : formatStatus.state)
		);
	current.appendChild(summary);

	if (formatStatus && formatStatus.state === "running") {
		const p = document.createElement("p");

		p.className = "hint";
		p.textContent = "Formatting…";
		actions.appendChild(p);
		return;
	}

	/*
	 * Every button below is gated by diskActionEligibility(d) -- the
	 * same source the tree's right-click menu reads -- so the two
	 * surfaces cannot disagree about what this disk can do.
	 */
	const e = diskActionEligibility(d);

	if (e.assignRole) {
		const assignBtn = document.createElement("button");

		assignBtn.type = "button";
		assignBtn.textContent = "Assign a role…";
		assignBtn.addEventListener("click", () => openAssignRole(d.name));
		actions.appendChild(assignBtn);
	}
	if (e.removeRole) {
		const removeBtn = document.createElement("button");

		removeBtn.type = "button";
		removeBtn.textContent = "Remove role";
		removeBtn.addEventListener("click", () => removeDiskRole(d.name));
		actions.appendChild(removeBtn);
	}
	if (e.format) {
		const fsSelect = buildFsSelect(d.name);
		const formatBtn = document.createElement("button");

		formatBtn.type = "button";
		formatBtn.className = "button-danger";
		formatBtn.textContent = "Format…";
		formatBtn.addEventListener("click", () => formatDisk(d.name, fsSelect.value));
		actions.appendChild(fsSelect);
		actions.appendChild(formatBtn);
	}
	if (e.unmount) {
		const unmountBtn = document.createElement("button");

		unmountBtn.type = "button";
		unmountBtn.textContent = "Unmount";
		unmountBtn.addEventListener("click", () => unmountDisk(d.name));
		actions.appendChild(unmountBtn);
	}
	if (e.grow) {
		const growBtn = document.createElement("button");

		growBtn.type = "button";
		growBtn.textContent = "Grow…";
		growBtn.addEventListener("click", () => growPartition(d));
		actions.appendChild(growBtn);
	}
	if (e.del) {
		const delBtn = document.createElement("button");

		delBtn.type = "button";
		delBtn.className = "button-danger";
		delBtn.textContent = "Delete this partition";
		delBtn.disabled = e.delDisabled;
		delBtn.title = e.delDisabled ? "Unmount it first." : "";
		delBtn.addEventListener("click", () => deletePartition(d.parent_disk, d.name));
		actions.appendChild(delBtn);
	}
}

function openAssignRole(name) {
	populateDiskRoleSelect(true);
	document.getElementById("drf-disk-name").value = name;
	document.getElementById("drf-role").value = "container-storage";
	openModal("diskrole-form", "Assign disk role");
}

/*
 * Issue #140: how the disk is actually carved up, as a donut.
 *
 * A table of partition sizes answers "how big is each" but not "how
 * much of this disk is spoken for", which is the question an operator
 * asks before adding one -- and the question that started this issue,
 * since the free space existed and the platform would not use it.
 *
 * Unallocated space is a segment in its own right rather than an
 * absence. That is the whole point: free space you cannot see is free
 * space you do not know you have.
 *
 * Hand-drawn SVG rather than a charting library -- the dashboard ships
 * no dependencies and this is one circle with dash offsets.
 */
function renderDiskAllocationChart(d, parts) {
	const host = document.getElementById("dd-alloc-chart");

	if (host === null)
		return;
	host.textContent = "";
	if (!d || d.is_partition || !d.size_bytes)
		return;
	/*
	 * A disk with no partitions is not a disk with nothing to show.
	 *
	 * This used to bail on parts.length === 0, so a whole-disk
	 * filesystem -- which is what carries images and artifacts here --
	 * drew nothing at all, while the OS disk drew a full chart. The
	 * question this answers is "how much of this disk is spoken for",
	 * and that question is if anything MORE pressing for the disk that
	 * fills up as packages are built. Same argument the issue that
	 * created this chart made: free space you cannot see is free space
	 * you do not know you have.
	 *
	 * A partitioned disk is carved up by its partition table, so the
	 * segments are partitions. An unpartitioned one is carved up by its
	 * own filesystem, so the segments are used and free -- statvfs
	 * figures the daemon already reports (ADR-0142). Nothing to draw if
	 * it is not mounted, since nothing has measured it.
	 */
	const wholeDisk = parts.length === 0;

	if (wholeDisk && (!d.mounted || (!d.used_bytes && !d.free_bytes)))
		return;

	const R = 54;
	const C = 2 * Math.PI * R;
	const SVG_NS = "http://www.w3.org/2000/svg";
	/* Distinct hues, with protected partitions deliberately desaturated
	 * so "cannot be touched" reads at a glance rather than needing the
	 * legend. */
	const hues = [200, 160, 40, 280, 340, 100, 20, 240];
	let allocated = 0;
	const segments = [];

	if (wholeDisk) {
		const used = d.used_bytes || 0;
		const avail = d.free_bytes || 0;

		if (used > 0) {
			segments.push({
				label: "used",
				bytes: used,
				color: "hsl(200,58%,52%)",
				note: d.fs_type || "",
			});
		}
		if (avail > 0) {
			segments.push({
				label: "free",
				bytes: avail,
				color: "var(--muted, #888)",
				note: "available",
				isFree: true,
			});
		}
		/*
		 * used + free is normally a little short of the disk's own
		 * size -- filesystem metadata, and on ext4 the reserved
		 * blocks that f_bavail deliberately excludes. Shown rather
		 * than hidden or silently folded into one of the other two,
		 * because a chart whose segments do not add up to the whole
		 * is the kind of small wrongness that makes someone distrust
		 * the rest of the page.
		 */
		const overhead = Math.max(0, d.size_bytes - used - avail);

		if (overhead > 0 && overhead / d.size_bytes > 0.001) {
			segments.push({
				label: "filesystem overhead",
				bytes: overhead,
				color: "var(--muted, #888)",
				note: "metadata and reserved blocks",
				isFree: true,
			});
		}
	} else {
		parts.forEach((p, i) => {
			const bytes = p.size_bytes || 0;

			allocated += bytes;
			segments.push({
				label: p.name,
				bytes: bytes,
				color: p.protected ? "hsl(" + hues[i % hues.length] + ",12%,55%)"
				                   : "hsl(" + hues[i % hues.length] + ",58%,52%)",
				note: p.protected ? "protected" : p.part_label || "",
			});
		});
		const free = Math.max(0, d.size_bytes - allocated);

		if (free > 0) {
			segments.push({
				label: "unallocated",
				bytes: free,
				color: "var(--muted, #888)",
				note: "available",
				isFree: true,
			});
		}
	}

	const wrap = document.createElement("div");

	wrap.className = "alloc-chart";

	const svg = document.createElementNS(SVG_NS, "svg");

	svg.setAttribute("viewBox", "0 0 140 140");
	svg.setAttribute("width", "140");
	svg.setAttribute("height", "140");
	svg.setAttribute("role", "img");
	svg.setAttribute("aria-label", (wholeDisk ? "Filesystem usage for " : "Partition allocation for ") + d.name);

	let offset = 0;

	for (const seg of segments) {
		const circle = document.createElementNS(SVG_NS, "circle");
		const len = (seg.bytes / d.size_bytes) * C;

		circle.setAttribute("cx", "70");
		circle.setAttribute("cy", "70");
		circle.setAttribute("r", String(R));
		circle.setAttribute("fill", "none");
		circle.setAttribute("stroke", seg.color);
		circle.setAttribute("stroke-width", "18");
		circle.setAttribute("stroke-dasharray", len + " " + (C - len));
		circle.setAttribute("stroke-dashoffset", String(-offset));
		/* Start at twelve o'clock; the default is three, which reads
		 * as an arbitrary rotation rather than a whole. */
		circle.setAttribute("transform", "rotate(-90 70 70)");
		if (seg.isFree)
			circle.setAttribute("opacity", "0.35");
		svg.appendChild(circle);
		offset += len;
	}

	wrap.appendChild(svg);

	const legend = document.createElement("ul");

	legend.className = "alloc-legend";
	for (const seg of segments) {
		const li = document.createElement("li");
		const swatch = document.createElement("span");
		const pct = d.size_bytes > 0 ? (seg.bytes / d.size_bytes) * 100 : 0;

		swatch.className = "alloc-swatch";
		swatch.style.background = seg.color;
		if (seg.isFree)
			swatch.style.opacity = "0.35";
		li.appendChild(swatch);
		li.appendChild(
			document.createTextNode(
				seg.label + " \u2014 " + formatBytes(seg.bytes) + " (" +
					(pct < 1 ? "<1" : Math.round(pct)) + "%)" +
					(seg.note ? " \u00b7 " + seg.note : "")
			)
		);
		legend.appendChild(li);
	}
	wrap.appendChild(legend);
	host.appendChild(wrap);
}

/*
 * The partitions on this disk, as a read-only overview -- every action
 * on a partition lives on that partition's own page, which is the whole
 * point of a partition having one. Clicking a row goes there.
 */
function renderDiskPartitions(d, parts) {
	const body = document.getElementById("dd-partitions-body");
	const note = document.getElementById("dd-part-note");
	const actions = document.getElementById("dd-part-actions");

	/*
	 * Issue #140: the OS disk can be APPENDED to, just not rewritten.
	 *
	 * This block used to be hidden outright on the OS disk, which is
	 * the operator's literal complaint -- they had deliberately sized
	 * the containers partition smaller than the disk so the remainder
	 * stayed usable, and the dashboard offered no way to use it.
	 * Appending a partition cannot touch the ESP or either root slot;
	 * rewriting the table can, so only that stays hidden here.
	 */
	const tableActions = document.getElementById("dd-table-actions");

	actions.hidden = false;
	if (tableActions !== null)
		tableActions.hidden = d.is_os_disk;

	note.textContent = d.is_os_disk
		? "This is the OS disk. Its first four partitions (ESP, both root slots, /config) are fixed and are never offered for delete or resize \u2014 destroying one makes this machine unbootable. Everything after them is ordinary space: new partitions can be added here, and rewriting the whole table is the one thing that stays refused."
		: d.has_mounted_partition
		  ? "Something on this disk is mounted, so the partition table cannot be rewritten and a mounted partition cannot be deleted. Unmount it first."
		  : "Click a partition to give it a role, format it, or delete it.";

	renderDiskAllocationChart(d, parts);

	body.textContent = "";
	if (parts.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 7;
		cell.className = "empty";
		cell.textContent = d.is_os_disk ? "No partitions reported" : "No partitions — this disk is unpartitioned";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}
	for (const p of parts) {
		const row = document.createElement("tr");
		const role = storageRoleFor(p.name);
		const nameCell = document.createElement("td");

		nameCell.appendChild(treeLink("#storage/" + encodeURIComponent(p.name), p.name, ""));
		row.appendChild(nameCell);
		for (const text of [
			formatBytes(p.size_bytes),
			role ? role.role : p.part_label || "-",
			/*
			 * Its own column, not a suffix on the role. Protection
			 * answers a different question from "what is this for",
			 * and hanging it off the label meant a partition with a
			 * role never showed it at all.
			 */
			p.protected ? "protected" : "-",
			p.fs_type || "unformatted",
			p.mounted ? p.mount_path : "-",
			p.mounted && p.used_bytes + p.free_bytes > 0
				? Math.round((p.used_bytes / (p.used_bytes + p.free_bytes)) * 100) + "% used"
				: "-",
		]) {
			const td = document.createElement("td");

			td.textContent = text;
			row.appendChild(td);
		}
		body.appendChild(row);
	}
}

/*
 * Issue #94: grow a partition into the free space immediately after it.
 * Only that space counts -- free space elsewhere on the disk cannot
 * extend this partition -- so the prompt names the real limit rather
 * than the disk's total free space.
 */
async function growPartition(p) {
	let room = null;

	try {
		const fs = await apiRequest("GET", CIX_API.getStorageFreeSpace(p.parent_disk));
		const endSector = p.start_sector + p.size_bytes / 512;
		const after = (fs.extents || []).find((e) => e.start_sector === endSector);

		room = after ? after.bytes : 0;
	} catch (e) {
		/* Advisory only; the daemon validates for real. */
	}
	if (room === 0) {
		showStatus(
			"There is no free space immediately after " +
				p.name +
				" — free space elsewhere on the disk cannot extend it.",
			true
		);
		return;
	}
	const roomMib = room === null ? null : Math.floor(room / (1024 * 1024));
	const answer = prompt(
		"Grow " +
			p.name +
			" by how much?\n\n" +
			"Enter a new TOTAL size in MiB, or leave blank to use all " +
			(roomMib === null ? "free space after it" : roomMib + " MiB available after it") +
			".\n\nCurrent size: " +
			Math.floor(p.size_bytes / (1024 * 1024)) +
			" MiB. A partition can only be grown, never shrunk.",
		""
	);

	if (answer === null)
		return;
	const body = { size_mib: answer.trim() === "" ? 0 : parseInt(answer, 10) };

	if (answer.trim() !== "" && (!body.size_mib || body.size_mib <= 0)) {
		showStatus("That is not a valid size.", true);
		return;
	}
	try {
		await apiRequest("POST", CIX_API.resizeStoragePartition(p.parent_disk, p.name),
			body
		);
		clearStatus();
		await refreshDisks();
		renderDiskDetail(p.name);
		renderTree();
	} catch (e) {
		showStatus("Failed to grow partition: " + e.message, true);
	}
}

async function deletePartition(diskName, partitionName) {
	if (!confirm('Delete partition "' + partitionName + '"? Everything on it is destroyed and this cannot be undone.'))
		return;
	try {
		await apiRequest("DELETE", CIX_API.deleteStoragePartition(diskName, partitionName));
		clearStatus();
		await refreshDisks();
		/* The deleted partition's own page no longer exists, so go back
		 * to the disk that held it rather than rendering "not found". */
		location.hash = "#storage/" + encodeURIComponent(diskName);
		renderTree();
	} catch (e) {
		showStatus("Failed to delete partition: " + e.message, true);
	}
}

async function unmountDisk(name) {
	try {
		await apiRequest("POST", CIX_API.unmountStorage(name), {
			confirm_disk_name: name,
		});
		clearStatus();
		await refreshDisks();
		if (currentDiskDetailName !== null)
			renderDiskDetail(currentDiskDetailName);
	} catch (e) {
		showStatus("Failed to unmount: " + e.message, true);
	}
}

/*
 * Issue #95: what will actually fit, asked of the daemon rather than
 * computed here. Subtracting the partition sizes from the disk size
 * looks equivalent and is not -- it misses alignment, the GPT's own
 * reserved areas, and any gap an earlier delete left behind.
 *
 * Fetched when a disk page renders, never in the poll loop: the
 * endpoint forks sfdisk.
 */
let diskFreeSpace = {};

async function refreshDiskFreeSpace(d) {
	const note = document.getElementById("dd-free-space");
	const sizeInput = document.getElementById("dd-part-size");

	/*
	 * The OS disk is NOT skipped. #140 made appending into its free
	 * space legal and the partition-table tab's own note tells the
	 * operator so; skipping the lookup here meant the figure that
	 * would let them act on it was never fetched.
	 */
	if (d.is_partition)
		return;
	try {
		const fs = await apiRequest("GET", CIX_API.getStorageFreeSpace(d.name));

		diskFreeSpace[d.name] = fs;
		if (!fs.has_partition_table) {
			note.textContent = "This disk has no partition table yet — write one below before adding partitions.";
			sizeInput.max = "";
			return;
		}
		const largestMib = Math.floor(fs.largest_free_bytes / (1024 * 1024));

		sizeInput.max = String(largestMib);
		sizeInput.placeholder = "(rest of disk — up to " + largestMib + " MiB)";
		if (fs.total_free_bytes > fs.largest_free_bytes) {
			note.textContent =
				formatBytes(fs.total_free_bytes) +
				" free in total, but split across " +
				fs.extents.length +
				" gaps — the largest single gap is " +
				formatBytes(fs.largest_free_bytes) +
				", which is the most one new partition can take.";
		} else {
			note.textContent = formatBytes(fs.largest_free_bytes) + " free (" + largestMib + " MiB).";
		}
	} catch (e) {
		/* Never block the form on this: the daemon still validates the
		 * real request, so a failed advisory check must not stop
		 * someone submitting one. */
		note.textContent = "Could not read free space: " + e.message;
	}
}

document.getElementById("dd-write-table").addEventListener("click", async () => {
	const name = currentDiskDetailName;

	if (name === null)
		return;
	if (
		!confirm(
			'Write a fresh empty GPT table to "' +
				name +
				'"? Every partition on this disk and everything on them is destroyed. This cannot be undone.'
		)
	)
		return;
	try {
		await apiRequest("POST", CIX_API.createStoragePartitionTable(name), {
			confirm_disk_name: name,
		});
		clearStatus();
		await refreshDisks();
		renderDiskDetail(name);
		renderTree();
	} catch (e) {
		showStatus("Failed to write partition table: " + e.message, true);
	}
});

document.getElementById("dd-add-partition-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	const name = currentDiskDetailName;

	if (name === null)
		return;
	const partName = document.getElementById("dd-part-name").value.trim();
	const sizeText = document.getElementById("dd-part-size").value.trim();
	const body = { name: partName };

	if (sizeText !== "")
		body.size_mib = parseInt(sizeText, 10);

	/* Catch a too-large size here rather than letting it come back as
	 * an sfdisk rejection -- the daemon still checks, this just says so
	 * before the round trip. */
	const fs = diskFreeSpace[name];

	if (fs && fs.has_partition_table && body.size_mib) {
		const largestMib = Math.floor(fs.largest_free_bytes / (1024 * 1024));

		if (body.size_mib > largestMib) {
			showStatus("That is larger than the biggest free gap on this disk (" + largestMib + " MiB).", true);
			return;
		}
	}
	try {
		await apiRequest("POST", CIX_API.addStoragePartition(name), body);
		clearStatus();
		document.getElementById("dd-add-partition-form").reset();
		await refreshDisks();
		renderDiskDetail(name);
		renderTree();
	} catch (e) {
		showStatus("Failed to add partition: " + e.message, true);
	}
});

/*
 * `force` is for the paths that OPEN the dialog: they populate and then
 * select a value, so they need the options rebuilt now. The poll path
 * passes nothing and rebuilds only when the disks or their roles
 * actually changed -- repopulating a <select> under someone who is
 * using it throws away what they had chosen.
 */
function populateDiskRoleSelect(force) {
	const select = document.getElementById("drf-disk-name");

	if (!force && !dataChanged("diskrole-options",
	                           { storage: cache.storage, roles: cache.storageRoles }))
		return;
	select.textContent = "";
	for (const d of cache.storage) {
		if (d.protected || storageRoleFor(d.name) !== null)
			continue;
		const opt = document.createElement("option");

		opt.value = d.name;
		opt.textContent = d.name + (d.model ? " (" + d.model + ")" : "");
		select.appendChild(opt);
	}
}

document.getElementById("diskrole-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const diskName = document.getElementById("drf-disk-name").value;
	const role = document.getElementById("drf-role").value;

	if (diskName === "")
		return;
	try {
		await apiRequest("POST", CIX_API.createStorageRole(), { disk_name: diskName, role: role });
		clearStatus();
		document.getElementById("diskrole-form").reset();
		closeModal();
		await refreshDiskRoles();
	} catch (e) {
		showStatus("Failed to assign role to " + diskName + ": " + e.message, true);
	}
});

async function removeDiskRole(diskName) {
	try {
		await apiRequest("DELETE", CIX_API.deleteStorageRole(diskName));
		clearStatus();
		await refreshDiskRoles();
	} catch (e) {
		showStatus("Failed to remove role from " + diskName + ": " + e.message, true);
	}
}

/* Destructive -- wipes every byte of existing content on the disk, per
 * the API's own doc comment. The disk is already unambiguous (this
 * button only ever appears on one specific disk's own row), so a real
 * confirm() dialog is the actual gate here, the same severity class as
 * Kill/Reset-CA-chain elsewhere in this dashboard -- confirm_disk_name
 * is filled in automatically from that same unambiguous context,
 * mirroring cixctl's own "the operator already specified which disk
 * by typing its name once" reasoning (cli/src/main.c's cmd_disks_
 * format()), not asked for a second time as a separate typed field. */
/*
 * The filesystem chosen for a format, remembered per disk.
 *
 * Both format controls are rebuilt from scratch on every render, and a
 * rebuilt <select> starts at its first option -- ext4. So choosing
 * btrfs and then waiting for one poll silently reverted the choice, and
 * the format went ahead as ext4: reported by an operator who asked for
 * btrfs on sda1 and got ext4, with nothing anywhere saying so. A
 * destructive action must never be able to change its own meaning
 * between choosing it and confirming it.
 */
const diskFormatFsChoice = {};

function buildFsSelect(diskName) {
	const select = document.createElement("select");

	for (const fs of ["ext4", "btrfs"]) {
		const opt = document.createElement("option");

		opt.value = fs;
		opt.textContent = fs;
		select.appendChild(opt);
	}
	select.value = diskFormatFsChoice[diskName] || "ext4";
	select.addEventListener("change", () => {
		diskFormatFsChoice[diskName] = select.value;
	});
	return select;
}

async function formatDisk(diskName, fsType) {
	if (!confirm("Format " + diskName + " as " + fsType + "? This destroys every byte of existing content on the disk. This cannot be undone."))
		return;
	try {
		cache.diskFormatStatus[diskName] = await apiRequest("POST", CIX_API.formatStorage(diskName), {
			confirm_disk_name: diskName,
			fs_type: fsType,
		});
		clearStatus();
		renderDisks();
	} catch (e) {
		showStatus("Failed to format " + diskName + ": " + e.message, true);
	}
}

/* ---------- DNS Records ---------- */

/* Set while the DNS record modal form is open in edit mode (task #749) --
 * null means the next submit is a create (POST). */
let dnsRecordEditName = null;

function editDnsRecord(rec) {
	/* Set AFTER openModal() -- it calls closeModal(), which resets this
	 * to null (otherwise the submit handler POSTs instead of PUTs). */
	openModal("dns-record-form", "Edit DNS record");
	dnsRecordEditName = rec.name;
	document.getElementById("df-name").value = rec.name;
	document.getElementById("df-name").readOnly = true;
	document.getElementById("df-ip").value = rec.ip;
	document.getElementById("df-submit").textContent = "Save";
}

function renderDnsRecords(records) {
	const body = document.getElementById("dns-records-body");

	body.textContent = "";
	if (records.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 4;
		cell.className = "empty";
		cell.textContent = "No DNS records";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const rec of records) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.textContent = rec.name;
		row.appendChild(nameCell);

		const ipCell = document.createElement("td");
		ipCell.textContent = rec.ip;
		row.appendChild(ipCell);

		const ownerCell = document.createElement("td");
		ownerCell.textContent = formatOwner(rec.owner);
		row.appendChild(ownerCell);

		const actionCell = document.createElement("td");
		const editButton = document.createElement("button");

		editButton.textContent = "Edit";
		editButton.addEventListener("click", () => editDnsRecord(rec));
		actionCell.appendChild(editButton);

		const rmButton = document.createElement("button");

		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeDnsRecord(rec.name));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshDnsRecords() {
	const data = await apiRequest("GET", CIX_API.listDnsRecords());
	cache.dnsRecords = data.records;
	renderDnsRecords(cache.dnsRecords);
}

async function removeDnsRecord(name) {
	try {
		await apiRequest("DELETE", CIX_API.deleteDnsRecord(name));
		clearStatus();
		await refreshDnsRecords();
	} catch (e) {
		showStatus("Failed to remove DNS record " + name + ": " + e.message, true);
	}
}

/* ---------- DNS Servers ---------- */

function renderDnsServers(servers) {
	const body = document.getElementById("dns-servers-body");

	body.textContent = "";
	if (servers.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 3;
		cell.className = "empty";
		cell.textContent = "No DNS server bindings";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const s of servers) {
		const row = document.createElement("tr");

		const containerCell = document.createElement("td");
		containerCell.textContent = s.container;
		row.appendChild(containerCell);

		const pathCell = document.createElement("td");
		pathCell.textContent = s.hosts_path;
		row.appendChild(pathCell);

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		rmButton.textContent = "Unregister";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeDnsServer(s.container));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshDnsServers() {
	const data = await apiRequest("GET", CIX_API.listDnsServers());
	cache.dnsServers = data.servers;
	renderDnsServers(cache.dnsServers);
}

async function removeDnsServer(container) {
	try {
		await apiRequest("DELETE", CIX_API.deleteDnsServer(container));
		clearStatus();
		await refreshDnsServers();
	} catch (e) {
		showStatus("Failed to unregister DNS server " + container + ": " + e.message, true);
	}
}

/* ---------- LDAP Servers (task #725) ---------- */

function renderLdapServers(servers) {
	const body = document.getElementById("ldap-servers-body");

	body.textContent = "";
	if (servers.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 3;
		cell.className = "empty";
		cell.textContent = "No LDAP server bindings";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const s of servers) {
		const row = document.createElement("tr");

		const containerCell = document.createElement("td");
		containerCell.textContent = s.container;
		row.appendChild(containerCell);

		const pathCell = document.createElement("td");
		pathCell.textContent = s.config_path;
		row.appendChild(pathCell);

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		rmButton.textContent = "Unregister";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeLdapServer(s.container));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshLdapServers() {
	const data = await apiRequest("GET", CIX_API.listLdapServers());
	cache.ldapServers = data.servers;
	renderLdapServers(cache.ldapServers);
}

async function removeLdapServer(container) {
	try {
		await apiRequest("DELETE", CIX_API.deleteLdapServer(container));
		clearStatus();
		await refreshLdapServers();
	} catch (e) {
		showStatus("Failed to unregister LDAP server " + container + ": " + e.message, true);
	}
}

/* ---------- LDAP Groups (task #726) ---------- */

/* Set while the LDAP group modal form is open in edit mode (task #750) --
 * null means the next submit is a create (POST). */
let ldapGroupEditName = null;

function editLdapGroup(group) {
	/* Set AFTER openModal() -- it calls closeModal(), which resets this
	 * to null (otherwise the submit handler POSTs instead of PUTs). */
	openModal("ldap-group-form", "Edit LDAP group");
	ldapGroupEditName = group.name;
	document.getElementById("lgf-name").value = group.name;
	document.getElementById("lgf-name").readOnly = true;
	document.getElementById("lgf-gidnumber").value = group.gidnumber;
	document.getElementById("lgf-submit").textContent = "Save";
}

function renderLdapGroups(groups) {
	const body = document.getElementById("ldap-groups-body");

	body.textContent = "";
	if (groups.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 3;
		cell.className = "empty";
		cell.textContent = "No LDAP groups";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const g of groups) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.textContent = g.name;
		row.appendChild(nameCell);

		const gidCell = document.createElement("td");
		gidCell.textContent = g.gidnumber;
		row.appendChild(gidCell);

		const actionCell = document.createElement("td");
		const editButton = document.createElement("button");

		editButton.textContent = "Edit";
		editButton.addEventListener("click", () => editLdapGroup(g));
		actionCell.appendChild(editButton);

		const rmButton = document.createElement("button");

		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeLdapGroup(g.name));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshLdapGroups() {
	const data = await apiRequest("GET", CIX_API.listLdapGroups());
	cache.ldapGroups = data.groups;
	renderLdapGroups(cache.ldapGroups);
}

async function removeLdapGroup(name) {
	try {
		await apiRequest("DELETE", CIX_API.deleteLdapGroup(name));
		clearStatus();
		await refreshLdapGroups();
	} catch (e) {
		showStatus("Failed to remove LDAP group " + name + ": " + e.message, true);
	}
}

/* ---------- LDAP Users (task #726) ---------- */

/* Set while the LDAP user modal form is open in edit mode (task #731,
 * following the exact ldapGroupEditName pattern from task #750) -- null
 * means the next submit is a create (POST). */
let ldapUserEditName = null;

function editLdapUser(user) {
	/* openModal() calls closeModal(), which resets ldapUserEditName to
	 * null -- so this MUST be set AFTER openModal(), or the submit
	 * handler sees null and POSTs (create) instead of PUT (update),
	 * 409-ing on the existing name. */
	openModal("ldap-user-form", "Edit LDAP user");
	ldapUserEditName = user.name;
	document.getElementById("luf-name").value = user.name;
	document.getElementById("luf-name").readOnly = true;
	document.getElementById("luf-uidnumber").value = user.uidnumber;
	document.getElementById("luf-primarygroup").value = user.primarygroup;
	document.getElementById("luf-secondary-groups").value = (user.secondary_groups || []).join(",");
	document.getElementById("luf-givenname").value = user.givenname || "";
	document.getElementById("luf-sn").value = user.sn || "";
	document.getElementById("luf-mail").value = user.mail || "";
	document.getElementById("luf-loginshell").value = user.loginshell || "";
	document.getElementById("luf-homedirectory").value = user.homedirectory || "";
	document.getElementById("luf-ssh-key").value = user.ssh_public_key || "";
	document.getElementById("luf-disabled").checked = !!user.disabled;
	document.getElementById("luf-submit").textContent = "Save";
}

function renderLdapUsers(users) {
	const body = document.getElementById("ldap-users-body");

	body.textContent = "";
	if (users.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 9;
		cell.className = "empty";
		cell.textContent = "No LDAP users";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const u of users) {
		const row = document.createElement("tr");

		const cells = [u.name, u.uidnumber, u.primarygroup,
		               (u.secondary_groups || []).join(", ") || "-", u.mail,
		               u.has_password ? "set" : "unset", u.disabled ? "yes" : "no",
		               u.ssh_public_key ? "set" : "unset"];
		for (const v of cells) {
			const cell = document.createElement("td");
			cell.textContent = v;
			row.appendChild(cell);
		}

		const actionCell = document.createElement("td");
		const editButton = document.createElement("button");

		editButton.textContent = "Edit";
		editButton.addEventListener("click", () => editLdapUser(u));
		actionCell.appendChild(editButton);

		const rmButton = document.createElement("button");

		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeLdapUser(u.name));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshLdapUsers() {
	const data = await apiRequest("GET", CIX_API.listLdapUsers());
	cache.ldapUsers = data.users;
	renderLdapUsers(cache.ldapUsers);
}

async function removeLdapUser(name) {
	try {
		await apiRequest("DELETE", CIX_API.deleteLdapUser(name));
		clearStatus();
		await refreshLdapUsers();
	} catch (e) {
		showStatus("Failed to remove LDAP user " + name + ": " + e.message, true);
	}
}



/* ---------- Build logs (issue #57) ---------- */

async function refreshBuildLogs() {
	const body = document.getElementById("build-logs-body");

	try {
		const data = await apiRequest("GET", CIX_API.listBuildLogs());
		const logs = data.logs || [];

		/* #432: a log list changes when a build writes one, which is
		 * rare next to the refresh tick -- so in practice this table
		 * stops being rebuilt at all, and the log you just opened
		 * stops taking your scroll position with it. */
		if (unchangedAndRendered(body, "build-logs", logs))
			return;
		body.textContent = "";
		if (logs.length === 0) {
			const tr = document.createElement("tr");
			const td = document.createElement("td");

			td.colSpan = 4;
			td.className = "empty";
			td.textContent = "No builds recorded yet";
			tr.appendChild(td);
			body.appendChild(tr);
			return;
		}
		for (const l of logs) {
			const tr = document.createElement("tr");
			const view = document.createElement("button");

			for (const text of [l.file, formatBytes(l.size_bytes),
			                    new Date(l.modified_at * 1000).toLocaleString()]) {
				const c = document.createElement("td");

				c.textContent = text;
				tr.appendChild(c);
			}
			view.textContent = "View";
			view.className = "button-small";
			view.addEventListener("click", () => showBuildLog(l.file));
			const td = document.createElement("td");

			td.appendChild(view);
			tr.appendChild(td);
			body.appendChild(tr);
		}
	} catch (e) {
		/* Best-effort, same as every other panel here. */
	}
}

/* #433: the one place that closes the viewer. Both the button and
 * Escape route here, so there is a single definition of "closed"
 * rather than two that can disagree. */
function closeBuildLog() {
	const panel = document.getElementById("build-log-panel");

	if (panel !== null)
		panel.hidden = true;
}

async function showBuildLog(file) {
	const view = document.getElementById("build-log-view");
	const panel = document.getElementById("build-log-panel");
	const title = document.getElementById("build-log-title");

	if (panel !== null)
		panel.hidden = false;
	if (title !== null)
		title.textContent = file;
	view.textContent = "Loading…";
	try {
		/* Fetched directly rather than through apiRequest(): the
		 * response is plain text, not JSON, and it can be megabytes --
		 * the log is the point, so it is not summarised or reshaped
		 * here. */
		const res = await fetch(CIX_API.getBuildLog(file), {
			headers: authToken ? { Authorization: "Bearer " + authToken } : {},
		});

		view.textContent = res.ok ? await res.text() : "Could not read " + file;
		/* The tail is what matters on a truncated log, so land there. */
		view.scrollTop = view.scrollHeight;
	} catch (e) {
		view.textContent = "Could not read " + file + ": " + e.message;
	}
}



/* ---------- Running configuration (ADR-0206, #150) ---------- */

/*
 * The same document `cixctl show running-config` renders, rendered the
 * dashboard's own way. Generic on purpose -- it walks whatever sections
 * the document contains rather than knowing them, so a subsystem added
 * to the ConfigDocument schema appears here with no change to this file.
 * The schema is the vocabulary; a renderer that had to be taught each
 * section would be a second copy of that list.
 */
function runningConfigLines(value, depth, out) {
	const pad = "  ".repeat(depth);

	if (value === null || value === undefined) {
		out.push(pad + "-");
		return;
	}
	if (Array.isArray(value)) {
		if (value.length === 0) {
			out.push(pad + "(none)");
			return;
		}
		value.forEach((item, i) => {
			if (item !== null && typeof item === "object") {
				if (i > 0)
					out.push("");
				runningConfigLines(item, depth, out);
			} else {
				out.push(pad + String(item));
			}
		});
		return;
	}
	if (typeof value === "object") {
		for (const key of Object.keys(value)) {
			const v = value[key];

			if (v !== null && typeof v === "object") {
				out.push(pad + key);
				runningConfigLines(v, depth + 1, out);
			} else {
				out.push(pad + key + " " + (v === null ? "-" : String(v)));
			}
		}
		return;
	}
	out.push(pad + String(value));
}

function formatRunningConfig(doc) {
	const out = [];

	for (const section of Object.keys(doc)) {
		out.push("!");
		out.push(section);
		runningConfigLines(doc[section], 1, out);
	}
	out.push("!");
	return out.join("\n");
}

async function refreshRunningConfig() {
	const pre = document.getElementById("running-config-text");

	if (pre.textContent === "")
		pre.textContent = "Loading…";
	try {
		const doc = await apiRequest("GET", CIX_API.getConfig());

		if (dataChanged("running-config", doc))
			pre.textContent = formatRunningConfig(doc);
	} catch (e) {
		pre.textContent = "Could not load the running configuration: " + e.message;
	}
}

document.getElementById("running-config-refresh").addEventListener("click", refreshRunningConfig);

/* Copy is the point of having this on a screen at all: the document
 * exists to be pasted into a ticket or a review, which is also exactly
 * why it is redacted before it ever reaches here. */
document.getElementById("running-config-copy").addEventListener("click", async () => {
	const pre = document.getElementById("running-config-text");
	const button = document.getElementById("running-config-copy");

	try {
		await navigator.clipboard.writeText(pre.textContent);
		button.textContent = "Copied";
	} catch (e) {
		/* Clipboard access is denied outside a secure context and in
		 * some browsers -- say so rather than appearing to succeed. */
		button.textContent = "Copy blocked";
	}
	setTimeout(() => { button.textContent = "Copy"; }, 1500);
});

/* ---------- Control-plane stalls (issue #100) ---------- */

/*
 * Everything this host does on a clock (ADR-0257).
 *
 * The action is picked from GET /schedule-actions, never typed: a
 * free-text command field on a shell-less host is a shell-exec
 * endpoint, and the closed registry is the security boundary rather
 * than a convenience.
 *
 * `describes` is rendered by the daemon and only ever read -- nothing
 * parses it back. The schedule itself is a structured body, so there
 * is no syntax for anyone to mistype; a wrong field is a missing
 * field, not a valid expression meaning something else.
 */
let scheduleActions = [];

async function refreshScheduleActions() {
	try {
		const data = await apiRequest("GET", CIX_API.listScheduleActions());

		scheduleActions = data.actions || [];
	} catch (e) {
		/* The list only feeds the create form's own select. */
	}
}

function scheduleWhenText(s) {
	/* The daemon's own rendering when it gave one -- one source of
	 * truth for "daily at 02:00", rather than a second formatter here
	 * that could disagree with the CLI's. */
	if (s.describes)
		return s.describes;
	return "—";
}

async function refreshSchedules() {
	const body = document.getElementById("schedules-body");

	if (body === null)
		return;
	try {
		const data = await apiRequest("GET", CIX_API.listSchedules());
		const rows = data.schedules || [];

		body.textContent = "";
		if (rows.length === 0) {
			const tr = document.createElement("tr");
			const td = document.createElement("td");

			td.colSpan = 7;
			td.className = "empty";
			td.textContent = "Nothing is scheduled.";
			tr.appendChild(td);
			body.appendChild(tr);
			return;
		}
		for (const s of rows) {
			const tr = document.createElement("tr");

			for (const text of [s.name, s.action, scheduleWhenText(s)]) {
				const td = document.createElement("td");

				td.textContent = text;
				tr.appendChild(td);
			}

			const en = document.createElement("td");

			en.textContent = s.enabled === false ? "no" : "yes";
			tr.appendChild(en);

			const last = document.createElement("td");

			if (s.last_run_at) {
				/* "has not run" and "ran and failed" are different
				 * facts, which is why last_ok is nullable -- say which
				 * one this is rather than collapsing them. */
				last.textContent = new Date(s.last_run_at * 1000).toLocaleString() +
				                   (s.last_ok === false ? " — failed" : "");
				if (s.last_ok === false) {
					last.className = "cell-error";
					last.title = s.last_reason || "";
				}
			} else {
				last.textContent = "never";
			}
			tr.appendChild(last);

			const next = document.createElement("td");

			/* Null when disabled: a disabled job has no next run, and
			 * inventing one would be a time nothing will happen at. */
			next.textContent = s.next_run_at
			    ? new Date(s.next_run_at * 1000).toLocaleString() : "—";
			tr.appendChild(next);

			const actions = document.createElement("td");
			const run = document.createElement("button");

			run.type = "button";
			run.textContent = "Run now";
			run.addEventListener("click", async () => {
				try {
					await apiRequest("POST", CIX_API.runSchedule(s.name), {});
					showStatus("Ran " + s.name + ".", false);
				} catch (e) {
					showStatus("Could not run " + s.name + ": " + e.message, true);
				}
				refreshSchedules();
			});
			actions.appendChild(run);

			const toggle = document.createElement("button");

			toggle.type = "button";
			toggle.textContent = s.enabled === false ? "Enable" : "Disable";
			toggle.addEventListener("click", async () => {
				try {
					await apiRequest("PUT", CIX_API.setSchedule(s.name), {
						action: s.action,
						params: s.params || {},
						schedule: s.schedule,
						window_minutes: s.window_minutes || 0,
						catch_up: s.catch_up === true,
						enabled: s.enabled === false,
					});
				} catch (e) {
					showStatus("Could not update " + s.name + ": " + e.message, true);
				}
				refreshSchedules();
			});
			actions.appendChild(toggle);

			const del = document.createElement("button");

			del.type = "button";
			del.className = "button-danger";
			del.textContent = "Delete";
			del.addEventListener("click", async () => {
				if (!confirm("Delete schedule \"" + s.name + "\"?"))
					return;
				try {
					await apiRequest("DELETE", CIX_API.deleteSchedule(s.name));
				} catch (e) {
					showStatus("Could not delete " + s.name + ": " + e.message, true);
				}
				refreshSchedules();
			});
			actions.appendChild(del);
			tr.appendChild(actions);
			body.appendChild(tr);
		}
	} catch (e) {
		body.textContent = "";
		const tr = document.createElement("tr");
		const td = document.createElement("td");

		td.colSpan = 7;
		td.className = "empty";
		td.textContent = "Could not load schedules: " + e.message;
		tr.appendChild(td);
		body.appendChild(tr);
	}
}

/*
 * Installer media, built server-side (ADR-0064).
 *
 * Only one build is ever in flight -- a second POST while state is
 * "building" is refused with 409 rather than starting another, so the
 * button reflects that rather than pretending otherwise.
 */
async function refreshIso() {
	const box = document.getElementById("iso-status");

	if (box === null)
		return;
	try {
		const s = await apiRequest("GET", CIX_API.getSystemIso());

		if (!dataChanged("iso", s))
			return;
		box.textContent = "";
		box.appendChild(fieldBlock("State", s.state));
		if (s.built_version)
			box.appendChild(fieldBlock("Built from", s.built_version));
		if (s.iso_path)
			box.appendChild(fieldBlock("ISO", s.iso_path));
		/* Size on the page, because an installer that quietly triples
		 * is invisible otherwise -- one did. */
		if (s.iso_bytes)
			box.appendChild(fieldBlock("Size", formatBytes(s.iso_bytes)));
		/* An unsigned ISO is still a usable ISO, so a missing release
		 * key does not fail the build -- it just leaves this empty,
		 * and saying so is the point. */
		box.appendChild(fieldBlock("Signature",
		    s.signature_path || (s.state === "ready" ? "unsigned — no release key at build time"
		                                            : "—")));
		if (s.error)
			box.appendChild(fieldBlock("Error", s.error));
		box.appendChild(fieldBlock("Publish", s.publish_state +
		    (s.published_name ? " — " + s.published_name : "") +
		    (s.publish_error ? " — " + s.publish_error : "")));

		document.getElementById("iso-build").disabled = s.state === "building";
		document.getElementById("iso-publish").disabled = s.state !== "ready";
	} catch (e) {
		/* Best-effort, same as every other panel here. */
	}
}

document.getElementById("iso-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const body = {};

	/* Every field is optional and independent -- an omitted one is
	 * simply not passed, and the installer asks on the console. Sending
	 * "" would be claiming an answer nobody gave. */
	for (const [field, id] of [["disk", "isof-disk"], ["ip", "isof-ip"],
	                           ["prefix", "isof-prefix"], ["gateway", "isof-gateway"],
	                           ["interface", "isof-interface"]]) {
		const v = document.getElementById(id).value.trim();

		if (v !== "")
			body[field] = v;
	}
	try {
		await apiRequest("POST", CIX_API.postSystemIso(), body);
		showStatus("Building installer media\u2026", false);
	} catch (e) {
		showStatus("Could not start the ISO build: " + e.message, true);
	}
	refreshIso();
});

document.getElementById("iso-publish").addEventListener("click", async () => {
	try {
		await apiRequest("POST", CIX_API.publishSystemIso(), {});
		showStatus("Publishing the installer to the artifact cache\u2026", false);
	} catch (e) {
		showStatus("Could not publish the ISO: " + e.message, true);
	}
	refreshIso();
});

async function refreshStalls() {
	const body = document.getElementById("stalls-body");

	try {
		const data = await apiRequest("GET", CIX_API.getStalls());
		const stalls = data.stalls || [];

		body.textContent = "";
		if (stalls.length === 0) {
			const tr = document.createElement("tr");
			const td = document.createElement("td");

			td.colSpan = 6;
			td.className = "empty";
			/* Nothing recorded is the good case, and worth saying so
			 * rather than leaving a blank table that reads as broken. */
			td.textContent = "None recorded — the loop has not gone quiet";
			tr.appendChild(td);
			body.appendChild(tr);
			return;
		}
		for (const s of stalls) {
			const tr = document.createElement("tr");
			const cells = [
				new Date(s.ts * 1000).toLocaleString(),
				s.event,
				s.seconds + "s",
				s.state,
				s.wchan || "(running)",
				s.activity || "(idle)",
			];

			for (const text of cells) {
				const td = document.createElement("td");

				td.textContent = text;
				tr.appendChild(td);
			}
			body.appendChild(tr);
		}
	} catch (e) {
		/* Best-effort, same as every other panel here. */
	}
}

/* ---------- Kernel Line (issue #65) ---------- */

let kernelPolicyDirty = false;

function renderKernelPolicy(data) {
	const grid = document.getElementById("kpf-status");

	if (!kernelPolicyDirty)
		document.getElementById("kpf-channel").value = data.channel || "pinned";

	grid.textContent = "";
	grid.appendChild(fieldBlock("Running", data.running_version +
	                             (data.running_series ? " (line " + data.running_series + ")" : "")));
	/*
	 * Three states, not two: a channel that has never been resolved is
	 * not the same as one you are up to date on, and rendering the
	 * unresolved case as "up to date" would be the one wrong answer
	 * that reads as reassuring.
	 */
	if (data.channel === "pinned")
		grid.appendChild(fieldBlock("Channel is at", "nothing — pinned proposes no version"));
	else if (data.resolved_version === null)
		grid.appendChild(fieldBlock("Channel is at", "not resolved yet — refresh from kernel.org"));
	else
		grid.appendChild(fieldBlock("Channel is at", data.resolved_version +
		                             (data.behind ? " — you are behind it" : " — you are on it")));
	if (data.channel === "longterm" && data.running_series_maintained === false &&
	    data.newest_longterm !== null) {
		grid.appendChild(fieldBlock("Note", data.running_series +
		                             " is not a longterm line kernel.org still lists; the newest is " +
		                             data.newest_longterm));
	}
	grid.appendChild(fieldBlock("Release data",
	                             data.releases_fetched_at === null
	                                 ? "never fetched"
	                                 : "fetched " + new Date(data.releases_fetched_at * 1000).toLocaleString()));
	if (data.refreshing)
		grid.appendChild(fieldBlock("Refresh", "running now"));
	if (data.last_refresh_error !== null)
		grid.appendChild(fieldBlock("Last refresh error", data.last_refresh_error));
}

async function refreshKernelPolicy() {
	try {
		renderKernelPolicy(await apiRequest("GET", CIX_API.getKernelPolicy()));
	} catch (e) {
		/* Best-effort, same as every other panel here. */
	}
}

document.getElementById("kpf-channel").addEventListener("change", () => {
	kernelPolicyDirty = true;
});

document.getElementById("kpf-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	try {
		const res = await apiRequest("PUT", CIX_API.setKernelPolicy(), {
			channel: document.getElementById("kpf-channel").value,
		});

		kernelPolicyDirty = false;
		showStatus("Kernel channel set to " + res.channel + ".", false);
		renderKernelPolicy(res);
	} catch (e) {
		showStatus("Failed to set kernel channel: " + e.message, true);
	}
});

document.getElementById("kpf-refresh").addEventListener("click", async () => {
	try {
		renderKernelPolicy(await apiRequest("POST", CIX_API.refreshKernelPolicy(), {}));
		showStatus("Asking kernel.org what each channel is at…", false);
		/* The fetch is a forked curl, not this request -- so the answer
		 * lands a moment after the 202 does. */
		window.setTimeout(refreshKernelPolicy, 3000);
	} catch (e) {
		showStatus("Could not start a refresh: " + e.message, true);
	}
});

/* ---------- Boot Console (issue #24) ---------- */

let bootConsoleDirty = false;

async function refreshBootConsole() {
	try {
		const data = await apiRequest("GET", CIX_API.getBootConsole());
		const cfg = data.config || {};
		const grid = document.getElementById("bcf-effect");
		const body = document.getElementById("boot-entries-body");

		if (!bootConsoleDirty) {
			document.getElementById("bcf-consoles").value = (cfg.consoles || []).join(" ");
			document.getElementById("bcf-extra").value = cfg.extra || "";
		}
		grid.textContent = "";
		grid.appendChild(fieldBlock("Rendered", cfg.rendered || "(none)"));

		body.textContent = "";
		const entries = data.loader_entries || [];

		if (entries.length === 0) {
			const tr = document.createElement("tr");
			const td = document.createElement("td");

			td.colSpan = 2;
			td.className = "empty";
			/* Not an error: a dev daemon has no ESP, and saying so beats
			 * an empty table that looks like something failed. */
			td.textContent = "No loader entries visible from here — this is not an installed host";
			tr.appendChild(td);
			body.appendChild(tr);
			return;
		}
		for (const e of entries) {
			const tr = document.createElement("tr");

			for (const text of [e.entry, e.options]) {
				const td = document.createElement("td");

				td.textContent = text;
				tr.appendChild(td);
			}
			body.appendChild(tr);
		}
	} catch (e) {
		/* Best-effort, same as every other panel here. */
	}
}

for (const id of ["bcf-consoles", "bcf-extra"]) {
	document.getElementById(id).addEventListener("input", () => {
		bootConsoleDirty = true;
	});
}

document.getElementById("bcf-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	const consoles = document.getElementById("bcf-consoles").value.trim();

	try {
		const res = await apiRequest("PUT", CIX_API.setBootConsole(), {
			consoles: consoles === "" ? [] : consoles.split(/\s+/),
			extra: document.getElementById("bcf-extra").value.trim(),
		});

		bootConsoleDirty = false;
		showStatus("Boot console saved — " + (res.loader_entries_updated || 0) +
		           " loader entry/entries rewritten; takes effect at the next boot.", false);
		await refreshBootConsole();
	} catch (e) {
		showStatus("Could not save the boot console: " + e.message, true);
	}
});

/* ---------- Control Plane Reservation (issue #86) ---------- */

let cprDirty = false;

async function refreshControlPlaneReservation() {
	try {
		const c = await apiRequest("GET", CIX_API.getControlPlaneReservation());
		const grid = document.getElementById("cpr-effect");

		if (!cprDirty) {
			document.getElementById("cpr-enabled").checked = !!c.enabled;
			document.getElementById("cpr-cpu-percent").value = c.cpu_percent;
			document.getElementById("cpr-memory-bytes").value = c.memory_bytes;
		}
		grid.textContent = "";
		grid.appendChild(fieldBlock("This host", (c.host_cpus || 0) + " CPUs, " + formatBytes(c.host_memory_bytes || 0)));
		grid.appendChild(fieldBlock("Workload cgroup", c.cgroup || "-"));
		/* The raw cgroup values, not a prettied version: this is the
		 * number the kernel is enforcing, and an operator checking
		 * whether a limit is really in place wants to see exactly what
		 * is in cpu.max. */
		grid.appendChild(fieldBlock("Workload cpu.max", c.workload_cpu_max || "unlimited"));
		grid.appendChild(fieldBlock("Workload memory.max",
		                            c.workload_memory_max ? formatBytes(c.workload_memory_max) : "unlimited"));
	} catch (e) {
		/* Best-effort, like every other config panel here. */
	}
}

for (const id of ["cpr-enabled", "cpr-cpu-percent", "cpr-memory-bytes"]) {
	document.getElementById(id).addEventListener("input", () => {
		cprDirty = true;
	});
}

document.getElementById("cpr-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	try {
		await apiRequest("PUT", CIX_API.setControlPlaneReservation(), {
			enabled: document.getElementById("cpr-enabled").checked,
			cpu_percent: parseInt(document.getElementById("cpr-cpu-percent").value, 10),
			memory_bytes: parseInt(document.getElementById("cpr-memory-bytes").value, 10),
		});
		cprDirty = false;
		showStatus("Control plane reservation saved — the workload ceiling is in effect now.", false);
		await refreshControlPlaneReservation();
	} catch (e) {
		showStatus("Could not save reservation: " + e.message, true);
	}
});

/* ---------- LDAP Config: start_uid/start_gid auto-allocation floor (task #748) ---------- */

let ldapConfigDirty = false;

async function refreshLdapConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getLdapConfig());

		cache.ldapConfig = config;
		{
			const grid = document.getElementById("ldap-client-delivery");

			grid.textContent = "";
			grid.appendChild(fieldBlock("Configured client_uri",
			                            config.client_uri || "(unset — derived from registered servers)"));
			grid.appendChild(fieldBlock("Effective (what clients get)",
			                            config.effective_client_uri || "(none)"));
		}
		if (!ldapConfigDirty) {
			document.getElementById("lcf-start-uid").value = config.start_uid;
			document.getElementById("lcf-start-gid").value = config.start_gid;
		}
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

document.getElementById("lcf-start-uid").addEventListener("input", () => {
	ldapConfigDirty = true;
});
document.getElementById("lcf-start-gid").addEventListener("input", () => {
	ldapConfigDirty = true;
});

document.getElementById("ldap-config-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const body = {
		start_uid: parseInt(document.getElementById("lcf-start-uid").value, 10),
		start_gid: parseInt(document.getElementById("lcf-start-gid").value, 10),
	};

	try {
		await apiRequest("PUT", CIX_API.updateLdapConfig(), body);
		clearStatus();
		showStatus("LDAP config saved", false);
		ldapConfigDirty = false;
		await refreshLdapConfig();
	} catch (e) {
		showStatus("Failed to save LDAP config: " + e.message, true);
	}
});

/* ---------- NTP (tasks #751-755) ---------- */

/* ---- ESP: loader config and boot entries (ADR-0202, issue #128) ---- */

let espDirty = false;

function renderEspEntries(data) {
	const body = document.getElementById("esp-entries-body");
	const entries = data.entries || [];

	body.textContent = "";
	if (entries.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 5;
		cell.className = "empty";
		cell.textContent = "No loader entries";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const e of entries) {
		const row = document.createElement("tr");

		for (const text of [e.name, e.title || "-",
		                     e.matches_default ? "yes" : "no",
		                     e.is_running_slot ? "yes" : ""]) {
			const cell = document.createElement("td");

			cell.textContent = text;
			row.appendChild(cell);
		}

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		rmButton.textContent = "Delete";
		rmButton.className = "button-danger";
		/* The daemon refuses to delete the running slot's own entry;
		 * disabling it here as well makes that a visible rule rather
		 * than a surprise 409. */
		rmButton.disabled = !!e.is_running_slot;
		if (e.is_running_slot)
			rmButton.title = "This is the entry the running system booted from";
		rmButton.addEventListener("click", () => removeEspEntry(e.name));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

/*
 * ADR-0212: the Secure Boot signing key pair `iso build` needs. The
 * private key goes in and never comes back -- GET reports only whether
 * it is set, plus the certificate's own public identity, so nothing on
 * this page can ever display key material.
 *
 * The paste box exists because a real Cix host has no shell, no scp
 * target and no console, so ADR-0064's "operator places it out of
 * band" had no band to be out of.
 */
async function refreshSigningKeys() {
	const box = document.getElementById("signing-keys-summary");

	if (box === null)
		return;
	try {
		const data = await apiRequest("GET", CIX_API.getSystemSigningKeys());

		box.textContent = "";
		if (!data.key_set && !data.cert_set) {
			const p = document.createElement("p");

			p.textContent =
			    "No key pair installed -- this host cannot build an ISO until one is.";
			box.appendChild(p);
			return;
		}
		const rows = [["Private key", data.key_set ? "installed" : "MISSING"],
		              ["Certificate", data.cert_set ? "installed" : "MISSING"]];

		if (data.subject)
			rows.push(["Subject", data.subject]);
		if (data.not_after)
			rows.push(["Expires", data.not_after]);
		if (data.fingerprint_sha256)
			rows.push(["Fingerprint", data.fingerprint_sha256]);
		for (const [label, value] of rows) {
			const p = document.createElement("p");

			p.textContent = label + ": " + value;
			box.appendChild(p);
		}
		if (data.key_set !== data.cert_set) {
			const p = document.createElement("p");

			p.textContent = "Only half the pair is present -- building an ISO needs both.";
			box.appendChild(p);
		}
	} catch (e) {
		box.textContent = "";
		const p = document.createElement("p");

		p.textContent = "Could not read signing-key state.";
		box.appendChild(p);
	}
}

async function installSigningKeys() {
	const key = document.getElementById("signing-key-pem").value.trim();
	const cert = document.getElementById("signing-cert-pem").value.trim();

	if (key === "" || cert === "") {
		clearStatus();
		showStatus("Paste both the private key and the certificate.", true);
		return;
	}
	try {
		await apiRequest("PUT", CIX_API.putSystemSigningKeys(), { key: key, cert: cert });
		clearStatus();
		showStatus("Signing key pair installed.", false);
		/*
		 * Cleared on success only. A rejected paste stays in the boxes
		 * so a correctable mistake -- the wrong half of a pair, most
		 * likely -- does not have to be pasted again from scratch.
		 */
		document.getElementById("signing-key-pem").value = "";
		document.getElementById("signing-cert-pem").value = "";
		await refreshSigningKeys();
	} catch (e) {
		clearStatus();
		showStatus(e.message, true);
	}
}

async function clearSigningKeys() {
	try {
		await apiRequest("DELETE", CIX_API.deleteSystemSigningKeys());
		clearStatus();
		showStatus("Signing key pair removed from this host.", false);
		await refreshSigningKeys();
	} catch (e) {
		clearStatus();
		showStatus(e.message, true);
	}
}

/*
 * The release key (ADR-0220). Same no-key-material-leaves rule as the
 * pair above, with one deliberate difference: the PUBLIC key is shown
 * in full and is meant to be copied out. Publishing it is what makes
 * every signature this host produces verifiable by someone who runs no
 * Cix software at all.
 */
async function refreshReleaseKey() {
	const box = document.getElementById("release-key-summary");

	if (box === null)
		return;
	try {
		const data = await apiRequest("GET", CIX_API.getSystemReleaseKey());

		box.textContent = "";
		if (!data.key_set) {
			const p = document.createElement("p");

			p.textContent = "No release key installed -- ISOs built here will be unsigned.";
			box.appendChild(p);
			return;
		}
		const head = document.createElement("p");

		head.textContent = "Release key: installed" +
		    (data.key_id ? " (key id " + data.key_id + ")" : "");
		box.appendChild(head);
		if (data.public_key) {
			const label = document.createElement("p");
			const pre = document.createElement("pre");

			label.textContent = "Public key -- publish this; verifiers need it:";
			box.appendChild(label);
			pre.textContent = data.public_key;
			box.appendChild(pre);
		}
	} catch (e) {
		box.textContent = "";
		const p = document.createElement("p");

		p.textContent = "Could not read release-key state.";
		box.appendChild(p);
	}
}

async function installReleaseKey() {
	const key = document.getElementById("release-key-pem").value.trim();

	if (key === "") {
		clearStatus();
		showStatus("Paste an Ed25519 private key.", true);
		return;
	}
	try {
		await apiRequest("PUT", CIX_API.putSystemReleaseKey(), { key: key });
		clearStatus();
		showStatus("Release key installed.", false);
		/* Cleared on success only -- same reasoning as the pair above. */
		document.getElementById("release-key-pem").value = "";
		await refreshReleaseKey();
	} catch (e) {
		clearStatus();
		showStatus(e.message, true);
	}
}

async function clearReleaseKey() {
	try {
		await apiRequest("DELETE", CIX_API.deleteSystemReleaseKey());
		clearStatus();
		showStatus("Release key removed from this host.", false);
		await refreshReleaseKey();
	} catch (e) {
		clearStatus();
		showStatus(e.message, true);
	}
}

async function refreshEsp() {
	try {
		const data = await apiRequest("GET", CIX_API.getEsp());
		const box = document.getElementById("esp-summary");

		cache.esp = data;
		box.textContent = "";
		if (!data.present) {
			const p = document.createElement("p");

			p.textContent = "No ESP reachable from this daemon.";
			box.appendChild(p);
			return;
		}
		for (const [label, value] of [
			    ["Will boot", data.selected_entry || "(nothing matches the default)"],
			    ["Running slot", data.running_slot || "(not an installed system)"],
			    ["Writable", data.writable ? "yes" : "no"]]) {
			const p = document.createElement("p");

			p.textContent = label + ": " + value;
			box.appendChild(p);
		}
		if (!espDirty) {
			document.getElementById("esp-default").value = data.default || "";
			document.getElementById("esp-timeout").value =
			    data.timeout === null || data.timeout === undefined ? "" : data.timeout;
		}
		renderEspEntries(data);
		await refreshBootNext();
	} catch (e) {
		/* Best-effort, same posture as the other config panels. */
	}
}

async function setBootNext(slot) {
	try {
		const r = await apiRequest("POST", CIX_API.setBootNext(), { slot: slot });

		clearStatus();
		showStatus("Next boot: " + r.entry + " (once, then normal selection)", false);
		await refreshEsp();
	} catch (e) {
		/* 409 covers both "no entry for that slot" and "no writable EFI
		 * variables here" -- both are real states an operator needs to
		 * see verbatim, not flattened into a generic failure. */
		showStatus("Could not arm slot " + slot + ": " + e.message, true);
	}
}

async function clearBootNext() {
	try {
		await apiRequest("DELETE", CIX_API.clearBootNext());
		clearStatus();
		showStatus("Disarmed -- normal selection applies", false);
		await refreshEsp();
	} catch (e) {
		showStatus("Could not disarm: " + e.message, true);
	}
}

async function refreshBootNext() {
	const el = document.getElementById("esp-boot-next-state");

	try {
		const r = await apiRequest("GET", CIX_API.getBootNext());

		el.textContent = r.entry ? "Armed: " + r.entry + " (next boot only)"
		                          : "Nothing armed \u2014 normal selection applies";
	} catch (e) {
		el.textContent = "";
	}
}

async function removeEspEntry(name) {
	try {
		await apiRequest("DELETE", CIX_API.deleteEspEntry(name));
		clearStatus();
		showStatus("Removed loader entry " + name, false);
		await refreshEsp();
	} catch (e) {
		showStatus("Failed to remove " + name + ": " + e.message, true);
	}
}

document.getElementById("esp-boot-next-a").addEventListener("click", () => setBootNext("a"));
document.getElementById("esp-boot-next-b").addEventListener("click", () => setBootNext("b"));
document.getElementById("esp-boot-next-clear").addEventListener("click", clearBootNext);

for (const id of ["esp-default", "esp-timeout"])
	document.getElementById(id).addEventListener("input", () => {
		espDirty = true;
	});

document.getElementById("esp-loader-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const pattern = document.getElementById("esp-default").value.trim();
	const timeoutRaw = document.getElementById("esp-timeout").value.trim();
	const payload = {};

	if (pattern !== "")
		payload.default = pattern;
	if (timeoutRaw !== "")
		payload.timeout = Number(timeoutRaw);

	try {
		await apiRequest("PUT", CIX_API.putEsp(), payload);
		clearStatus();
		showStatus("Loader configuration saved", false);
		espDirty = false;
		await refreshEsp();
	} catch (e) {
		/* A pattern matching no entry comes back 409 -- that guard is
		 * the whole point of the endpoint, so surface it as-is rather
		 * than a generic failure. */
		showStatus("Failed to save loader config: " + e.message, true);
	}
});

/* ---- DNS provisioning: one call, per-replica results (ADR-0205) ---- */

document.getElementById("dns-provision-btn").addEventListener("click", async () => {
	const btn = document.getElementById("dns-provision-btn");

	btn.disabled = true;
	try {
		/* Empty body: the daemon owns the default topology, so the
		 * dashboard does not restate it and the two cannot disagree. */
		const result = await apiRequest("POST", CIX_API.provisionDns(), {});
		const lines = (result.replicas || []).map((r) => {
			const bits = [r.name + " " + r.created];

			if (r.registered)
				bits.push("registered");
			if (r.ip)
				bits.push(r.ip);
			if (r.error)
				bits.push(r.error);
			return bits.join(", ");
		});

		if (result.resolver)
			lines.push("host resolver -> " + result.resolver.join(", "));
		clearStatus();
		/* problems > 0 comes back as 207, which apiRequest treats as
		 * success -- it IS a real result, just a partial one, so it is
		 * reported as a warning with the detail rather than an error. */
		showStatus(lines.join(" | "), result.problems > 0);
		await refreshDnsServers();
		await refreshDnsForwarders();
	} catch (e) {
		showStatus("Provisioning failed: " + e.message, true);
	} finally {
		btn.disabled = false;
	}
});

/* ---- DNS Forwarders: where the DNS servers recurse to (#134, ADR-0203) ---- */

let dnsForwardersDirty = false;

async function refreshDnsForwarders() {
	try {
		const config = await apiRequest("GET", CIX_API.getDnsForwarders());

		cache.dnsForwarders = config.forwarders;
		if (!dnsForwardersDirty)
			document.getElementById("dnsf-servers").value = config.forwarders.join(", ");
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

document.getElementById("dnsf-servers").addEventListener("input", () => {
	dnsForwardersDirty = true;
});

document.getElementById("dns-forwarders-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const raw = document.getElementById("dnsf-servers").value.trim();
	const forwarders = raw === "" ? [] : raw.split(/[\s,]+/).filter((s) => s.length > 0);

	try {
		await apiRequest("PUT", CIX_API.setDnsForwarders(), { forwarders: forwarders });
		clearStatus();
		/* Empty is a legitimate setting, not a no-op, so say which
		 * happened rather than a generic "saved". */
		showStatus(forwarders.length === 0
		                   ? "Forwarders cleared -- DNS is authoritative-only"
		                   : "Forwarders saved and applied to every registered server",
		           false);
		dnsForwardersDirty = false;
		await refreshDnsForwarders();
	} catch (e) {
		showStatus("Failed to save forwarders: " + e.message, true);
	}
});

/* ---- NTP Config: upstream server address list ---- */

let ntpConfigDirty = false;

async function refreshNtpConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getSystemNtp());

		cache.ntpConfig = config;
		if (!ntpConfigDirty)
			document.getElementById("ncf-servers").value = config.upstream.join(", ");
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

document.getElementById("ncf-servers").addEventListener("input", () => {
	ntpConfigDirty = true;
});

document.getElementById("ntp-config-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const raw = document.getElementById("ncf-servers").value.trim();
	const servers = raw === "" ? [] : raw.split(/[\s,]+/).filter((s) => s.length > 0);

	try {
		await apiRequest("PUT", CIX_API.putSystemNtp(), { upstream: servers });
		clearStatus();
		showStatus("NTP config saved", false);
		ntpConfigDirty = false;
		await refreshNtpConfig();
	} catch (e) {
		showStatus("Failed to save NTP config: " + e.message, true);
	}
});

/* ---- NTP Servers: registered container time sources ---- */

function renderNtpServers(servers) {
	const body = document.getElementById("ntp-servers-body");

	body.textContent = "";
	if (servers.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 2;
		cell.className = "empty";
		cell.textContent = "No NTP server bindings";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const s of servers) {
		const row = document.createElement("tr");

		const containerCell = document.createElement("td");
		containerCell.textContent = s.container;
		row.appendChild(containerCell);

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		rmButton.textContent = "Unregister";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeNtpServer(s.container));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshNtpServers() {
	const data = await apiRequest("GET", CIX_API.listNtpServers());
	cache.ntpServers = data.servers;
	renderNtpServers(cache.ntpServers);
}

async function removeNtpServer(container) {
	try {
		await apiRequest("DELETE", CIX_API.deleteNtpServer(container));
		clearStatus();
		await refreshNtpServers();
	} catch (e) {
		showStatus("Failed to unregister NTP server " + container + ": " + e.message, true);
	}
}

document.getElementById("ntp-server-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const container = document.getElementById("nsf-container").value.trim();

	try {
		await apiRequest("POST", CIX_API.createNtpServer(), { container: container });
		clearStatus();
		document.getElementById("ntp-server-form").reset();
		closeModal();
		await refreshNtpServers();
	} catch (e) {
		showStatus("Failed to register NTP server: " + e.message, true);
	}
});

/* ---- Syslog forward targets: registered container log receivers
 * (ADR-0127) -- same shape as NTP Servers above, mirrored exactly. */

function renderSyslogTargets(targets) {
	const body = document.getElementById("syslog-targets-body");

	body.textContent = "";
	if (targets.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 2;
		cell.className = "empty";
		cell.textContent = "No syslog forward targets registered";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const t of targets) {
		const row = document.createElement("tr");

		const containerCell = document.createElement("td");
		containerCell.textContent = t.container;
		row.appendChild(containerCell);

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		rmButton.textContent = "Unregister";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeSyslogTarget(t.container));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshSyslogTargets() {
	const data = await apiRequest("GET", CIX_API.listSyslogTargets());
	cache.syslogTargets = data.targets;
	renderSyslogTargets(cache.syslogTargets);
}

function renderSyslogTargetsList() {
	refreshSyslogTargets();
}

async function removeSyslogTarget(container) {
	try {
		await apiRequest("DELETE", CIX_API.deleteSyslogTarget(container));
		clearStatus();
		await refreshSyslogTargets();
	} catch (e) {
		showStatus("Failed to unregister syslog target " + container + ": " + e.message, true);
	}
}

document.getElementById("syslog-target-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const container = document.getElementById("stf-container").value.trim();

	try {
		await apiRequest("POST", CIX_API.createSyslogTarget(), { container: container });
		clearStatus();
		document.getElementById("syslog-target-form").reset();
		closeModal();
		await refreshSyslogTargets();
	} catch (e) {
		showStatus("Failed to register syslog target: " + e.message, true);
	}
});

/* ---- TLS Throttle: per-source-IP HTTPS-handshake-failure throttling
 * (ADR-0134) ---- */

let tlsThrottleConfigDirty = false;

async function refreshTlsThrottleConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getSystemTlsThrottle());

		cache.tlsThrottleConfig = config;
		if (!tlsThrottleConfigDirty) {
			document.getElementById("ttf-enabled").checked = config.enabled;
			document.getElementById("ttf-threshold").value = config.threshold;
			document.getElementById("ttf-window").value = config.window_seconds;
			document.getElementById("ttf-block").value = config.block_seconds;
			document.getElementById("ttf-log-interval").value = config.log_interval_seconds;
		}
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

for (const id of ["ttf-enabled", "ttf-threshold", "ttf-window", "ttf-block", "ttf-log-interval"]) {
	document.getElementById(id).addEventListener("input", () => {
		tlsThrottleConfigDirty = true;
	});
}

document.getElementById("ttf-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	try {
		await apiRequest("PUT", CIX_API.putSystemTlsThrottle(), {
			enabled: document.getElementById("ttf-enabled").checked,
			threshold: parseInt(document.getElementById("ttf-threshold").value, 10),
			window_seconds: parseInt(document.getElementById("ttf-window").value, 10),
			block_seconds: parseInt(document.getElementById("ttf-block").value, 10),
			log_interval_seconds: parseInt(document.getElementById("ttf-log-interval").value, 10),
		});
		clearStatus();
		showStatus("TLS throttle config saved", false);
		tlsThrottleConfigDirty = false;
		await refreshTlsThrottleConfig();
	} catch (e) {
		showStatus("Failed to save TLS throttle config: " + e.message, true);
	}
});

function renderTlsThrottleStatus(entries) {
	const body = document.getElementById("tls-throttle-status-body");

	body.textContent = "";
	if (entries.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 4;
		cell.className = "empty";
		cell.textContent = "No sources currently tracked";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const e of entries) {
		const row = document.createElement("tr");

		const ipCell = document.createElement("td");
		ipCell.textContent = e.ip;
		row.appendChild(ipCell);

		const failCell = document.createElement("td");
		failCell.textContent = e.fail_count;
		row.appendChild(failCell);

		const blockedCell = document.createElement("td");
		blockedCell.textContent = e.blocked ? "yes" : "no";
		row.appendChild(blockedCell);

		const untilCell = document.createElement("td");
		untilCell.textContent = e.blocked && e.blocked_until
			? new Date(e.blocked_until * 1000).toLocaleTimeString()
			: "";
		row.appendChild(untilCell);

		body.appendChild(row);
	}
}

async function refreshTlsThrottleStatus() {
	const data = await apiRequest("GET", CIX_API.getSystemTlsThrottleStatus());

	cache.tlsThrottleStatus = data.entries;
	renderTlsThrottleStatus(cache.tlsThrottleStatus);
}

/* ---- NTP Status: most recent sync attempt outcome ---- */

async function refreshNtpStatus() {
	const box = document.getElementById("ntp-status-box");

	try {
		const status = await apiRequest("GET", CIX_API.getSystemNtpStatus());

		cache.ntpStatus = status;
		box.textContent = "";

		const lines = [
			["State", status.state],
			["Synced from", status.synced_from || "(never)"],
			["Last sync unixtime", status.last_sync_unixtime || "(never)"],
		];
		for (const [label, value] of lines) {
			const p = document.createElement("p");
			p.textContent = label + ": " + value;
			box.appendChild(p);
		}
	} catch (e) {
		box.textContent = "Failed to load NTP status: " + e.message;
	}
}

document.getElementById("ntp-sync-now").addEventListener("click", async () => {
	try {
		await apiRequest("POST", CIX_API.postSystemNtpSync());
		clearStatus();
		showStatus("NTP sync started", false);
		await refreshNtpStatus();
	} catch (e) {
		showStatus("Failed to start NTP sync: " + e.message, true);
	}
});

/* ---- NTP Time: manual host clock view/override ---- */

async function refreshNtpTime() {
	const box = document.getElementById("ntp-time-box");

	try {
		const t = await apiRequest("GET", CIX_API.getSystemTime());

		cache.ntpTime = t;
		box.textContent = "";

		const p1 = document.createElement("p");
		p1.textContent = "Unix time: " + t.unixtime;
		box.appendChild(p1);

		const p2 = document.createElement("p");
		p2.textContent = "UTC: " + new Date(t.unixtime * 1000).toISOString();
		box.appendChild(p2);
	} catch (e) {
		box.textContent = "Failed to load host time: " + e.message;
	}
}

document.getElementById("ntp-time-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const raw = document.getElementById("ntf-unixtime").value.trim();
	const unixtime = raw === "" ? Math.floor(Date.now() / 1000) : parseInt(raw, 10);

	try {
		await apiRequest("PUT", CIX_API.putSystemTime(), { unixtime: unixtime });
		clearStatus();
		showStatus("Host clock set", false);
		document.getElementById("ntp-time-form").reset();
		await refreshNtpTime();
	} catch (e) {
		showStatus("Failed to set host clock: " + e.message, true);
	}
});

/* ---------- PKI ---------- */

/* Triggers a real browser save-as for pemText, named filename -- a
 * pure client-side convenience over data this dashboard already has
 * from GET /pki/ca|intermediate (cert_pem), not a new API capability
 * (the API-First Mandate is about capabilities existing at the API
 * layer first, and "get the CA cert" already does -- turning already-
 * fetched JSON into a downloadable file is presentation only).
 * Named/typed as .crt (application/x-x509-ca-cert), not .pem -- same
 * content (PEM-encoded text), but .crt is what triggers a real
 * double-click "install certificate" flow on more platforms
 * (Windows in particular) without a manual rename first. */
function downloadPem(filename, pemText) {
	const blob = new Blob([pemText], { type: "application/x-x509-ca-cert" });
	const url = URL.createObjectURL(blob);
	const a = document.createElement("a");

	a.href = url;
	a.download = filename;
	document.body.appendChild(a);
	a.click();
	a.remove();
	URL.revokeObjectURL(url);
}

function buildCaDownloadButton(filename, pemText) {
	const button = document.createElement("button");

	button.type = "button";
	button.textContent = "Download certificate (.crt)";
	button.addEventListener("click", () => downloadPem(filename, pemText));
	return button;
}

async function refreshPkiCa() {
	const pkiCaStatus = document.getElementById("pki-ca-status");
	const pkiCaForm = document.getElementById("pki-ca-form");

	try {
		const ca = await apiRequest("GET", CIX_API.getPkiCa());

		cache.pkiCa = ca;
		pkiCaStatus.className = "pki-ca-status bootstrapped";
		pkiCaStatus.textContent =
			"Bootstrapped: " + ca.subject + " (serial " + ca.serial + ", expires " + ca.not_after + ") ";
		pkiCaStatus.appendChild(buildCaDownloadButton("cix-root-ca.crt", ca.cert_pem));
		pkiCaForm.hidden = true;
	} catch (e) {
		cache.pkiCa = null;
		pkiCaStatus.className = "pki-ca-status";
		pkiCaStatus.textContent = "Not yet bootstrapped.";
		pkiCaForm.hidden = false;
	}
}

async function refreshPkiIntermediate() {
	const status = document.getElementById("pki-intermediate-status");
	const form = document.getElementById("pki-intermediate-form");

	try {
		const intermediate = await apiRequest("GET", CIX_API.getPkiIntermediate());

		cache.pkiIntermediate = intermediate;
		status.className = "pki-ca-status bootstrapped";
		status.textContent =
			"Bootstrapped: " +
			intermediate.subject +
			" (serial " +
			intermediate.serial +
			", expires " +
			intermediate.not_after +
			") ";
		status.appendChild(buildCaDownloadButton("cix-intermediate-ca.crt", intermediate.cert_pem));
		form.hidden = true;
	} catch (e) {
		cache.pkiIntermediate = null;
		status.className = "pki-ca-status";
		status.textContent = cache.pkiCa
			? "Not yet bootstrapped."
			: "Not yet bootstrapped (bootstrap the root CA first).";
		form.hidden = false;
	}
}

function renderPkiCerts(certs) {
	const body = document.getElementById("pki-certs-body");

	body.textContent = "";
	if (certs.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 6;
		cell.className = "empty";
		cell.textContent = "No certificates issued";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const cert of certs) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.textContent = cert.name;
		row.appendChild(nameCell);

		const serialCell = document.createElement("td");
		serialCell.textContent = cert.serial;
		row.appendChild(serialCell);

		const notAfterCell = document.createElement("td");
		notAfterCell.textContent = cert.not_after;
		row.appendChild(notAfterCell);

		const sansCell = document.createElement("td");
		sansCell.textContent = (cert.sans || []).join(", ");
		row.appendChild(sansCell);

		const ownerCell = document.createElement("td");
		ownerCell.textContent = formatOwner(cert.owner);
		row.appendChild(ownerCell);

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removePkiCert(cert.name));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshPkiCerts() {
	const data = await apiRequest("GET", CIX_API.listPkiCerts());
	cache.pkiCerts = data.certs;
	renderPkiCerts(cache.pkiCerts);
}

async function removePkiCert(name) {
	try {
		await apiRequest("DELETE", CIX_API.deletePkiCert(name));
		clearStatus();
		await refreshPkiCerts();
	} catch (e) {
		showStatus("Failed to remove certificate " + name + ": " + e.message, true);
	}
}

/* ---------- Packages ---------- */

/* One row per real installation (name+image pair) -- what's actually
 * on disk right now, not the recipe catalog (see renderRecipesList()
 * for that). Both link a name into the same package detail page
 * (Recipe + Installed tabs together already), the one place that
 * shows a given name's full picture -- no separate detail page per
 * list, matching this project's own single-source-of-truth posture. */
function renderPackagesList() {
	const body = document.getElementById("packages-body");

	body.textContent = "";
	if (cache.pkgList.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 5;
		cell.className = "empty";
		cell.textContent = "Nothing installed yet";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const pkg of cache.pkgList) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.appendChild(treeLink("#packages/" + encodeURIComponent(pkg.name), pkg.name, ""));
		row.appendChild(nameCell);

		const imageCell = document.createElement("td");
		imageCell.textContent = pkg.image;
		row.appendChild(imageCell);

		const versionCell = document.createElement("td");
		versionCell.textContent = pkg.version;
		row.appendChild(versionCell);

		const stateCell = document.createElement("td");
		/* Issue #101: a package that could not be REACHED and one that
		 * failed to BUILD both read "failed", and they call for
		 * opposite responses -- retry the first, fix the second. The
		 * kind sits next to the state; the message is the hover, so the
		 * table stays readable. */
		stateCell.textContent = pkg.stage ? pkg.state + " (" + pkg.stage + "/" + pkg.status + ")"
		                                          : pkg.state;
		if (pkg.error)
			stateCell.title = pkg.error;
		row.appendChild(stateCell);

		const actionCell = document.createElement("td");
		if (pkg.state === "installed") {
			const rmButton = document.createElement("button");

			rmButton.textContent = "Remove";
			rmButton.className = "button-danger";
			rmButton.addEventListener("click", () => removePkg(pkg.name, pkg.image));
			actionCell.appendChild(rmButton);
		}
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

/*
 * issue #19: GET /pkg/recipes returns one entry per (name,version) --
 * a package with many published versions (cix has 14) used to render
 * as that many separate, equally-weighted rows, and worse, the single
 * "Delete" button on any one of them called DELETE .../recipes/{name}
 * with no ?version= -- silently wiping *every* stored version, not just
 * the row clicked. Real data-loss footgun, not just a display nit.
 *
 * Collapsed to one row per name (the most recently published version,
 * by created_at -- simpler and equally correct here than reimplementing
 * pkg_version_compare()'s dpkg-style comparator in JS, since a real
 * recipe history is only ever appended to, never backdated). Its own
 * Delete button stays real and version-scoped (removePkgRecipe(r.name,
 * r.version), never a bare name-only delete) -- the old implicit
 * "delete removes everything" behavior is gone entirely, not just
 * hidden.
 *
 * issue #44 (2026-08-18): the former inline ▸/▾ "N versions" expand
 * toggle is gone -- older versions now live in the package's own detail
 * page, a real "Versions" tab (renderPackageDetailVersions()) alongside
 * a real per-version changelog, rather than a second copy of this same
 * table growing new rows in place. One browsing surface per concern:
 * this list is "what's the latest of everything," the detail page's
 * Versions tab is "every version of this one thing" -- not two
 * different UI patterns doing the same job depending on how you got
 * there.
 */
function renderRecipesList() {
	const body = document.getElementById("recipes-body");
	const filter = document.getElementById("recipes-pkg-search").value.trim().toLowerCase();

	const groups = new Map();

	for (const r of cache.pkgRecipes) {
		if (!groups.has(r.name))
			groups.set(r.name, []);
		groups.get(r.name).push(r);
	}
	for (const versions of groups.values())
		versions.sort((a, b) => b.created_at - a.created_at);

	const names = [...groups.keys()].filter((n) => n.toLowerCase().includes(filter));

	body.textContent = "";
	if (names.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 4;
		cell.className = "empty";
		cell.textContent = cache.pkgRecipes.length === 0 ? "No recipes" : "No match";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const name of names) {
		const latest = groups.get(name)[0];
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.appendChild(treeLink("#packages/" + encodeURIComponent(name), name, ""));
		row.appendChild(nameCell);

		const versionCell = document.createElement("td");
		versionCell.textContent = latest.version;
		row.appendChild(versionCell);

		const dependsCell = document.createElement("td");
		dependsCell.textContent = latest.depends || "-";
		row.appendChild(dependsCell);

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		rmButton.textContent = "Delete";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removePkgRecipe(latest.name, latest.version));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

/* name is null on the landing list, set on a specific package's own
 * detail page -- both share the same underlying cache.pkgRecipes/
 * cache.pkgList, refreshed independently by refreshPkgRecipes()/
 * refreshPkgList(), so either one completing re-renders whichever of
 * the two is currently on screen (the same "conditional re-render,
 * gated on the active route" convention refreshDevices()/
 * refreshDeviceMaps() already established). */
function renderPackagesView(name) {
	if (name === null)
		renderPackagesList();
	else
		renderPackageDetail(name);
}

/* Recipe *content* (the raw shell script text) isn't part of
 * cache.pkgRecipes' list-view metadata shape -- fetched separately,
 * once per name, and cached here so a poll-driven re-render of the
 * same detail page never re-fetches it every 2s (the same guard
 * openConsole() already uses for its own per-container WebSocket). */
/* version: the daemon's own ADR-0107 resolution for this name (what an
 * omitted ?version= actually resolves to -- dpkg-style HIGHEST version,
 * not newest-published). Cached from the same name-only GET that fetches
 * the content, so the dashboard never grows its own parallel version
 * comparator: one source of truth, the daemon's. */
let pkgRecipeContentCache = { name: null, content: null, version: null };

async function loadPkgRecipeContent(name) {
	if (pkgRecipeContentCache.name === name)
		return pkgRecipeContentCache.content;
	const data = await apiRequest("GET", CIX_API.getPkgRecipe(name));

	pkgRecipeContentCache = { name: name, content: data.content, version: data.version };
	return pkgRecipeContentCache.content;
}

function renderPackageDetail(name) {
	/* issue #19: .find() alone returns whatever GET /pkg/recipes
	 * happened to list first for this name -- not necessarily the
	 * latest version (confirmed live: squashfs-tools' 5 stored
	 * versions come back in on-disk readdir() order, not sorted).
	 *
	 * Which row IS "latest" here is the daemon's call, not this file's:
	 * once the name-only recipe GET has resolved (pkgRecipeContentCache
	 * .version, ADR-0107's dpkg-style highest -- the same version whose
	 * content the Recipe tab actually displays), prefer that exact row.
	 * The old newest-created_at rule stays only as the pre-load
	 * fallback -- it diverges from the daemon's real resolution
	 * whenever versions are published out of numeric order (confirmed
	 * live with gcc: 6.4.0-5 published after 16.2.0-5, daemon resolves
	 * 16.2.0-5, newest-created_at says 6.4.0-5 -- the old rule showed
	 * one version's number over another version's content). */
	const allVersions = cache.pkgRecipes.filter((r) => r.name === name);
	const resolvedVersion = pkgRecipeContentCache.name === name ? pkgRecipeContentCache.version : null;
	const recipe =
		allVersions.length === 0
			? undefined
			: allVersions.find((r) => r.version === resolvedVersion) ||
			  allVersions.reduce((a, b) => (b.created_at > a.created_at ? b : a));
	const missingEl = document.getElementById("pkgd-recipe-missing");
	const presentEl = document.getElementById("pkgd-recipe-present");
	const editBtn = document.getElementById("pkgd-edit-recipe");
	const removeBtn = document.getElementById("pkgd-remove-recipe");

	removeBtn.hidden = !recipe;

	if (recipe) {
		missingEl.hidden = true;
		presentEl.hidden = false;

		const fields = document.getElementById("pkgd-recipe-fields");

		fields.textContent = "";
		fields.appendChild(
			fieldBlock(
				"Version",
				recipe.version +
					(pkgRecipeContentCache.name === name && recipe.version === pkgRecipeContentCache.version
						? " (rolling candidate" + (allVersions.length > 1 ? " -- latest of " + allVersions.length + ", see the Versions tab" : "") + ")"
						: allVersions.length > 1 ? " (latest of " + allVersions.length + " -- see the Versions tab)" : "")
			)
		);
		fields.appendChild(fieldBlock("Depends", recipe.depends || "-"));
		if (recipe.changelog)
			fields.appendChild(fieldBlock("Changelog", recipe.changelog));

		const contentEl = document.getElementById("pkgd-recipe-content");

		if (pkgRecipeContentCache.name === name) {
			contentEl.textContent = pkgRecipeContentCache.content;
		} else {
			contentEl.textContent = "Loading…";
			loadPkgRecipeContent(name)
				.then(() => {
					/* Full re-render, not just the content text: the load
					 * also learned the daemon's resolved version, which
					 * drives the fields block, the Versions tab's rolling-
					 * candidate badge, and which row "latest" means. No
					 * loop -- the cache now matches, so the re-render's own
					 * load call short-circuits. */
					if (onPageOf("packages") && parseHash().name === name)
						renderPackageDetail(name);
				})
				.catch((e) => showStatus("Failed to load recipe content for " + name + ": " + e.message, true));
		}
	} else {
		missingEl.hidden = false;
		presentEl.hidden = true;
	}

	editBtn.onclick = async () => {
		let content = "";

		if (recipe) {
			try {
				content = await loadPkgRecipeContent(name);
			} catch (e) {
				showStatus("Failed to load recipe content for " + name + ": " + e.message, true);
				return;
			}
		}
		openModal("pkg-recipe-form", recipe ? "Edit recipe" : "Add recipe");
		document.getElementById("rf-name").value = name;
		document.getElementById("rf-file").value = "";
		document.getElementById("rf-content").value = content;
	};
	removeBtn.onclick = () => removePkgRecipe(name, recipe.version);

	renderPackageDetailVersions(name, allVersions);
	renderPackageDetailInstalled(name);
}

/* issue #44: every published version for this one name, newest first --
 * already fully present in cache.pkgRecipes (no separate fetch needed,
 * unlike renderImageDetailVersions()'s own GET /images/{name}, since
 * pkg recipe list-view metadata already carries everything a row here
 * needs). Delete here is real, per-version (removePkgRecipe(name,
 * v.version)) -- unlike an image's own immutable version history, a
 * package recipe version genuinely can be deleted on its own. */
function renderPackageDetailVersions(name, allVersions) {
	const body = document.querySelector("#pkgd-versions tbody");
	const versions = [...allVersions].sort((a, b) => b.created_at - a.created_at);

	body.textContent = "";
	if (versions.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 4;
		cell.className = "empty";
		cell.textContent = "No recipe on file for this name";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	versions.forEach((v) => {
		const row = document.createElement("tr");

		const versionCell = document.createElement("td");
		versionCell.textContent = v.version;
		/* The badge marks the DAEMON's own ADR-0107 resolution (cached
		 * from the name-only recipe GET), never a client-side guess.
		 * The first version of this badge marked the newest-published
		 * row (i === 0 after the created_at sort) on the assumption
		 * that versions are only ever published in increasing order --
		 * broken in practice the very session it shipped (gcc: 6.4.0-5
		 * published after 16.2.0-5; the daemon resolves 16.2.0-5, the
		 * old badge sat on 6.4.0-5 -- caught by the user asking what
		 * the selection criteria actually was). Until the resolution
		 * has loaded, no badge at all -- honest blank over a guess;
		 * the post-load re-render fills it in. */
		if (pkgRecipeContentCache.name === name && v.version === pkgRecipeContentCache.version) {
			const badge = document.createElement("strong");

			badge.textContent = " (rolling candidate)";
			versionCell.appendChild(badge);
		}
		row.appendChild(versionCell);

		const changelogCell = document.createElement("td");
		changelogCell.textContent = v.changelog || "-";
		row.appendChild(changelogCell);

		const createdCell = document.createElement("td");
		createdCell.textContent = v.created_at ? new Date(v.created_at * 1000).toLocaleString() : "-";
		row.appendChild(createdCell);

		const actionCell = document.createElement("td");
		const viewButton = document.createElement("button");

		viewButton.textContent = "View…";
		viewButton.addEventListener("click", () => viewPkgRecipeVersion(v.name, v.version));
		actionCell.appendChild(viewButton);

		const rmButton = document.createElement("button");

		rmButton.textContent = "Delete";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removePkgRecipe(v.name, v.version));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	});
}

/* issue reported directly by the user: a specific historical version's
 * own recipe content had no way to be viewed at all -- the Recipe tab
 * only ever shows the latest (loadPkgRecipeContent(name), no
 * ?version=). Reuses the existing edit-recipe modal as a viewer rather
 * than adding a second, read-only-only modal: opened pre-filled with
 * THIS version's real content (a real, version-scoped GET, not the
 * name-only latest one pkgRecipeContentCache holds), title says which
 * version is showing. Submitting it still works exactly like editing
 * the latest does -- publishes a new version via POST /pkg/recipes --
 * which doubles as a real, working "restore an old version" path (edit
 * nothing, just Save) for free, not a separate feature to build. */
async function viewPkgRecipeVersion(name, version) {
	let content;

	try {
		content = (
			await apiRequest("GET", CIX_API.getPkgRecipe(name, version) + "?version=%s")
		).content;
	} catch (e) {
		showStatus("Failed to load recipe content for " + name + "@" + version + ": " + e.message, true);
		return;
	}
	openModal("pkg-recipe-form", name + "@" + version);
	document.getElementById("rf-name").value = name;
	document.getElementById("rf-file").value = "";
	document.getElementById("rf-content").value = content;
}

function renderPackageDetailInstalled(name) {
	const body = document.querySelector("#pkgd-installed tbody");
	const pkgs = cache.pkgList.filter((p) => p.name === name);

	body.textContent = "";
	if (pkgs.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 7;
		cell.className = "empty";
		cell.textContent = "Not installed on any image";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const pkg of pkgs) {
		const row = document.createElement("tr");

		const imageCell = document.createElement("td");
		imageCell.appendChild(treeLink("#images/" + encodeURIComponent(pkg.image), pkg.image, ""));
		row.appendChild(imageCell);

		const versionCell = document.createElement("td");
		versionCell.textContent = pkg.version;
		row.appendChild(versionCell);

		const stateCell = document.createElement("td");
		/* Issue #101: a package that could not be REACHED and one that
		 * failed to BUILD both read "failed", and they call for
		 * opposite responses -- retry the first, fix the second. The
		 * kind sits next to the state; the message is the hover, so the
		 * table stays readable. */
		stateCell.textContent = pkg.stage ? pkg.state + " (" + pkg.stage + "/" + pkg.status + ")"
		                                          : pkg.state;
		if (pkg.error)
			stateCell.title = pkg.error;
		row.appendChild(stateCell);

		const availableCell = document.createElement("td");
		availableCell.textContent = pkg.available_version || "-";
		row.appendChild(availableCell);

		const errorCell = document.createElement("td");
		errorCell.textContent = pkg.error || "-";
		row.appendChild(errorCell);

		const filesCell = document.createElement("td");
		filesCell.textContent = String((pkg.files || []).length);
		row.appendChild(filesCell);

		const actionCell = document.createElement("td");
		if (pkg.state === "installed") {
			const rmButton = document.createElement("button");

			rmButton.textContent = "Remove";
			rmButton.className = "button-danger";
			rmButton.addEventListener("click", async () => {
				await removePkg(pkg.name, pkg.image);
				renderPackageDetailInstalled(name);
			});
			actionCell.appendChild(rmButton);
		}
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshPkgRecipes() {
	const data = await apiRequest("GET", CIX_API.listPkgRecipes());
	cache.pkgRecipes = data.recipes;
	if (onPageOf("packages"))
		renderPackagesView(parseHash().name);
}

/* version is always required from every call site now (issue #19) --
 * the old version-less call silently deleted every stored version of
 * name, a real data-loss footgun once the recipe list started showing
 * one row per version. */
async function removePkgRecipe(name, version) {
	try {
		await apiRequest("DELETE", CIX_API.deletePkgRecipe(name, version) + "?version=%s");
		clearStatus();
		await refreshPkgRecipes();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove recipe " + name + " version " + version + ": " + e.message, true);
	}
}

/* ---- Software Catalogue: Image + Container recipe tabs ----
 * The Packages tab (renderRecipesList()) already existed; these two
 * make the catalogue actually cover all three recipe kinds (ADR-0151)
 * instead of leaving image/container recipes only reachable buried in
 * an image's own detail page (image) or not browsable at all
 * (container, brand new). Both filtered client-side by their own
 * search box -- these lists are small, no server-side search needed. */

async function refreshImageRecipesList() {
	const data = await apiRequest("GET", CIX_API.listImageRecipes());

	cache.imageRecipes = data.recipes;
	if (onPageOf("recipes"))
		renderImageRecipesTable();
}

async function refreshContainerRecipesList() {
	const data = await apiRequest("GET", CIX_API.listDeployments());

	cache.containerRecipes = data.recipes;
	if (onPageOf("recipes"))
		renderContainerRecipesTable();
}

function renderImageRecipesTable() {
	const body = document.getElementById("image-recipes-body");
	const filter = document.getElementById("recipes-image-search").value.trim().toLowerCase();
	const rows = cache.imageRecipes.filter((r) => r.name.toLowerCase().includes(filter));

	body.textContent = "";
	if (rows.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 2;
		cell.className = "empty";
		cell.textContent = cache.imageRecipes.length === 0 ? "No image recipes" : "No match";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const r of rows) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.appendChild(treeLink("#images/" + encodeURIComponent(r.name), r.name, ""));
		row.appendChild(nameCell);

		const actionCell = document.createElement("td");

		const applyButton = document.createElement("button");
		applyButton.textContent = "Apply";
		applyButton.addEventListener("click", async () => {
			try {
				const result = await apiRequest("POST", CIX_API.applyImageRecipe(r.name));

				clearStatus();
				showStatus(
					result && result.state === "running"
						? "Recipe apply started for " + r.name + " (async artifact fetch)"
						: "Recipe applied for " + r.name,
					false
				);
			} catch (e) {
				showStatus("Failed to apply recipe for " + r.name + ": " + e.message, true);
			}
		});
		actionCell.appendChild(applyButton);

		const editButton = document.createElement("button");
		editButton.textContent = "Edit";
		editButton.addEventListener("click", async () => {
			let content = "";

			try {
				content = await loadImageRecipeContent(r.name);
			} catch (e) {
				/* Shouldn't happen for a name the list itself just returned. */
			}
			openModal("image-recipe-form", "Edit image recipe");
			document.getElementById("irf-name").value = r.name;
			document.getElementById("irf-name").readOnly = true;
			document.getElementById("irf-content").value = content;
		});
		actionCell.appendChild(editButton);

		const rmButton = document.createElement("button");
		rmButton.textContent = "Delete";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", async () => {
			try {
				await apiRequest("DELETE", CIX_API.deleteImageRecipe(r.name));
				clearStatus();
				imageRecipeContentCache = { name: null, content: null };
				await refreshImageRecipesList();
			} catch (e) {
				showStatus("Failed to remove image recipe for " + r.name + ": " + e.message, true);
			}
		});
		actionCell.appendChild(rmButton);

		row.appendChild(actionCell);
		body.appendChild(row);
	}
}

let containerRecipeContentCache = { name: null, content: null };

async function loadContainerRecipeContent(name) {
	if (containerRecipeContentCache.name === name)
		return containerRecipeContentCache.content;
	const data = await apiRequest("GET", CIX_API.getDeployment(name));

	containerRecipeContentCache = { name: name, content: data.content };
	return containerRecipeContentCache.content;
}

function renderContainerRecipesTable() {
	const body = document.getElementById("container-recipes-body");
	const filter = document.getElementById("recipes-container-search").value.trim().toLowerCase();
	const rows = cache.containerRecipes.filter((r) => r.name.toLowerCase().includes(filter));

	body.textContent = "";
	if (rows.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 2;
		cell.className = "empty";
		cell.textContent = cache.containerRecipes.length === 0 ? "No container recipes" : "No match";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const r of rows) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.textContent = r.name;
		row.appendChild(nameCell);

		const actionCell = document.createElement("td");

		const applyButton = document.createElement("button");
		applyButton.textContent = "Apply…";
		applyButton.addEventListener("click", () => {
			openModal("container-recipe-apply-form", "Apply recipe: " + r.name);
			document.getElementById("craf-name").value = r.name;
			document.getElementById("craf-secrets").value = "{}";
		});
		actionCell.appendChild(applyButton);

		const editButton = document.createElement("button");
		editButton.textContent = "Edit";
		editButton.addEventListener("click", async () => {
			let content = "";

			try {
				content = await loadContainerRecipeContent(r.name);
			} catch (e) {
				/* Shouldn't happen for a name the list itself just returned. */
			}
			openModal("container-recipe-form", "Edit container recipe");
			document.getElementById("crf-name").value = r.name;
			document.getElementById("crf-content").value = content;
		});
		actionCell.appendChild(editButton);

		const rmButton = document.createElement("button");
		rmButton.textContent = "Delete";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", async () => {
			try {
				await apiRequest("DELETE", CIX_API.deleteDeployment(r.name));
				clearStatus();
				containerRecipeContentCache = { name: null, content: null };
				await refreshContainerRecipesList();
			} catch (e) {
				showStatus("Failed to remove container recipe for " + r.name + ": " + e.message, true);
			}
		});
		actionCell.appendChild(rmButton);

		row.appendChild(actionCell);
		body.appendChild(row);
	}
}

document.getElementById("recipes-pkg-search").addEventListener("input", renderRecipesList);
document.getElementById("recipes-image-search").addEventListener("input", renderImageRecipesTable);
document.getElementById("recipes-container-search").addEventListener("input", renderContainerRecipesTable);

document.getElementById("recipes-pkg-add").addEventListener("click", () => openModal("pkg-recipe-form", "Add recipe"));
document.getElementById("recipes-image-add").addEventListener("click", () => {
	openModal("image-recipe-form", "Add image recipe");
	document.getElementById("irf-name").readOnly = false;
});
document.getElementById("recipes-container-add").addEventListener("click", () => openModal("container-recipe-form", "Add container recipe"));

document.getElementById("container-recipe-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("crf-name").value.trim();
	const fileInput = document.getElementById("crf-file");
	const contentField = document.getElementById("crf-content");

	if (fileInput.files.length === 0 && contentField.value.trim() === "")
		return;

	try {
		const content = fileInput.files.length > 0 ? await readFileAsText(fileInput.files[0]) : contentField.value;

		await apiRequest("POST", CIX_API.addDeployment(), { name: name, content: content });
		clearStatus();
		document.getElementById("container-recipe-form").reset();
		closeModal();
		containerRecipeContentCache = { name: null, content: null };
		await refreshContainerRecipesList();
	} catch (e) {
		showStatus("Failed to save container recipe " + name + ": " + e.message, true);
	}
});

document.getElementById("container-recipe-apply-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("craf-name").value;
	const secretsText = document.getElementById("craf-secrets").value.trim();
	let secrets = {};

	if (secretsText !== "") {
		try {
			secrets = JSON.parse(secretsText);
		} catch (e) {
			showStatus("Secrets must be valid JSON: " + e.message, true);
			return;
		}
	}

	try {
		await apiRequest("POST", CIX_API.applyDeployment(name), { secrets: secrets });
		clearStatus();
		showStatus("Container " + name + " created from recipe.", false);
		document.getElementById("container-recipe-apply-form").reset();
		closeModal();
		await refreshContainers();
		renderTree();
	} catch (e) {
		showStatus("Failed to apply recipe " + name + ": " + e.message, true);
	}
});

/*
 * How far behind this host is, shown next to the verb that fixes it.
 *
 * GET /pkg/{name} has always carried available_version per package, and
 * nothing aggregated it, so drift accumulated unseen -- 34 of 139
 * installed packages on the reference host, found by accident (#217).
 * A number nobody can see is a number nobody acts on, which is why this
 * sits beside "Update all" rather than on a page of its own.
 */
async function refreshPkgDrift() {
	const el = document.getElementById("pkg-drift-summary");

	if (!el)
		return;
	try {
		const d = await apiRequest("GET", CIX_API.getPkgDrift());

		if (!d || typeof d.behind !== "number") {
			el.textContent = "";
			return;
		}
		el.textContent = d.behind === 0
			? d.installed + " installed, all current"
			: d.behind + " of " + d.installed + " behind their recipes";
	} catch (e) {
		/* Never let a status line break the page it decorates. */
		el.textContent = "";
	}
}

async function refreshPkgList() {
	const data = await apiRequest("GET", CIX_API.listPkg());
	cache.pkgList = data.packages;
	if (onPageOf("packages"))
		renderPackagesView(parseHash().name);
	/* Kept in step with the list it describes rather than refreshed
	 * separately, so the two can never disagree on screen. */
	refreshPkgDrift();
}

/*
 * Issue #213: stop an in-flight build.
 *
 * Offered only while a build is actually running, because the endpoint
 * refuses anything else -- a button that exists to return 409 teaches
 * people to ignore buttons. The judgement it supports is the operator's:
 * a build silent for an hour is either a long link or something waiting
 * on stdin, and the package row shows how long it has been quiet.
 */
async function cancelPkgBuild(name, image) {
	const key = image && image !== "base" ? name + "@" + image : name;

	try {
		await apiRequest("POST", CIX_API.pkgCancel(), { name: name, image: image });
		clearStatus();
		await refreshPkgList();
	} catch (e) {
		showStatus("Failed to cancel the build of " + key + ": " + e.message, true);
	}
}

async function removePkg(name, image) {
	const key = image && image !== "base" ? name + "@" + image : name;

	try {
		await apiRequest("DELETE", CIX_API.deletePkg(key));
		clearStatus();
		await refreshPkgList();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove package " + key + ": " + e.message, true);
	}
}

/* ---- Package repo config + sync (ADR-0121) ---- */

let pkgRepoConfigDirty = false;

async function refreshPkgRepoConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getPkgRepoConfig());

		cache.pkgRepoConfig = config;
		if (!pkgRepoConfigDirty) {
			document.getElementById("prc-url").value = config.repo_url || "";
			document.getElementById("prc-kind").value = config.repo_kind || "gitea";
			document.getElementById("prc-ref").value = config.ref || "";
			document.getElementById("prc-token").value = "";
			document.getElementById("prc-token").placeholder =
				config.auth_token_set ? "(unchanged, a token is set)" : "(unchanged, no token set)";
		}
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

for (const id of ["prc-url", "prc-kind", "prc-ref", "prc-token", "prc-clear-token"]) {
	document.getElementById(id).addEventListener("input", () => {
		pkgRepoConfigDirty = true;
	});
	document.getElementById(id).addEventListener("change", () => {
		pkgRepoConfigDirty = true;
	});
}

document.getElementById("prc-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const body = {
		repo_url: document.getElementById("prc-url").value.trim(),
		repo_kind: document.getElementById("prc-kind").value,
		ref: document.getElementById("prc-ref").value.trim(),
	};
	const token = document.getElementById("prc-token").value;

	/* "" explicitly clears an already-configured token (pkg_repo_set_
	 * config()'s own NULL-vs-empty-string contract) -- omitting the
	 * field entirely (the common case, token left blank and not
	 * clearing) leaves whatever's already configured untouched. */
	if (document.getElementById("prc-clear-token").checked)
		body.auth_token = "";
	else if (token !== "")
		body.auth_token = token;

	try {
		await apiRequest("PUT", CIX_API.putPkgRepoConfig(), body);
		clearStatus();
		showStatus("Package repo config saved", false);
		pkgRepoConfigDirty = false;
		document.getElementById("prc-clear-token").checked = false;
		await refreshPkgRepoConfig();
	} catch (e) {
		showStatus("Failed to save package repo config: " + e.message, true);
	}
});

async function refreshPkgSyncStatus() {
	const box = document.getElementById("pkg-sync-status-box");

	try {
		const status = await apiRequest("GET", CIX_API.getPkgSyncStatus());

		cache.pkgSyncStatus = status;
		box.textContent = "";

		const lines = [
			["State", status.state],
			["Last attempt", status.last_attempt || "(never)"],
			["Added", status.added],
			["Skipped", status.skipped],
			["Error", status.error || "-"],
		];
		for (const [label, value] of lines) {
			const p = document.createElement("p");
			p.textContent = label + ": " + value;
			box.appendChild(p);
		}
	} catch (e) {
		box.textContent = "Failed to load sync status: " + e.message;
	}
}

document.getElementById("pkg-sync-now").addEventListener("click", async () => {
	try {
		await apiRequest("POST", CIX_API.pkgSync());
		clearStatus();
		showStatus("Package sync started", false);
		await refreshPkgSyncStatus();
	} catch (e) {
		showStatus("Failed to start package sync: " + e.message, true);
	}
});

/* ---- Package cache + artifact server config (ADR-0122) ---- */

let pkgCacheConfigDirty = false;

async function refreshPkgCacheConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getPkgCacheConfig());

		cache.pkgCacheConfig = config;
		if (!pkgCacheConfigDirty)
			document.getElementById("pcc-max-bytes").value = config.max_bytes;
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

document.getElementById("pcc-max-bytes").addEventListener("input", () => {
	pkgCacheConfigDirty = true;
});

document.getElementById("pcc-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	try {
		await apiRequest("PUT", CIX_API.putPkgCacheConfig(), {
			max_bytes: parseInt(document.getElementById("pcc-max-bytes").value, 10),
		});
		clearStatus();
		showStatus("Cache config saved", false);
		pkgCacheConfigDirty = false;
		await refreshPkgCacheConfig();
	} catch (e) {
		showStatus("Failed to save cache config: " + e.message, true);
	}
});

async function refreshPkgCacheStatus() {
	const box = document.getElementById("pkg-cache-status-box");

	try {
		const status = await apiRequest("GET", CIX_API.getPkgCache());

		cache.pkgCacheStatus = status;
		box.textContent = "";

		const lines = [
			["Entries", status.entry_count],
			["Current size (bytes)", status.current_bytes],
			["Max size (bytes)", status.max_bytes],
		];
		for (const [label, value] of lines) {
			const p = document.createElement("p");
			p.textContent = label + ": " + value;
			box.appendChild(p);
		}
	} catch (e) {
		box.textContent = "Failed to load cache status: " + e.message;
	}
}

document.getElementById("pkg-cache-clear").addEventListener("click", async () => {
	if (!confirm("Clear the entire local package artifact cache?"))
		return;
	try {
		await apiRequest("DELETE", CIX_API.deletePkgCache());
		clearStatus();
		showStatus("Cache cleared", false);
		await refreshPkgCacheStatus();
	} catch (e) {
		showStatus("Failed to clear cache: " + e.message, true);
	}
});

/* ---- Rolling-restart jitter config (Part 5, ADR-0124) ---- */

let rollingConfigDirty = false;

/* ---- ADR-0152: hostauth session listing/revoke -- real admin
 * visibility into a table that previously had none (no endpoint, no
 * CLI, no web panel) ---- */

async function refreshHostauthSessions() {
	try {
		const data = await apiRequest("GET", CIX_API.listHostauthSessions());

		renderHostauthSessions(data.sessions || []);
	} catch (e) {
		showStatus("Failed to load active sessions: " + e.message, true);
	}
}

function renderHostauthSessions(sessions) {
	const body = document.getElementById("hostauth-sessions-body");

	body.textContent = "";
	if (sessions.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 3;
		cell.className = "empty";
		cell.textContent = "No active sessions";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const s of sessions) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.textContent = s.username;
		row.appendChild(nameCell);

		const expiresCell = document.createElement("td");
		expiresCell.textContent =
			s.expires_in_seconds === null || s.expires_in_seconds === undefined
				? "single-use (no idle timeout configured)"
				: s.expires_in_seconds + "s";
		row.appendChild(expiresCell);

		const actionCell = document.createElement("td");
		const revokeButton = document.createElement("button");

		revokeButton.textContent = "Log out everywhere";
		revokeButton.className = "button-danger";
		revokeButton.addEventListener("click", async () => {
			try {
				await apiRequest("DELETE", CIX_API.revokeHostauthSessions(s.username));
				clearStatus();
				await refreshHostauthSessions();
			} catch (e) {
				showStatus("Failed to revoke sessions for " + s.username + ": " + e.message, true);
			}
		});
		actionCell.appendChild(revokeButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshRollingConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getSystemRollingConfig());

		cache.rollingConfig = config;
		if (!rollingConfigDirty)
			document.getElementById("rc-jitter-window").value = config.jitter_window_seconds;
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

document.getElementById("rc-jitter-window").addEventListener("input", () => {
	rollingConfigDirty = true;
});

document.getElementById("rc-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	try {
		await apiRequest("PUT", CIX_API.putSystemRollingConfig(), {
			jitter_window_seconds: parseInt(document.getElementById("rc-jitter-window").value, 10),
		});
		clearStatus();
		showStatus("Rolling-restart config saved", false);
		rollingConfigDirty = false;
		await refreshRollingConfig();
	} catch (e) {
		showStatus("Failed to save rolling-restart config: " + e.message, true);
	}
});

/* ---- ADR-0157 Phase 3: pkg-build concurrency config ---- */

let pkgBuildConfigDirty = false;

async function refreshPkgBuildConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getSystemPkgBuildConfig());

		cache.pkgBuildConfig = config;
		if (!pkgBuildConfigDirty) {
			document.getElementById("pbc-max-jobs").value = config.max_concurrent_jobs;
			document.getElementById("pbc-memory-max").value = config.memory_max;
			document.getElementById("pbc-cpu-max").value = config.cpu_max || "";
		}
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

for (const id of ["pbc-max-jobs", "pbc-memory-max", "pbc-cpu-max"]) {
	document.getElementById(id).addEventListener("input", () => {
		pkgBuildConfigDirty = true;
	});
}

document.getElementById("pbc-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	try {
		const cpuMax = document.getElementById("pbc-cpu-max").value.trim();

		await apiRequest("PUT", CIX_API.putSystemPkgBuildConfig(), {
			max_concurrent_jobs: parseInt(document.getElementById("pbc-max-jobs").value, 10),
			memory_max: parseInt(document.getElementById("pbc-memory-max").value, 10),
			cpu_max: cpuMax === "" ? null : cpuMax,
		});
		clearStatus();
		showStatus("Package-build config saved", false);
		pkgBuildConfigDirty = false;
		await refreshPkgBuildConfig();
	} catch (e) {
		showStatus("Failed to save package-build config: " + e.message, true);
	}
});

let pkgArtifactConfigDirty = false;

async function refreshPkgArtifactConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getPkgArtifactConfig());

		cache.pkgArtifactConfig = config;
		if (!pkgArtifactConfigDirty) {
			document.getElementById("pac-base-url").value = config.base_url || "";
			document.getElementById("pac-token").value = "";
			document.getElementById("pac-token").placeholder =
				config.auth_token_set ? "(unchanged, a token is set)" : "(unchanged, no token set)";
		}
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

for (const id of ["pac-base-url", "pac-token", "pac-clear-token"]) {
	document.getElementById(id).addEventListener("input", () => {
		pkgArtifactConfigDirty = true;
	});
}

document.getElementById("pac-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const body = { base_url: document.getElementById("pac-base-url").value.trim() };
	const token = document.getElementById("pac-token").value;

	if (document.getElementById("pac-clear-token").checked)
		body.auth_token = "";
	else if (token !== "")
		body.auth_token = token;

	try {
		await apiRequest("PUT", CIX_API.putPkgArtifactConfig(), body);
		clearStatus();
		showStatus("Artifact server config saved", false);
		pkgArtifactConfigDirty = false;
		document.getElementById("pac-clear-token").checked = false;
		await refreshPkgArtifactConfig();
	} catch (e) {
		showStatus("Failed to save artifact server config: " + e.message, true);
	}
});

/* ---------- Container create form: devices/interfaces pickers ---------- */

function populateContainerFormDeviceLists() {
	const deviceSelect = document.getElementById("f-devices");
	const ifaceSelect = document.getElementById("f-interfaces");
	const prevDevices = new Set(Array.from(deviceSelect.selectedOptions).map((o) => o.value));
	const prevIfaces = new Set(Array.from(ifaceSelect.selectedOptions).map((o) => o.value));

	deviceSelect.textContent = "";
	ifaceSelect.textContent = "";

	const assignable = cache.devices.filter((d) => d.assignable);

	/* GPU member ids collapse to their gpu:N group prefix -- granting
	 * a container "gpu:0" is what actually works (device_find_group()),
	 * granting "gpu:0:card0" alone is not the intended unit. */
	const seenGpuGroups = new Set();

	for (const d of assignable) {
		if (d.bus === "net") {
			const opt = document.createElement("option");

			opt.value = d.id;
			opt.textContent = d.id + (d.description ? " (" + d.description + ")" : "");
			if (prevIfaces.has(opt.value))
				opt.selected = true;
			ifaceSelect.appendChild(opt);
			continue;
		}

		let value = d.id;
		let label = d.id + (d.description ? " (" + d.description + ")" : "");

		if (d.bus === "gpu") {
			const parts = d.id.split(":");
			const groupId = parts[0] + ":" + parts[1];

			if (seenGpuGroups.has(groupId))
				continue;
			seenGpuGroups.add(groupId);
			value = groupId;
			label = groupId + " (whole GPU group)";
		}

		const opt = document.createElement("option");

		opt.value = value;
		opt.textContent = label;
		if (prevDevices.has(value))
			opt.selected = true;
		deviceSelect.appendChild(opt);
	}
}

let fileRowCount = 0;

function addFileRow() {
	const container = document.getElementById("f-files-rows");
	const rowId = "file-row-" + fileRowCount++;
	const row = document.createElement("div");

	row.className = "file-row";
	row.id = rowId;

	const pathLabel = document.createElement("label");

	pathLabel.textContent = "Container path";
	const pathInput = document.createElement("input");

	pathInput.type = "text";
	pathInput.placeholder = "/etc/app/config.toml";
	pathInput.className = "file-row-path";
	pathLabel.appendChild(pathInput);
	row.appendChild(pathLabel);

	const fileLabel = document.createElement("label");

	fileLabel.textContent = "Local file";
	const fileInput = document.createElement("input");

	fileInput.type = "file";
	fileInput.className = "file-row-file";
	fileLabel.appendChild(fileInput);
	row.appendChild(fileLabel);

	const modeLabel = document.createElement("label");

	modeLabel.textContent = "Mode (optional)";
	const modeInput = document.createElement("input");

	modeInput.type = "text";
	modeInput.placeholder = "0644";
	modeInput.className = "file-row-mode";
	modeLabel.appendChild(modeInput);
	row.appendChild(modeLabel);

	const rmButton = document.createElement("button");

	rmButton.type = "button";
	rmButton.textContent = "Remove";
	rmButton.className = "button-danger button-small";
	rmButton.addEventListener("click", () => row.remove());
	row.appendChild(rmButton);

	container.appendChild(row);
}

document.getElementById("f-files-add").addEventListener("click", addFileRow);

/* ---------- form submit handlers ---------- */

document.getElementById("run-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("f-name").value.trim();
	const image = document.getElementById("f-image").value.trim();
	const servicesText = document.getElementById("f-services").value;
	const memoryMaxText = document.getElementById("f-memory-max").value.trim();
	const memorySwapMaxText = document.getElementById("f-memory-swap-max").value.trim();
	const pidsMaxText = document.getElementById("f-pids-max").value.trim();
	const cpuMaxText = document.getElementById("f-cpu-max").value.trim();
	const cpusetText = document.getElementById("f-cpuset").value.trim();
	const diskQuotaText = document.getElementById("f-disk-quota").value.trim();
	const network = document.getElementById("f-network").value.trim();
	const ipForward = document.getElementById("f-ip-forward").checked;
	const dnsRegister = document.getElementById("f-dns-register").checked;
	/* Issue #77 surface parity -- REST-only until now. */
	const userns = document.getElementById("f-userns").checked;
	const ldapClient = document.getElementById("f-ldap-client").checked;
	const ldapAllowGroupsText = document.getElementById("f-ldap-allow-groups").value.trim();
	const captureOutput = document.getElementById("f-capture-output").checked;
	const routesText = document.getElementById("f-routes").value.trim();
	const dnsServersText = document.getElementById("f-dns-servers").value.trim();
	const devices = Array.from(document.getElementById("f-devices").selectedOptions).map((o) => o.value);
	const interfaces = Array.from(document.getElementById("f-interfaces").selectedOptions).map((o) => o.value.replace(/^net:/, ""));
	const restart = document.getElementById("f-restart").value;
	const restartDelayText = document.getElementById("f-restart-delay").value.trim();
	const followRolling = document.getElementById("f-follow-rolling").checked;
	const followRollingJitterText = document.getElementById("f-follow-rolling-jitter").value.trim();
	const dependsOnText = document.getElementById("f-depends-on").value.trim();
	const sysctlsText = document.getElementById("f-sysctls").value.trim();
	const envText = document.getElementById("f-env").value.trim();
	const pkiIssue = document.getElementById("f-pki-issue").checked;
	const pkiCertDir = document.getElementById("f-pki-cert-dir").value.trim();
	const pkiDaysText = document.getElementById("f-pki-days").value.trim();
	const ldapProvision = document.getElementById("f-ldap-provision").checked;
	const ldapGroup = document.getElementById("f-ldap-group").value.trim();
	const ldapUser = document.getElementById("f-ldap-user").value.trim();
	const ldapUidText = document.getElementById("f-ldap-uid").value.trim();
	const ldapSecretDir = document.getElementById("f-ldap-secret-dir").value.trim();

	const services = parseServicesText(servicesText);
	if (services === null)
		return;
	const body = {
		name: name,
		image: image,
		services: services,
	};
	if (memoryMaxText !== "")
		body.memory_max = parseInt(memoryMaxText, 10);
	/* Blank is unset, and 0 is a real setting -- so this is a check for
	 * an empty field, never for a falsy number. */
	if (memorySwapMaxText !== "")
		body.memory_swap_max = parseInt(memorySwapMaxText, 10);
	if (pidsMaxText !== "")
		body.pids_max = parseInt(pidsMaxText, 10);
	if (cpuMaxText !== "")
		body.cpu_max = cpuMaxText;
	if (cpusetText !== "")
		body.cpuset_cpus = cpusetText;
	if (diskQuotaText !== "")
		body.disk_quota_bytes = parseInt(diskQuotaText, 10);
	if (network !== "") {
		body.networks = network
			.split(",")
			.map((s) => s.trim())
			.filter((s) => s.length > 0)
			.map((entry) => {
				const colon = entry.indexOf(":");

				return colon < 0 ? entry : { name: entry.slice(0, colon), ip: entry.slice(colon + 1) };
			});
	}
	if (ipForward)
		body.ip_forward = true;
	if (dnsRegister)
		body.dns_register = true;
	if (userns)
		body.userns = true;
	if (ldapClient)
		body.ldap_client = true;
	if (ldapAllowGroupsText !== "") {
		body.ldap_allow_groups = ldapAllowGroupsText
			.split(",")
			.map((s) => s.trim())
			.filter((s) => s.length > 0);
	}
	if (captureOutput)
		body.capture_output = true;
	if (routesText !== "") {
		body.routes = routesText
			.split(",")
			.map((s) => s.trim())
			.filter((s) => s.length > 0)
			.map((entry) => {
				const slashIdx = entry.indexOf("/");
				const colonIdx = entry.indexOf(":", slashIdx + 1);

				return {
					dest: entry.slice(0, slashIdx),
					prefix_len: parseInt(entry.slice(slashIdx + 1, colonIdx), 10),
					via: entry.slice(colonIdx + 1),
				};
			});
	}
	if (dnsServersText !== "") {
		body.dns_servers = dnsServersText
			.split(",")
			.map((s) => s.trim())
			.filter((s) => s.length > 0);
	}
	if (devices.length > 0)
		body.devices = devices;
	if (interfaces.length > 0)
		body.interfaces = interfaces;
	if (restart !== "no")
		body.restart = restart;
	if (restartDelayText !== "")
		body.restart_delay_seconds = parseInt(restartDelayText, 10);
	if (followRolling)
		body.follow_rolling = true;
	if (followRollingJitterText !== "")
		body.follow_rolling_jitter_seconds = parseInt(followRollingJitterText, 10);
	if (dependsOnText !== "") {
		body.depends_on = dependsOnText
			.split(",")
			.map((s) => s.trim())
			.filter((s) => s.length > 0);
	}
	if (sysctlsText !== "") {
		body.sysctls = {};
		for (const pair of sysctlsText.split(";").map((s) => s.trim()).filter((s) => s.length > 0)) {
			const eq = pair.indexOf("=");

			if (eq > 0)
				body.sysctls[pair.slice(0, eq)] = pair.slice(eq + 1);
		}
	}
	if (envText !== "") {
		body.env = {};
		for (const pair of envText.split(";").map((s) => s.trim()).filter((s) => s.length > 0)) {
			const eq = pair.indexOf("=");

			if (eq > 0)
				body.env[pair.slice(0, eq)] = pair.slice(eq + 1);
		}
	}
	if (pkiIssue) {
		body.pki_issue = true;
		if (pkiCertDir !== "")
			body.pki_cert_dir = pkiCertDir;
		if (pkiDaysText !== "")
			body.pki_days = parseInt(pkiDaysText, 10);
	}
	if (ldapProvision) {
		body.ldap_provision = true;
		if (ldapGroup !== "")
			body.ldap_group = ldapGroup;
		if (ldapUser !== "")
			body.ldap_user = ldapUser;
		if (ldapUidText !== "")
			body.ldap_uid = parseInt(ldapUidText, 10);
		if (ldapSecretDir !== "")
			body.ldap_secret_dir = ldapSecretDir;
	}

	try {
		const fileRows = document.querySelectorAll("#f-files-rows .file-row");

		if (fileRows.length > 0) {
			body.files = [];
			for (const row of fileRows) {
				const path = row.querySelector(".file-row-path").value.trim();
				const fileInput = row.querySelector(".file-row-file");
				const mode = row.querySelector(".file-row-mode").value.trim();

				if (path === "" || fileInput.files.length === 0)
					continue;
				const content = await readFileAsText(fileInput.files[0]);
				const entry = { path: path, content: content };

				if (mode !== "")
					entry.mode = mode;
				body.files.push(entry);
			}
		}

		await apiRequest("POST", CIX_API.createContainer(), body);
		clearStatus();
		document.getElementById("run-form").reset();
		document.getElementById("f-files-rows").textContent = "";
		closeModal();
		await refreshContainers();
		renderTree();
	} catch (e) {
		showStatus("Failed to create container: " + e.message, true);
	}
});

document.getElementById("network-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("nf-name").value.trim();
	const subnet = document.getElementById("nf-subnet").value.trim();
	const prefixText = document.getElementById("nf-prefix").value.trim();
	const address = document.getElementById("nf-address").value.trim();

	const body = {
		name: name,
		subnet: subnet,
		prefix_len: parseInt(prefixText, 10),
	};
	if (address)
		body.address = address;

	try {
		await apiRequest("POST", CIX_API.createNetwork(), body);
		clearStatus();
		document.getElementById("network-form").reset();
		closeModal();
		await refreshNetworks();
		renderTree();
	} catch (e) {
		showStatus("Failed to create network: " + e.message, true);
	}
});

document.getElementById("devicemap-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const body = {
		name: document.getElementById("dmf-name").value.trim(),
		kind: document.getElementById("dmf-kind").value,
		selector: document.getElementById("dmf-selector").value.trim(),
	};

	try {
		await apiRequest("POST", CIX_API.createDeviceMap(), body);
		clearStatus();
		document.getElementById("devicemap-form").reset();
		closeModal();
		await refreshDeviceMaps();
		renderTree();
	} catch (e) {
		showStatus("Failed to create device mapping: " + e.message, true);
	}
});

document.getElementById("nd-attach-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const route = parseHash();
	const ifname = document.getElementById("nd-attach-ifname").value;
	const vlanText = document.getElementById("nd-attach-vlan").value.trim();

	if (route.name === null || ifname === "")
		return;
	await attachInterface(route.name, ifname, vlanText !== "" ? parseInt(vlanText, 10) : undefined);
	document.getElementById("nd-attach-form").reset();
});

async function addRoute(isDefault, dest, prefix, gateway) {
	try {
		const body = {};

		if (!isDefault) {
			body.dest = dest;
			body.prefix = prefix;
		}
		if (gateway)
			body.gateway = gateway;
		await apiRequest("POST", CIX_API.postSystemRoute(), body);
		clearStatus();
		await refreshRoutes();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to add route: " + e.message, true);
	}
}

document.getElementById("rf-default").addEventListener("change", (event) => {
	const disabled = event.target.checked;

	document.getElementById("rf-dest").disabled = disabled;
	document.getElementById("rf-prefix").disabled = disabled;
});

document.getElementById("route-add-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const isDefault = document.getElementById("rf-default").checked;
	const dest = document.getElementById("rf-dest").value.trim();
	const prefixText = document.getElementById("rf-prefix").value.trim();
	const gateway = document.getElementById("rf-gateway").value.trim();

	if (!isDefault && (dest === "" || prefixText === "")) {
		showStatus("Destination and prefix are required unless Default route is checked.", true);
		return;
	}
	await addRoute(isDefault, dest, prefixText !== "" ? parseInt(prefixText, 10) : undefined, gateway);
	document.getElementById("route-add-form").reset();
	document.getElementById("rf-dest").disabled = false;
	document.getElementById("rf-prefix").disabled = false;
	closeModal();
});

document.getElementById("image-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("if-name").value.trim();

	try {
		await apiRequest("POST", CIX_API.createImage(), { name: name });
		clearStatus();
		document.getElementById("image-form").reset();
		closeModal();
		await refreshImages();
		renderTree();
	} catch (e) {
		showStatus("Failed to create image: " + e.message, true);
	}
});

document.getElementById("dns-record-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("df-name").value.trim();
	const ip = document.getElementById("df-ip").value.trim();

	try {
		if (dnsRecordEditName !== null)
			await apiRequest("PUT", CIX_API.updateDnsRecord(dnsRecordEditName), { ip: ip });
		else
			await apiRequest("POST", CIX_API.createDnsRecord(), { name: name, ip: ip });
		clearStatus();
		document.getElementById("dns-record-form").reset();
		closeModal();
		await refreshDnsRecords();
	} catch (e) {
		showStatus((dnsRecordEditName !== null ? "Failed to update" : "Failed to create") + " DNS record: " + e.message, true);
	}
});

document.getElementById("dns-server-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const container = document.getElementById("sf-container").value.trim();
	const hostsPath = document.getElementById("sf-hosts-path").value.trim();

	try {
		await apiRequest("POST", CIX_API.createDnsServer(), { container: container, hosts_path: hostsPath });
		clearStatus();
		document.getElementById("dns-server-form").reset();
		closeModal();
		await refreshDnsServers();
	} catch (e) {
		showStatus("Failed to register DNS server: " + e.message, true);
	}
});

document.getElementById("ldap-server-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const container = document.getElementById("lf-container").value.trim();
	const configPath = document.getElementById("lf-config-path").value.trim();

	try {
		await apiRequest("POST", CIX_API.createLdapServer(), { container: container, config_path: configPath });
		clearStatus();
		document.getElementById("ldap-server-form").reset();
		closeModal();
		await refreshLdapServers();
	} catch (e) {
		showStatus("Failed to register LDAP server: " + e.message, true);
	}
});

document.getElementById("ldap-group-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("lgf-name").value.trim();
	const gidnumberRaw = document.getElementById("lgf-gidnumber").value;

	try {
		if (ldapGroupEditName !== null) {
			await apiRequest("PUT", CIX_API.updateLdapGroup(ldapGroupEditName), {
				gidnumber: parseInt(gidnumberRaw, 10),
			});
		} else {
			const body = { name: name };
			if (gidnumberRaw !== "")
				body.gidnumber = parseInt(gidnumberRaw, 10);
			await apiRequest("POST", CIX_API.createLdapGroup(), body);
		}
		clearStatus();
		document.getElementById("ldap-group-form").reset();
		closeModal();
		await refreshLdapGroups();
	} catch (e) {
		showStatus((ldapGroupEditName !== null ? "Failed to update" : "Failed to create") + " LDAP group: " + e.message, true);
	}
});

document.getElementById("ldap-user-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	/* Comma-separated GID numbers, matching cixctl's own
	 * --secondary-groups= parsing convention exactly -- PUT is a real
	 * full-field-replacement (same as every other field here), so this
	 * is always sent, defaulting to an empty array (no secondary
	 * groups) rather than omitted, the same way every other field on
	 * this form already behaves whether editing or creating. */
	const secondaryGroups = document
		.getElementById("luf-secondary-groups")
		.value.split(",")
		.map((s) => s.trim())
		.filter((s) => s !== "")
		.map((s) => parseInt(s, 10));

	const body = {
		name: document.getElementById("luf-name").value.trim(),
		primarygroup: parseInt(document.getElementById("luf-primarygroup").value, 10),
		secondary_groups: secondaryGroups,
		givenname: document.getElementById("luf-givenname").value.trim(),
		sn: document.getElementById("luf-sn").value.trim(),
		mail: document.getElementById("luf-mail").value.trim(),
		loginshell: document.getElementById("luf-loginshell").value.trim(),
		homedirectory: document.getElementById("luf-homedirectory").value.trim(),
		password: document.getElementById("luf-password").value,
		ssh_public_key: document.getElementById("luf-ssh-key").value.trim(),
		disabled: document.getElementById("luf-disabled").checked,
	};
	const uidnumberRaw = document.getElementById("luf-uidnumber").value;

	if (uidnumberRaw !== "")
		body.uidnumber = parseInt(uidnumberRaw, 10);

	try {
		if (ldapUserEditName !== null)
			await apiRequest("PUT", CIX_API.updateLdapUser(ldapUserEditName), body);
		else
			await apiRequest("POST", CIX_API.createLdapUser(), body);
		clearStatus();
		document.getElementById("ldap-user-form").reset();
		closeModal();
		await refreshLdapUsers();
	} catch (e) {
		showStatus((ldapUserEditName !== null ? "Failed to update" : "Failed to create") + " LDAP user: " + e.message, true);
	}
});

document.getElementById("pki-ca-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const commonName = document.getElementById("caf-common-name").value.trim();
	const daysText = document.getElementById("caf-days").value.trim();
	const body = {};

	if (commonName !== "")
		body.common_name = commonName;
	if (daysText !== "")
		body.days = parseInt(daysText, 10);

	try {
		await apiRequest("POST", CIX_API.createPkiCa(), body);
		clearStatus();
		document.getElementById("pki-ca-form").reset();
		await refreshPkiCa();
	} catch (e) {
		showStatus("Failed to bootstrap CA: " + e.message, true);
	}
});

document.getElementById("pki-intermediate-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const commonName = document.getElementById("icaf-common-name").value.trim();
	const daysText = document.getElementById("icaf-days").value.trim();
	const body = {};

	if (commonName !== "")
		body.common_name = commonName;
	if (daysText !== "")
		body.days = parseInt(daysText, 10);

	try {
		await apiRequest("POST", CIX_API.createPkiIntermediate(), body);
		clearStatus();
		document.getElementById("pki-intermediate-form").reset();
		await refreshPkiIntermediate();
	} catch (e) {
		showStatus("Failed to bootstrap intermediate: " + e.message, true);
	}
});

document.getElementById("pki-reset-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	if (!confirm("Wipe and regenerate the entire CA chain? Every previously-issued " +
	             "certificate stops verifying against the new root the moment this completes."))
		return;

	const rootCn = document.getElementById("resetf-root-cn").value.trim();
	const intermediateCn = document.getElementById("resetf-intermediate-cn").value.trim();
	const body = {};

	if (rootCn !== "")
		body.root_common_name = rootCn;
	if (intermediateCn !== "")
		body.intermediate_common_name = intermediateCn;

	try {
		const result = await apiRequest("POST", CIX_API.resetPki(), body);
		clearStatus();
		document.getElementById("pki-reset-form").reset();

		const resultBox = document.getElementById("pki-reset-result");
		const reissued = Array.isArray(result.reissued) ? result.reissued : [];
		const names = reissued.map((c) => c.name).join(", ");

		resultBox.hidden = false;
		resultBox.textContent = "Reset complete. Root: " + (result.root ? result.root.subject : "?") +
			(result.intermediate ? " / Intermediate: " + result.intermediate.subject : "") +
			(reissued.length > 0 ? " / Reissued: " + names : " / No leaves to reissue");

		await refreshPkiCa();
		await refreshPkiIntermediate();
		await refreshPkiCerts();
	} catch (e) {
		showStatus("Failed to reset CA chain: " + e.message, true);
	}
});

document.getElementById("pki-cert-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("pf-name").value.trim();
	const sansText = document.getElementById("pf-sans").value.trim();
	const daysText = document.getElementById("pf-days").value.trim();
	const body = { name: name };

	if (sansText !== "") {
		body.sans = sansText
			.split(",")
			.map((s) => s.trim())
			.filter((s) => s.length > 0);
	}
	if (daysText !== "")
		body.days = parseInt(daysText, 10);

	try {
		const issued = await apiRequest("POST", CIX_API.createPkiCert(), body);
		clearStatus();
		document.getElementById("pki-cert-form").reset();
		await refreshPkiCerts();

		/* The response is the ONLY place key_pem is ever returned --
		 * shown once here, built with textContent (never innerHTML)
		 * since cert_pem/key_pem are PEM text, not markup. */
		const pkiCertIssued = document.getElementById("pki-cert-issued");

		pkiCertIssued.textContent = "";
		const heading = document.createElement("h3");

		heading.textContent = "Issued " + issued.name + " -- copy the private key now, it won't be shown again";
		pkiCertIssued.appendChild(heading);

		const certLabel = document.createElement("div");

		certLabel.textContent = "Certificate:";
		pkiCertIssued.appendChild(certLabel);
		const certPre = document.createElement("pre");

		certPre.textContent = issued.cert_pem;
		pkiCertIssued.appendChild(certPre);

		const keyLabel = document.createElement("div");

		keyLabel.textContent = "Private key:";
		pkiCertIssued.appendChild(keyLabel);
		const keyPre = document.createElement("pre");

		keyPre.textContent = issued.key_pem;
		pkiCertIssued.appendChild(keyPre);

		pkiCertIssued.hidden = false;
	} catch (e) {
		showStatus("Failed to issue certificate: " + e.message, true);
	}
});

document.getElementById("pkg-recipe-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("rf-name").value.trim();
	const fileInput = document.getElementById("rf-file");
	const contentField = document.getElementById("rf-content");

	if (fileInput.files.length === 0 && contentField.value.trim() === "")
		return;

	try {
		const content = fileInput.files.length > 0 ? await readFileAsText(fileInput.files[0]) : contentField.value;

		await apiRequest("POST", CIX_API.addPkgRecipe(), { name: name, content: content });
		clearStatus();
		document.getElementById("pkg-recipe-form").reset();
		closeModal();
		if (pkgRecipeContentCache.name === name)
			pkgRecipeContentCache = { name: null, content: null };
		await refreshPkgRecipes();
		renderTree();
	} catch (e) {
		showStatus("Failed to add recipe " + name + ": " + e.message, true);
	}
});

document.getElementById("pkg-bootstrap-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const toolchainPath = document.getElementById("pkgf-toolchain-path").value.trim();
	const body = toolchainPath ? { toolchain_path: toolchainPath } : {};

	try {
		await apiRequest("POST", CIX_API.pkgBootstrap(), body);
		clearStatus();
		showStatus("Build toolchain image staged.", false);
		closeModal();
		await refreshPkgRecipes();
	} catch (e) {
		showStatus("Failed to bootstrap build image: " + e.message, true);
	}
});

document.getElementById("pkg-install-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("pkgf-name").value.trim();
	const image = document.getElementById("pkgf-image").value.trim();
	const upgrade = document.getElementById("pkgf-upgrade").checked;
	const body = { name: name };

	if (image !== "")
		body.image = image;
	if (upgrade)
		body.upgrade = true;

	try {
		await apiRequest("POST", CIX_API.pkgInstall(), body);
		clearStatus();
		document.getElementById("pkg-install-form").reset();
		closeModal();
		await refreshPkgList();
	} catch (e) {
		showStatus("Failed to start install: " + e.message, true);
	}
});

document.getElementById("pkg-update-all").addEventListener("click", async () => {
	try {
		const result = await apiRequest("POST", CIX_API.pkgUpdateAll());
		clearStatus();
		showStatus(result && result.status ? result.status : "update started", false);
		await refreshPkgList();
		await refreshPkgDrift();
	} catch (e) {
		showStatus("Failed to start update-all: " + e.message, true);
	}
});

/* ---------- System ---------- */

let siteConfigDirty = false;

/* Composes this site's own suggested FQDN for a bare label, or null if
 * no site config has loaded yet OR no site_name is set. Convenience
 * only (ADR-0046) -- never required, never validated against. Gated
 * on site_name specifically (matching the server-side
 * siteconfig_qualify()'s own gate, ADR-0052) -- domain_suffix alone
 * defaults to a real, non-empty value ("internal") on every install,
 * so suggesting a qualified name whenever it's merely present would
 * suggest something the server itself won't actually apply unless a
 * site has genuinely been configured. */
function suggestedFqdn(label) {
	if (!cache.siteConfig || !cache.siteConfig.site_name)
		return null;
	return `${label}.${cache.siteConfig.site_name}.${cache.siteConfig.domain_suffix}`;
}

async function refreshSiteConfig() {
	try {
		const site = await apiRequest("GET", CIX_API.getSystemSite());

		cache.siteConfig = site;
		if (!siteConfigDirty) {
			document.getElementById("sitef-instance-name").value = site.instance_name;
			document.getElementById("sitef-site-name").value = site.site_name;
			document.getElementById("sitef-domain-suffix").value = site.domain_suffix;
		}
		document.getElementById("df-name").placeholder = suggestedFqdn("db") || "db.internal";
		document.getElementById("pf-name").placeholder = suggestedFqdn("svc") || "svc.internal";

		const label = document.getElementById("tree-instance-label");

		/* The host's real name, not just its bare label -- which box
		 * this is includes the site it is in and the domain it answers
		 * under, and those are exactly what tell two instances apart. */
		label.textContent = instanceFqdn(site);
		label.title = instanceFqdn(site);
		label.hidden = false;
		document.title = "Cix — " + instanceFqdn(site);
	} catch (e) {
		/* Best-effort -- the form/header just stay at whatever was last shown. */
	}
}

/* On leaving a bare-label name field (no dot typed -- an FQDN the
 * operator already fully typed is left alone), auto-expand it to this
 * site's suggested FQDN. Still a plain text field afterward -- fully
 * editable, never enforced. */
function qualifyOnBlur(input) {
	input.addEventListener("blur", () => {
		const value = input.value.trim();

		if (value === "" || value.includes("."))
			return;
		const fqdn = suggestedFqdn(value);
		if (fqdn)
			input.value = fqdn;
	});
}

qualifyOnBlur(document.getElementById("df-name"));
qualifyOnBlur(document.getElementById("pf-name"));

document.getElementById("sitef-instance-name").addEventListener("input", () => {
	siteConfigDirty = true;
});
document.getElementById("sitef-site-name").addEventListener("input", () => {
	siteConfigDirty = true;
});
document.getElementById("sitef-domain-suffix").addEventListener("input", () => {
	siteConfigDirty = true;
});

document.getElementById("sys-site-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const body = {
		instance_name: document.getElementById("sitef-instance-name").value.trim(),
		site_name: document.getElementById("sitef-site-name").value.trim(),
		domain_suffix: document.getElementById("sitef-domain-suffix").value.trim(),
	};

	try {
		await apiRequest("PUT", CIX_API.putSystemSite(), body);
		clearStatus();
		showStatus("Site config saved", false);
		siteConfigDirty = false;
		await refreshSiteConfig();
	} catch (e) {
		showStatus("Failed to save site config: " + e.message, true);
	}
});

let daemonConfigDirty = false;


async function refreshRoutes() {
	try {
		const r = await apiRequest("GET", CIX_API.getSystemRoutes());

		cache.routes = r.routes;
	} catch (e) {
		/* Best-effort -- cache.routes just stays at whatever was last shown. */
	}
}

/* Renders one route row into tbody, with Remove wired to DELETE
 * /v1/system/routes -- shared by the full table (renderRoutesList())
 * and the per-network filtered sub-table (renderNetworkDetail()). */
function appendRouteRow(tbody, route) {
	const row = document.createElement("tr");
	const dest = document.createElement("td");
	const gw = document.createElement("td");
	const iface = document.createElement("td");
	const actionCell = document.createElement("td");
	const rmButton = document.createElement("button");

	dest.textContent = route.dest === "default" ? "default" : route.dest + "/" + route.prefix;
	gw.textContent = route.gateway !== null ? route.gateway : "-";
	iface.textContent = route.interface !== null ? route.interface : "-";
	row.appendChild(dest);
	row.appendChild(gw);
	row.appendChild(iface);

	rmButton.textContent = "Remove";
	rmButton.className = "button-danger";
	rmButton.addEventListener("click", () => removeRoute(route));
	actionCell.appendChild(rmButton);
	row.appendChild(actionCell);

	tbody.appendChild(row);
}

async function removeRoute(route) {
	const body = route.dest === "default" ? {} : { dest: route.dest, prefix: route.prefix };

	try {
		await apiRequest("DELETE", CIX_API.deleteSystemRoute(), body);
		clearStatus();
		await refreshRoutes();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to remove route: " + e.message, true);
	}
}

function renderRoutesList() {
	const tbody = document.getElementById("routes-body");

	tbody.textContent = "";
	if (cache.routes.length === 0) {
		tbody.innerHTML = '<tr><td colspan="4" class="empty">No routes.</td></tr>';
		return;
	}
	for (const route of cache.routes)
		appendRouteRow(tbody, route);
}

/* ---------- sysctl (ADR-0160) ---------- */

async function refreshSysctl() {
	try {
		const r = await apiRequest("GET", CIX_API.listSystemSysctl());

		cache.sysctls = r.sysctls;
	} catch (e) {
		/* Best-effort -- cache.sysctls just stays at whatever was last shown. */
	}
}

/* A sysctl's own "value" field is either a bare string or an array
 * (tuple-shaped keys, e.g. net.ipv4.ip_local_port_range) -- both
 * already documented shapes the daemon accepts and returns as-is. */
function formatSysctlValue(value) {
	return Array.isArray(value) ? value.join(" ") : String(value);
}

async function removeSysctl(key) {
	try {
		await apiRequest("DELETE", CIX_API.deleteSystemSysctl(key));
		clearStatus();
		await refreshSysctl();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to remove sysctl: " + e.message, true);
	}
}

function renderSysctlList() {
	const tbody = document.getElementById("sysctl-body");

	tbody.textContent = "";
	if (cache.sysctls.length === 0) {
		tbody.innerHTML = '<tr><td colspan="3" class="empty">No sysctls persisted.</td></tr>';
		return;
	}
	for (const s of cache.sysctls) {
		const row = document.createElement("tr");
		const key = document.createElement("td");
		const value = document.createElement("td");
		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		key.textContent = s.key;
		key.className = "processes-cmdline";
		value.textContent = formatSysctlValue(s.value);
		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeSysctl(s.key));
		actionCell.appendChild(rmButton);
		row.appendChild(key);
		row.appendChild(value);
		row.appendChild(actionCell);
		tbody.appendChild(row);
	}
}

/* Parses the web form's own single "KEY=VALUE,KEY=VALUE" text field
 * into the {key: value} object shape both /kmod POST (load options)
 * and /kmod-config PUT (default_options) expect -- the web analog of
 * cixctl's repeatable --option=KEY=VALUE. Empty input -> {} (no
 * options), matching "omit means fall back to persisted defaults". */
function parseKeyValueList(text) {
	const result = {};
	const trimmed = text.trim();

	if (trimmed === "")
		return result;
	for (const pair of trimmed.split(",")) {
		const eq = pair.indexOf("=");

		if (eq > 0)
			result[pair.slice(0, eq).trim()] = pair.slice(eq + 1).trim();
	}
	return result;
}

/* The action list is a closed registry (ADR-0257) -- populated from the
 * daemon, never typed, because a free-text command field on a
 * shell-less host is a shell-exec endpoint. */
function fillScheduleActions() {
	const sel = document.getElementById("schf-action");

	if (sel === null)
		return;
	sel.textContent = "";
	for (const a of scheduleActions) {
		const opt = document.createElement("option");
		const name = typeof a === "string" ? a : a.name;

		opt.value = name;
		/* The field is `summary`, checked against a live
		 * /schedule-actions rather than guessed. */
		const summary = typeof a === "string" ? "" : a.summary;

		opt.textContent = summary ? name + " — " + summary : name;
		sel.appendChild(opt);
	}
}

function scheduleKindChanged() {
	const kind = document.getElementById("schf-kind").value;

	document.getElementById("schf-every-fields").hidden = kind !== "every";
	document.getElementById("schf-clock-fields").hidden = kind === "every";
	document.getElementById("schf-weekday-label").hidden = kind !== "weekly";
}

document.getElementById("schf-kind").addEventListener("change", scheduleKindChanged);

document.getElementById("schedule-add").addEventListener("click", async () => {
	await refreshScheduleActions();
	fillScheduleActions();
	scheduleKindChanged();
	openModal("schedule-form", "Add schedule");
});

document.getElementById("schedule-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("schf-name").value.trim();
	const kind = document.getElementById("schf-kind").value;
	const at = document.getElementById("schf-at").value;
	let spec;

	if (name === "")
		return;
	/* Exactly one of every/daily/weekly -- the daemon refuses two
	 * rather than resolving by precedence, so the form sends one. */
	if (kind === "every") {
		spec = { every: {
			days: Number(document.getElementById("schf-days").value) || 0,
			hours: Number(document.getElementById("schf-hours").value) || 0,
			minutes: Number(document.getElementById("schf-minutes").value) || 0,
		} };
	} else if (kind === "daily") {
		spec = { daily: { at: at } };
	} else {
		/* The field is `on`, a three-letter day name -- not a weekday
		 * number. Checked against ScheduleSpec rather than guessed. */
		spec = { weekly: {
			at: at,
			on: document.getElementById("schf-weekday").value,
		} };
	}
	try {
		await apiRequest("PUT", CIX_API.setSchedule(name), {
			action: document.getElementById("schf-action").value,
			schedule: spec,
			catch_up: document.getElementById("schf-catchup").checked,
			enabled: true,
		});
		clearStatus();
		document.getElementById("schedule-form").reset();
		closeModal();
		await refreshSchedules();
	} catch (e) {
		showStatus("Failed to create schedule: " + e.message, true);
	}
});

document.getElementById("sysctl-set-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const key = document.getElementById("sf-key").value.trim();
	const rawValues = document.getElementById("sf-value").value.split(",").map((v) => v.trim()).filter((v) => v !== "");
	const persist = document.getElementById("sf-persist").checked;

	if (key === "" || rawValues.length === 0)
		return;
	try {
		const body = { value: rawValues.length === 1 ? rawValues[0] : rawValues };

		if (!persist)
			body.persist = false;
		await apiRequest("PUT", CIX_API.putSystemSysctl(key), body);
		clearStatus();
		document.getElementById("sysctl-set-form").reset();
		document.getElementById("sf-persist").checked = true;
		closeModal();
		await refreshSysctl();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to set sysctl: " + e.message, true);
	}
});

/* ---------- kernel modules (ADR-0159 Phase A) ---------- */

async function refreshKmod() {
	try {
		const r = await apiRequest("GET", CIX_API.listKmod());

		cache.kmodModules = r.modules;
	} catch (e) {
		/* Best-effort -- cache.kmodModules just stays at whatever was last shown. */
	}
}

async function refreshKmodConfig() {
	try {
		const r = await apiRequest("GET", CIX_API.listKmodConfig());

		cache.kmodConfig = r.kmod_config;
	} catch (e) {
		/* Best-effort -- cache.kmodConfig just stays at whatever was last shown. */
	}
}

async function unloadKmod(name) {
	try {
		await apiRequest("DELETE", CIX_API.deleteKmod(name));
		clearStatus();
		await refreshKmod();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to unload " + name + ": " + e.message, true);
	}
}

async function removeKmodConfig(name) {
	try {
		await apiRequest("DELETE", CIX_API.deleteKmodConfig(name));
		clearStatus();
		await refreshKmodConfig();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to remove kmod config: " + e.message, true);
	}
}

function renderKmodList() {
	const tbody = document.getElementById("kmod-body");

	tbody.textContent = "";
	if (cache.kmodModules.length === 0) {
		tbody.innerHTML = '<tr><td colspan="5" class="empty">No modules loaded.</td></tr>';
	} else {
		for (const m of cache.kmodModules) {
			const row = document.createElement("tr");
			const name = document.createElement("td");
			const size = document.createElement("td");
			const usedBy = document.createElement("td");
			const state = document.createElement("td");
			const actionCell = document.createElement("td");
			const unloadButton = document.createElement("button");

			name.textContent = m.name;
			size.textContent = m.size;
			usedBy.textContent = m.used_by_count + (m.used_by && m.used_by.length > 0 ? " [" + m.used_by.join(",") + "]" : "");
			state.textContent = m.state;
			unloadButton.textContent = "Unload";
			unloadButton.className = "button-danger";
			unloadButton.addEventListener("click", () => unloadKmod(m.name));
			actionCell.appendChild(unloadButton);
			row.appendChild(name);
			row.appendChild(size);
			row.appendChild(usedBy);
			row.appendChild(state);
			row.appendChild(actionCell);
			tbody.appendChild(row);
		}
	}

	const configBody = document.getElementById("kmodconfig-body");

	configBody.textContent = "";
	if (cache.kmodConfig.length === 0) {
		configBody.innerHTML = '<tr><td colspan="4" class="empty">No kmod config persisted.</td></tr>';
		return;
	}
	for (const c of cache.kmodConfig) {
		const row = document.createElement("tr");
		const name = document.createElement("td");
		const options = document.createElement("td");
		const autoload = document.createElement("td");
		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		name.textContent = c.name;
		options.textContent = c.default_options && Object.keys(c.default_options).length > 0
			? Object.entries(c.default_options).map(([k, v]) => k + "=" + v).join(" ")
			: "(none)";
		autoload.textContent = c.autoload ? "yes" : "no";
		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeKmodConfig(c.name));
		actionCell.appendChild(rmButton);
		row.appendChild(name);
		row.appendChild(options);
		row.appendChild(autoload);
		row.appendChild(actionCell);
		configBody.appendChild(row);
	}
}

document.getElementById("kmod-load-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("klf-name").value.trim();

	if (name === "")
		return;
	try {
		const options = parseKeyValueList(document.getElementById("klf-options").value);

		await apiRequest("POST", CIX_API.postKmodLoad(name), { options: options });
		clearStatus();
		document.getElementById("kmod-load-form").reset();
		closeModal();
		await refreshKmod();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to load " + name + ": " + e.message, true);
	}
});

document.getElementById("kmcf-set-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("kmcf-name").value.trim();

	if (name === "")
		return;
	try {
		const body = {
			default_options: parseKeyValueList(document.getElementById("kmcf-options").value),
			autoload: document.getElementById("kmcf-autoload").checked,
		};

		await apiRequest("PUT", CIX_API.putKmodConfig(name), body);
		clearStatus();
		document.getElementById("kmcf-set-form").reset();
		closeModal();
		await refreshKmodConfig();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to configure " + name + ": " + e.message, true);
	}
});

document.getElementById("kmod-build-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const buildImage = document.getElementById("kbf-build-image").value.trim();
	const version = document.getElementById("kbf-version").value.trim();
	const symbolsText = document.getElementById("kbf-symbols").value.trim();
	const upgrade = document.getElementById("kbf-upgrade").checked;
	const statusEl = document.getElementById("kbf-status");

	if (buildImage === "")
		return;
	try {
		const body = { build_image: buildImage };

		if (version !== "")
			body.version = version;
		if (symbolsText !== "")
			body.symbols = symbolsText.split(",").map((s) => s.trim()).filter((s) => s !== "");
		if (upgrade)
			body.upgrade = true;
		await apiRequest("POST", CIX_API.postKmodBuild(), body);
		clearStatus();
		statusEl.textContent = "Build started -- see the log panel below for progress.";
	} catch (e) {
		showStatus("Failed to start kernel build: " + e.message, true);
	}
});

/* The log browsing table/filter form moved to the always-visible
 * bottom log panel (ADR-0129) -- this page now only shows/edits the
 * server-side store's own size cap. */
async function refreshLogsConfig() {
	try {
		const cfg = await apiRequest("GET", CIX_API.getSystemLogsConfig());

		document.getElementById("lcf-max-bytes").value = cfg.max_bytes;
	} catch (e) {
		/* Best-effort -- the field just stays at whatever was last shown. */
	}
}

function renderLogsList() {
	refreshLogsConfig();
}

/*
 * Issue #77 surface parity: GET /v1/system/kmsg (the kernel ring buffer)
 * had no web surface at all. Rendered as plain text rather than a table --
 * kmsg is a linear stream of kernel messages, and an operator reading it is
 * scanning for a failure in sequence, exactly as with dmesg.
 */
const KMSG_PRIORITY_NAMES = ["emerg", "alert", "crit", "err", "warn", "notice", "info", "debug"];

async function refreshKmsg() {
	const out = document.getElementById("kmsg-output");
	const tailRaw = document.getElementById("kmsg-tail").value;
	const tail = parseInt(tailRaw, 10);
	const path = Number.isFinite(tail) && tail > 0 ? CIX_API.getSystemKmsg() + "?tail=" + tail : CIX_API.getSystemKmsg();

	out.textContent = "(loading)";
	try {
		const data = await apiRequest("GET", path);
		const entries = data.entries || [];

		if (entries.length === 0) {
			out.textContent = "(no kernel log entries)";
			return;
		}
		out.textContent = entries
			.map((e) => {
				/* ts_usec is the kernel's own monotonic time since boot, not a
				   wall clock -- shown as seconds, the way dmesg itself does. */
				const secs = (Number(e.ts_usec) / 1000000).toFixed(6).padStart(14);
				const prio = KMSG_PRIORITY_NAMES[e.priority] || String(e.priority);
				return "[" + secs + "] " + prio.padEnd(6) + " " + (e.message || "");
			})
			.join("\n");
	} catch (err) {
		out.textContent = "(failed to read the kernel log: " + err.message + ")";
	}
}

/*
 * Issue #81: registered-server health. One table for all four kinds
 * (ldap/dns/ntp/syslog) because it is one mechanism, not four.
 */
/*
 * Issue #88: persistent volumes. The delete button is marked danger and
 * confirmed, because deleting a volume destroys real data that outlived
 * every container that ever used it -- the one genuinely irreversible
 * operation on this page.
 */
/* Just the fetch, for the poll loop and the tree -- refreshVolumes()
 * also renders the Volumes page, which is wasted work when that page
 * is not the one being viewed. */
/* Shared by the tree's right-click menu and the Volumes page, so the
 * confirmation and the refresh behave identically wherever a volume is
 * deleted from. */
async function deleteVolumeByName(name) {
	if (!confirm('Delete volume "' + name + '"? This permanently destroys its data.'))
		return;
	try {
		await apiRequest("DELETE", CIX_API.deleteVolume(name));
		clearStatus();
		await refreshVolumeCache();
		renderTree();
		if (onPageOf("volumes"))
			await refreshVolumes();
	} catch (e) {
		showStatus("Failed to delete volume: " + e.message, true);
	}
}

/*
 * Issue #97: declared against installed. Computed by the daemon, not
 * joined here -- two clients each implementing this join across four
 * endpoints is how they drift from each other.
 */
async function refreshSoftwareReconcile() {
	const body = document.getElementById("sw-body");
	const summary = document.getElementById("sw-summary");
	let list;

	try {
		const data = await apiRequest("GET", CIX_API.listSoftware());

		list = data.software || [];
	} catch (e) {
		summary.textContent = "Could not read: " + e.message;
		return;
	}
	body.textContent = "";
	let drift = 0;

	for (const item of list) {
		const row = document.createElement("tr");
		const recipeCell = document.createElement("td");
		const instCell = document.createElement("td");

		for (const text of [item.name, item.kind]) {
			const td = document.createElement("td");

			td.textContent = text;
			row.appendChild(td);
		}
		if (item.declared) {
			recipeCell.textContent = "yes";
		} else {
			/* The one state that is a problem rather than information:
			 * flagged, not just left blank. */
			const badge = document.createElement("span");

			badge.className = statusBadge("error");
			badge.textContent = "none";
			badge.title = "Cannot be rebuilt from source control";
			recipeCell.appendChild(badge);
			drift++;
		}
		instCell.textContent = item.installed ? "yes" : "no";
		row.appendChild(recipeCell);
		row.appendChild(instCell);
		body.appendChild(row);
	}
	if (list.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 4;
		cell.className = "empty";
		cell.textContent = "Nothing to reconcile";
		row.appendChild(cell);
		body.appendChild(row);
	}
	summary.textContent =
		drift === 0
			? "Everything installed has a recipe."
			: drift + " item(s) installed with no recipe — either capture one, or they are debris.";
}

/*
 * Issue #63. The confirmation is this install's own name typed back --
 * the daemon requires it regardless, and pre-filling it here would turn
 * a guard into a formality.
 */
document.getElementById("factory-reset-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	const typed = document.getElementById("fr-confirm").value.trim();

	if (
		!confirm(
			"Factory reset " +
				typed +
				"?\n\nThis destroys every container, image, network and VOLUME (and all data in " +
				"them), then reboots. It cannot be undone."
		)
	)
		return;
	try {
		await apiRequest("POST", CIX_API.factoryReset(), { confirm: typed });
		showStatus("Factory reset armed \u2014 the box is rebooting and will come back empty.", false);
	} catch (e) {
		showStatus("Factory reset refused: " + e.message, true);
	}
});

/* ---- Issue #96: volume content snapshots ---- */

async function refreshVolumeBackupConfig() {
	const select = document.getElementById("vbc-disk");

	try {
		const cfg = await apiRequest("GET", CIX_API.getVolumeBackupConfig());

		/* Rebuilding this select on every poll discarded whatever the
		 * operator had picked while they were still picking. */
		if (!dataChanged("volume-backup-config", { cfg: cfg, disks: cache.storage }))
			return;
		select.textContent = "";
		const none = document.createElement("option");

		none.value = "";
		none.textContent = "(none)";
		select.appendChild(none);
		/* Only disks carrying the backup role: offering any other would
		 * be offering a choice the daemon is going to refuse. */
		for (const d of cache.storage) {
			const role = storageRoleFor(d.name);

			if (!role || role.role !== "backup")
				continue;
			const opt = document.createElement("option");

			opt.value = d.name;
			opt.textContent = d.name + (d.mounted ? " (" + d.mount_path + ")" : " — not mounted");
			select.appendChild(opt);
		}
		select.value = cfg.disk || "";
		document.getElementById("vbc-enabled").checked = !!cfg.enabled;
	} catch (e) {
		showStatus("Failed to read the volume backup config: " + e.message, true);
	}
}

document.getElementById("vbc-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	try {
		await apiRequest("PUT", CIX_API.setVolumeBackupConfig(), {
			disk: document.getElementById("vbc-disk").value,
			enabled: document.getElementById("vbc-enabled").checked,
		});
		clearStatus();
		await refreshVolumeBackupConfig();
	} catch (e) {
		showStatus("Failed to save: " + e.message, true);
	}
});

/*
 * One sentence saying where a volume's backups stand, composed in one
 * place. Both the volume's own page and the mounting container's
 * volumes table say it, and two spellings of one fact would drift.
 */
function volumeBackupSummaryText(data) {
	const n = (data.snapshots || []).length;

	if (!data.enabled)
		return "not backed up";
	if (n === 0)
		return "on, no snapshots yet";
	return n + " snapshot" + (n === 1 ? "" : "s") + ", newest " + data.snapshots[0].stamp;
}

/*
 * name -> { text, fetchedAt }. The container detail page redraws its
 * whole volumes table on every poll, and re-asking (and re-blanking
 * the cell to a placeholder) each time made that column jump width
 * once per cycle -- a table that visibly resized while you read it.
 * The answer only changes when a snapshot is taken or the policy is
 * edited, so a remembered one is the right one nearly always.
 */
const volumeBackupSummaries = new Map();
const VOLUME_BACKUP_SUMMARY_MAX_AGE_MS = 30000;

/* Writes the summary into a cell, from memory where we have it, and
 * re-asks only once it is old enough to be worth another request. The
 * cell may have been replaced by a later redraw before the answer
 * arrives -- writing to a detached node is harmless, and that redraw
 * has already read the fresher value out of the cache. */
function fillVolumeBackupCell(volumeName, cell) {
	const known = volumeBackupSummaries.get(volumeName);

	cell.textContent = known !== undefined ? known.text : "…";
	if (known !== undefined && Date.now() - known.fetchedAt < VOLUME_BACKUP_SUMMARY_MAX_AGE_MS)
		return;
	apiRequest("GET", CIX_API.getVolumeBackups(volumeName))
		.then((b) => {
			cell.textContent = rememberVolumeBackupSummary(volumeName, b);
		})
		.catch(() => {
			/* Keep the last true thing we were told rather than
			 * replacing it with a dash on one failed poll. */
			if (!volumeBackupSummaries.has(volumeName))
				cell.textContent = "-";
		});
}

/*
 * A volume's real size, filled in after the row is on screen (#365).
 *
 * Measuring is a full tree walk on the daemon side, so it is a separate
 * endpoint rather than a field on the volume list, and this cell asks
 * for it per row instead of the list carrying it. Cached with the same
 * max-age the backup summary above uses, so re-rendering the table
 * while someone is looking at it does not re-walk every volume.
 *
 * The limit comes back in the same response, so "used of limit" needs
 * no second request.
 */
const volumeUsageSummaries = new Map();

function fillVolumeUsageCell(volumeName, cell) {
	const known = volumeUsageSummaries.get(volumeName);

	cell.textContent = known !== undefined ? known.text : "\u2026";
	if (known !== undefined && Date.now() - known.fetchedAt < VOLUME_BACKUP_SUMMARY_MAX_AGE_MS)
		return;
	apiRequest("GET", CIX_API.getVolumeUsage(volumeName))
		.then((u) => {
			const used = formatBytes(u.bytes || 0);
			const text = u.quota_bytes > 0
			                 ? used + " of " + formatBytes(u.quota_bytes)
			                 : used + " (no limit)";

			volumeUsageSummaries.set(volumeName, { text: text, fetchedAt: Date.now() });
			cell.textContent = text;
		})
		.catch(() => {
			/* Keep the last true thing we were told rather than
			 * replacing it with a dash on one failed poll. */
			if (!volumeUsageSummaries.has(volumeName))
				cell.textContent = "-";
		});
}

function rememberVolumeBackupSummary(volumeName, data) {
	const text = volumeBackupSummaryText(data);

	volumeBackupSummaries.set(volumeName, { text: text, fetchedAt: Date.now() });
	return text;
}

/*
 * A volume's own snapshots. Rendered from one response carrying policy,
 * snapshots and last-attempt together -- they are always wanted at once
 * and three calls to draw one panel would be three round trips.
 */
async function renderVolumeBackups(volumeName, fresh) {
    const note = document.getElementById("vd-backup-note");
	const body = document.getElementById("vd-snapshots-body");
	let data;

	try {
		data = await apiRequest("GET", CIX_API.getVolumeBackups(volumeName));
	} catch (e) {
		note.textContent = "Could not read backups: " + e.message;
		return;
	}
	/* This response is the authoritative one, and it is fetched here
	 * whenever a snapshot is taken, deleted or restored -- so the
	 * summary a container's table shows is refreshed from the same
	 * answer rather than going stale behind its own 30s window. */
	rememberVolumeBackupSummary(volumeName, data);
	/* Same reasoning as the quota input: these are controls the
	 * operator edits, so they are filled on arrival and then left
	 * alone rather than rewritten under them on every poll. */
	if (fresh) {
		document.getElementById("vd-backup-enabled").checked = !!data.enabled;
		document.getElementById("vd-backup-retain").value = data.retain;
		document.getElementById("vd-backup-while-running").value = data.while_running || "refuse";
	}

	const failed = data.status && data.status.last_error;

	/*
	 * The state most worth surfacing is the quiet one: enabled, but
	 * mounted by a container that never stops and not allowed to be
	 * copied while running -- which means it is never actually backed
	 * up, and nothing else on the page would say so.
	 */
	const blocked =
		data.enabled &&
		(data.while_running || "refuse") === "refuse" &&
		(cache.containers || []).some(
			(c) =>
				(c.status === "running" || c.status === "paused") &&
				(c.volumes || []).some((m) => m.name === volumeName)
		);

	note.textContent = blocked
		? "Enabled, but never actually running: a container mounting this volume is up, and this volume is set to skip the backup while that is true. Choose \"pause it, copy, then resume\" below, or stop the container for each backup."
		: failed
		  ? "Last attempt failed: " + data.status.last_error
		  : data.enabled
		    ? "Backed up on the shared schedule, keeping the newest " + data.retain + "."
		    : "Not backed up. Opt in below — copying workload data is never assumed.";

	body.textContent = "";
	const snaps = data.snapshots || [];

	if (snaps.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 2;
		cell.className = "empty";
		cell.textContent = "No snapshots yet";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}
	for (const snap of snaps) {
		const row = document.createElement("tr");
		const nameCell = document.createElement("td");
		const actions = document.createElement("td");
		const restore = document.createElement("button");
		const del = document.createElement("button");

		nameCell.textContent = snap.stamp;
		restore.type = "button";
		restore.className = "button-danger button-small";
		restore.textContent = "Restore";
		restore.addEventListener("click", () => restoreVolumeSnapshot(volumeName, snap.stamp));
		del.type = "button";
		del.className = "button-small";
		del.textContent = "Delete";
		del.addEventListener("click", async () => {
			if (!confirm("Delete snapshot " + snap.stamp + "?"))
				return;
			try {
				await apiRequest("DELETE", CIX_API.deleteVolumeSnapshot(volumeName, snap.stamp));
				clearStatus();
				await renderVolumeBackups(volumeName, 1);
			} catch (e) {
				showStatus("Failed to delete snapshot: " + e.message, true);
			}
		});
		actions.appendChild(restore);
		actions.appendChild(del);
		row.appendChild(nameCell);
		row.appendChild(actions);
		body.appendChild(row);
	}
}

/* Restoring replaces everything in the volume, so it asks for the
 * volume's name typed back -- the same bar every other irreversible
 * action in this dashboard sets. */
async function restoreVolumeSnapshot(volumeName, stamp) {
	const typed = prompt(
		"Restoring " +
			stamp +
			" REPLACES everything currently in volume \"" +
			volumeName +
			"\". This cannot be undone.\n\nType the volume name to confirm:",
		""
	);

	if (typed === null)
		return;
	if (typed !== volumeName) {
		showStatus("Name did not match — nothing was restored.", true);
		return;
	}
	try {
		await apiRequest("POST", CIX_API.restoreVolume(volumeName), {
			snapshot: stamp,
			confirm_volume_name: volumeName,
		});
		clearStatus();
		await renderVolumeBackups(volumeName, 1);
	} catch (e) {
		showStatus("Failed to restore: " + e.message, true);
	}
}

document.getElementById("vd-backup-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	const name = currentVolumeDetailName;

	if (name === null)
		return;
	try {
		await apiRequest("PUT", CIX_API.setVolumeBackupPolicy(name), {
			enabled: document.getElementById("vd-backup-enabled").checked,
			retain: parseInt(document.getElementById("vd-backup-retain").value, 10),
			while_running: document.getElementById("vd-backup-while-running").value,
		});
		clearStatus();
		await renderVolumeBackups(name, 1);
	} catch (e) {
		showStatus("Failed to save the backup policy: " + e.message, true);
	}
});

document.getElementById("vd-backup-now").addEventListener("click", async () => {
	const name = currentVolumeDetailName;

	if (name === null)
		return;
	try {
		await apiRequest("POST", CIX_API.takeVolumeBackup(name), {});
		clearStatus();
		await renderVolumeBackups(name, 1);
	} catch (e) {
		showStatus("Failed to back up: " + e.message, true);
	}
});

/* ---- One volume's page ---- */

let currentVolumeDetailName = null;

/*
 * Where a volume's data lives, who mounts it, and how to move it.
 * Volumes were placed at create time and could never move afterwards,
 * which for storage that deliberately outlives its containers is the
 * wrong lifetime to fix at one moment.
 */
async function renderVolumeDetail(name) {
	const fields = document.getElementById("vd-fields");
	const note = document.getElementById("vd-migrate-note");
	const select = document.getElementById("vd-target-disk");

	/* True only for the first render after arriving here -- see the
	 * quota input below for why that distinction matters. */
	const volumeDetailFresh = currentVolumeDetailName !== name;

	currentVolumeDetailName = name;
	await refreshVolumeCache();

	const v = (cache.volumes || []).find((x) => x.name === name);

	if (!v) {
		fields.textContent = name + " \u2014 not found.";
		return;
	}

	const holder = deviceHoldingVolume(v);
	const users = (cache.containers || []).filter((c) =>
		(c.volumes || []).some((m) => m.name === v.name)
	);
	const running = users.filter((c) => c.status === "running" || c.status === "paused");

	fields.textContent = "";
	fields.appendChild(fieldBlock("Placement", v.disk || "default OS-disk placement"));
	fields.appendChild(fieldBlock("Held on", holder ? holder.name : "unknown"));
	fields.appendChild(fieldBlock("Host path", v.host_path || "-"));
	fields.appendChild(
		fieldBlock(
			"Mounted by",
			users.length === 0
				? "not currently mounted"
				: users.map((c) => c.name + " at " + (c.volumes.find((m) => m.name === v.name) || {}).path).join(", ")
		)
	);
	fields.appendChild(
		fieldBlock("Created", v.created_at ? new Date(v.created_at * 1000).toLocaleString() : "-")
	);
	fields.appendChild(
		fieldBlock("Owner",
		           v.owner_uid !== null && v.owner_uid !== undefined
		               ? v.owner_uid + ":" + v.owner_gid
		               : "root (0:0)")
	);

	/* Issue #102: only refilled on arrival, so typing here is not
	 * overwritten by the poll -- the same reasoning the quota input
	 * below already documents. */
	if (volumeDetailFresh) {
		document.getElementById("vd-owner-uid").value =
			v.owner_uid !== null && v.owner_uid !== undefined ? v.owner_uid : "";
		document.getElementById("vd-owner-gid").value =
			v.owner_gid !== null && v.owner_gid !== undefined ? v.owner_gid : "";
		document.getElementById("vd-owner-recursive").checked = false;
	}
	document.getElementById("vd-owner-note").textContent =
		v.owner_uid !== null && v.owner_uid !== undefined
			? "Owned by " + v.owner_uid + ":" + v.owner_gid + "."
			: "Owned by root — a workload running as anyone else cannot write to it.";

	/* Only devices that could actually hold it: mounted, and not the
	 * one it is already on. Offering anything else would be offering a
	 * choice the daemon is going to refuse. */
	select.textContent = "";
	const def = document.createElement("option");

	def.value = "";
	def.textContent = "(default OS-disk placement)";
	select.appendChild(def);
	for (const d of cache.storage) {
		if (!d.mounted || d.protected)
			continue;
		const opt = document.createElement("option");

		opt.value = d.name;
		opt.textContent = d.name + " (" + d.mount_path + ")";
		select.appendChild(opt);
	}
	select.value = v.disk || "";

	{
		const q = document.getElementById("vd-quota");
		const qnote = document.getElementById("vd-quota-note");

		/*
		 * Only refill the input when this page was just navigated to.
		 * renderVolumeDetail() also runs on every poll, so writing to it
		 * unconditionally would overwrite whatever the operator was
		 * halfway through typing, roughly every two seconds. The note
		 * below it is display-only and always refreshed.
		 */
		if (volumeDetailFresh)
			q.value = v.quota_bytes > 0 ? Math.round(v.quota_bytes / (1024 * 1024 * 1024)) : 0;
		qnote.textContent =
			v.quota_bytes > 0
				? "Limited to " + formatBytes(v.quota_bytes) + "."
				: "No limit — this volume can grow until its disk is full.";
	}

	renderVolumeBackups(v.name, volumeDetailFresh);

	note.textContent =
		running.length > 0
			? "Cannot move it right now: " + running.map((c) => c.name).join(", ") + " is running and has it mounted. Stop it first."
			: select.options.length === 1
			  ? "No other mounted disk is available to move it to. Give a disk or partition a role and format it first."
			  : "";
	document.querySelector("#vd-migrate-form button").disabled = running.length > 0;
}

document.getElementById("vd-quota-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	const name = currentVolumeDetailName;

	if (name === null)
		return;
	const gib = parseInt(document.getElementById("vd-quota").value, 10);

	if (isNaN(gib) || gib < 0) {
		showStatus("That is not a valid size.", true);
		return;
	}
	try {
		await apiRequest("PUT", CIX_API.setVolumeQuota(name), {
			quota_bytes: gib * 1024 * 1024 * 1024,
		});
		clearStatus();
		await renderVolumeDetail(name);
	} catch (e) {
		showStatus("Failed to set the limit: " + e.message, true);
	}
});

document.getElementById("vd-migrate-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	const name = currentVolumeDetailName;

	if (name === null)
		return;
	const target = document.getElementById("vd-target-disk").value;

	if (!confirm('Move volume "' + name + '" to ' + (target || "the default OS-disk placement") + "?"))
		return;
	try {
		await apiRequest("POST", CIX_API.migrateVolume(name), {
			disk: target,
		});
		clearStatus();
		await refreshDisks();
		await renderVolumeDetail(name);
		renderTree();
	} catch (e) {
		showStatus("Failed to migrate volume: " + e.message, true);
	}
});

async function refreshVolumeCache() {
	try {
		const data = await apiRequest("GET", CIX_API.listVolumes());

		cache.volumes = data.volumes || [];
	} catch (e) {
		/* A failed poll must never blank the tree -- keep the last
		 * known list, exactly as every other cached resource here does. */
	}
}

async function refreshVolumes() {
	const body = document.getElementById("volumes-body");
	let vols = [];
	let containers = [];
	try {
		const data = await apiRequest("GET", CIX_API.listVolumes());
		vols = data.volumes || [];
		cache.volumes = vols;
		/*
		 * The reverse mapping -- which containers mount each volume --
		 * is what actually explains the ownership model, and there is
		 * no dedicated field for it because a volume genuinely does not
		 * know: containers reference volumes, never the other way
		 * round. Cross-referenced client-side against the container
		 * list, the same approach the network detail page already uses
		 * for "containers on this network" (ADR-0138).
		 */
		const cdata = await apiRequest("GET", CIX_API.listContainers());
		containers = cdata.containers || [];
	} catch (e) {
		showStatus("Failed to read volumes: " + e.message, true);
		return;
	}
	const mountedBy = {};
	for (const c of containers) {
		for (const v of c.volumes || []) {
			if (!mountedBy[v.name])
				mountedBy[v.name] = [];
			mountedBy[v.name].push(c.name + " at " + v.path + (v.read_only ? " (ro)" : ""));
		}
	}

	if (!dataChanged("volumes", { vols: vols, mountedBy: mountedBy }))
		return;
	body.textContent = "";
	if (vols.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 7;
		cell.className = "empty";
		cell.textContent = "No volumes";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}
	for (const v of vols) {
		const row = document.createElement("tr");
		const created = v.created_at ? new Date(v.created_at * 1000).toLocaleString() : "-";

		const users = mountedBy[v.name] || [];

		{
			const td = document.createElement("td");

			td.appendChild(treeLink("#volumes/" + encodeURIComponent(v.name), v.name, ""));
			row.appendChild(td);
		}
		{
			/* Filled after the row is on screen: measuring is a tree
			 * walk, so it is asked for per row rather than carried by
			 * the volume list every render. */
			const td = document.createElement("td");

			fillVolumeUsageCell(v.name, td);
			row.appendChild(td);
		}
		for (const text of [
			/* "unused" is deliberately not an error state -- a volume
			 * outliving every container that used it is the whole
			 * point, and is exactly when its data is most at risk of
			 * being deleted by someone who assumes it is dead weight. */
			users.length > 0 ? users.join(", ") : "not currently mounted",
			v.disk || "(default)",
			v.host_path || "-",
			created,
		]) {
			const td = document.createElement("td");
			td.textContent = text;
			row.appendChild(td);
		}
		const actions = document.createElement("td");
		const del = document.createElement("button");

		del.type = "button";
		del.className = "danger";
		del.textContent = "Delete";
		del.addEventListener("click", async () => {
			if (!confirm("Delete volume \"" + v.name + "\"? This permanently destroys its data."))
				return;
			try {
				await apiRequest("DELETE", CIX_API.deleteVolume(v.name));
				clearStatus();
				await refreshVolumes();
			} catch (e) {
				showStatus("Failed to delete volume: " + e.message, true);
			}
		});
		actions.appendChild(del);
		row.appendChild(actions);
		body.appendChild(row);
	}
}


/* ---------- volume ownership (issue #102) ---------- */

async function applyVolumeOwner(clear) {
	const name = currentVolumeDetailName;

	if (!name)
		return;
	const uid = document.getElementById("vd-owner-uid").value.trim();
	const gid = document.getElementById("vd-owner-gid").value.trim();

	if (!clear && (uid === "" || gid === "")) {
		showStatus("Give both a uid and a gid, or hand it back to root.", true);
		return;
	}
	try {
		await apiRequest("PUT", CIX_API.setVolumeOwner(name), {
			uid: clear ? null : parseInt(uid, 10),
			gid: clear ? null : parseInt(gid, 10),
			recursive: document.getElementById("vd-owner-recursive").checked,
		});
		showStatus(clear ? name + " handed back to root." : name + " now belongs to " + uid + ":" + gid + ".",
		           false);
		currentVolumeDetailName = null; /* force a fresh render of the inputs */
		await renderVolumeDetail(name);
	} catch (e) {
		showStatus("Could not change the owner: " + e.message, true);
	}
}

document.getElementById("vd-owner-form").addEventListener("submit", (event) => {
	event.preventDefault();
	applyVolumeOwner(false);
});
document.getElementById("vd-owner-root").addEventListener("click", () => applyVolumeOwner(true));

document.getElementById("volume-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	const body = { name: document.getElementById("vf-name").value.trim() };
	const disk = document.getElementById("vf-disk").value.trim();

	if (disk !== "")
		body.disk = disk;
	{
		/* Issue #102: both or neither -- a volume owned by one user and
		 * an unrelated group is almost always a typo, and the daemon
		 * refuses it, so say so here rather than sending it. */
		const uid = document.getElementById("vf-owner-uid").value.trim();
		const gid = document.getElementById("vf-owner-gid").value.trim();

		if ((uid === "") !== (gid === "")) {
			showStatus("Give both an owner uid and gid, or neither.", true);
			return;
		}
		if (uid !== "") {
			body.owner_uid = parseInt(uid, 10);
			body.owner_gid = parseInt(gid, 10);
		}
	}
	try {
		await apiRequest("POST", CIX_API.createVolume(), body);
		clearStatus();
		document.getElementById("volume-form").reset();
		closeModal();
		await refreshVolumes();
	} catch (e) {
		showStatus("Failed to create volume: " + e.message, true);
	}
});

async function refreshServerHealth() {
	const body = document.getElementById("server-health-body");
	let servers = [];
	let warnings = [];

	try {
		const data = await apiRequest("GET", CIX_API.getServerHealth());
		servers = data.servers || [];
		warnings = data.warnings || [];
	} catch (e) {
		showStatus("Failed to read server health: " + e.message, true);
		return;
	}

	/* Identical every two seconds is the normal case for a health
	 * table, and rebuilding it then is pure churn. */
	if (!dataChanged("server-health", { servers: servers, warnings: warnings }))
		return;
	body.textContent = "";

	/* Issue #83: a warning here means Cix is managing state with nowhere
	   to deliver it -- a silent, total failure of that subsystem, not a
	   degradation. Shown above the table because it is more urgent than
	   any individual server's state. */
	const warnBox = document.getElementById("server-health-warnings");
	warnBox.textContent = "";
	warnBox.hidden = warnings.length === 0;
	for (const wmsg of warnings) {
		const p = document.createElement("p");
		p.className = "warning";
		p.textContent = wmsg;
		warnBox.appendChild(p);
	}
	if (servers.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 8;
		cell.className = "empty";
		cell.textContent = "No registered servers are being health-tracked yet";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}
	for (const s of servers) {
		const row = document.createElement("tr");
		const cells = [
			s.kind,
			s.container,
			s.state,
			s.in_service ? "yes" : "no",
			s.probe || "-",
			String(s.consecutive_failures || 0),
			s.last_error || "",
		];

		for (const text of cells) {
			const td = document.createElement("td");
			td.textContent = text;
			row.appendChild(td);
		}

		/* Drain/undrain -- the operator override, distinct from what the
		   probe says. Labelled by the action it performs, not the state
		   it is in, so the button never reads as a status. */
		const actionCell = document.createElement("td");
		const btn = document.createElement("button");

		btn.type = "button";
		btn.textContent = s.drained ? "Undrain" : "Drain";
		btn.addEventListener("click", async () => {
			try {
				await apiRequest("PUT", CIX_API.setServerHealthDrain(s.kind, s.container),
					{ drained: !s.drained }
				);
				clearStatus();
				await refreshServerHealth();
			} catch (e) {
				showStatus("Failed to change drain state: " + e.message, true);
			}
		});
		actionCell.appendChild(btn);
		row.appendChild(actionCell);
		body.appendChild(row);
	}
}

document.getElementById("kmsg-form").addEventListener("submit", (event) => {
	event.preventDefault();
	refreshKmsg();
});

document.getElementById("logs-config-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	try {
		await apiRequest("PUT", CIX_API.putSystemLogsConfig(), {
			max_bytes: parseInt(document.getElementById("lcf-max-bytes").value, 10),
		});
		clearStatus();
		showStatus("Log size cap saved", false);
	} catch (e) {
		showStatus("Failed to save log size cap: " + e.message, true);
	}
});

async function refreshDaemonConfig() {
	try {
		const dc = await apiRequest("GET", CIX_API.getDaemonConfig());

		cache.daemonConfig = dc;
		if (!daemonConfigDirty) {
			document.getElementById("dcf-port").value = dc.port;
			document.getElementById("dcf-https-port").value = dc.https_port;
			document.getElementById("dcf-http-enabled").checked = dc.http_enabled;
			document.getElementById("dcf-https-enabled").checked = dc.https_enabled;
		}
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
	await refreshManagementAddress();
}

/* The single off-box address cixd answers on (ADR-0287). Its own
 * resource, GET/PUT/DELETE /system/management-address; the network is
 * derived from the address, never chosen here. */
async function refreshManagementAddress() {
	const status = document.getElementById("mgmt-addr-status");

	try {
		const ma = await apiRequest("GET", CIX_API.getManagementAddress());

		cache.managementAddress = ma;
		if (!ma.configured) {
			status.textContent = "Loopback-only: cixd answers on 127.0.0.1 and nothing off-box.";
			document.getElementById("maf-address").value = "";
		} else if (ma.bind_unavailable) {
			status.textContent = "Configured " + ma.address + " (network " + ma.network +
				") but NOT BOUND -- " + ma.bind_unavailable + " is not on this host; 127.0.0.1 only.";
			document.getElementById("maf-address").value = ma.address;
		} else {
			status.textContent = "Bound on " + ma.bound + " (network " + ma.network +
				"). 127.0.0.1 is always bound too.";
			document.getElementById("maf-address").value = ma.address;
		}
	} catch (e) {
		status.textContent = "Could not read the management address.";
	}
}

for (const id of ["dcf-port", "dcf-https-port", "dcf-http-enabled", "dcf-https-enabled"]) {
	document.getElementById(id).addEventListener("input", () => {
		daemonConfigDirty = true;
	});
}

document.getElementById("sys-daemon-config-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const body = {
		port: parseInt(document.getElementById("dcf-port").value, 10),
		https_port: parseInt(document.getElementById("dcf-https-port").value, 10),
		http_enabled: document.getElementById("dcf-http-enabled").checked,
		https_enabled: document.getElementById("dcf-https-enabled").checked,
	};

	/* This dashboard's fetch() calls are relative to the page's own
	 * origin, so changing the plain-HTTP port disconnects this exact
	 * page the moment it takes effect -- confirmed explicitly, same as
	 * the reboot/shutdown guard. (Moving the off-box address is the
	 * management-address form below, which has its own guard.) */
	const cur = cache.daemonConfig;
	const reconnectNeeded = cur && body.port !== cur.port;

	if (reconnectNeeded &&
	    !confirm("This changes the port this dashboard is served on -- this page will lose its " +
	             "connection once it takes effect. You'll need to reload at the new port. Continue?"))
		return;

	try {
		await apiRequest("PUT", CIX_API.putDaemonConfig(), body);
		clearStatus();
		showStatus("Daemon config saved", false);
		daemonConfigDirty = false;
		await refreshDaemonConfig();
	} catch (e) {
		showStatus("Failed to save daemon config: " + e.message, true);
	}
});

document.getElementById("sys-management-address-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	const address = document.getElementById("maf-address").value.trim();

	if (!address) {
		showStatus("Enter an address, or use Reset to go loopback-only", true);
		return;
	}
	/* Moving the off-box address disconnects this page if it is being
	 * viewed over that address -- guard it like reboot/shutdown. */
	if (!confirm("This sets the off-box address cixd answers on. If you are viewing this " +
	             "dashboard over that address, this page will lose its connection and you'll " +
	             "need to reload at the new address (127.0.0.1 always works on the box). Continue?"))
		return;
	try {
		await apiRequest("PUT", CIX_API.putManagementAddress(), { address: address });
		clearStatus();
		showStatus("Management address set", false);
		await refreshManagementAddress();
	} catch (e) {
		showStatus("Failed to set management address: " + e.message, true);
	}
});

document.getElementById("maf-reset").addEventListener("click", async () => {
	if (!confirm("Reset to loopback-only? cixd will stop answering off-box; it stays reachable " +
	             "on 127.0.0.1 (the on-box console). Continue?"))
		return;
	try {
		await apiRequest("DELETE", CIX_API.deleteManagementAddress());
		clearStatus();
		showStatus("Reset to loopback-only", false);
		await refreshManagementAddress();
	} catch (e) {
		showStatus("Failed to reset management address: " + e.message, true);
	}
});

/* ---------- zswap (issue #51) ---------- */

let zswapDirty = false;

function renderZswap(z) {
	const kernelGrid = document.getElementById("zswap-kernel");
	const divergence = document.getElementById("zswap-divergence");
	const select = document.getElementById("zswap-compressor");

	if (!zswapDirty) {
		document.getElementById("zswap-enabled").checked = !!z.enabled;
		document.getElementById("zswap-max-pool").value = z.max_pool_percent;
		/* Only what this kernel actually has. A compressor it was not
		 * built with is one it will refuse, and offering it would be
		 * offering a setting that cannot take. */
		select.textContent = "";
		for (const name of z.available_compressors || []) {
			const opt = document.createElement("option");

			opt.value = name;
			opt.textContent = name;
			select.appendChild(opt);
		}
		select.value = z.compressor;
	}

	kernelGrid.textContent = "";
	if (!z.supported) {
		divergence.hidden = false;
		divergence.textContent = "This kernel has no zswap at all — nothing here can take effect.";
		return;
	}
	kernelGrid.appendChild(fieldBlock("Kernel says",
	                                   (z.kernel.enabled === null
	                                       ? "unknown"
	                                       : z.kernel.enabled ? "enabled" : "disabled") +
	                                       ", " + z.kernel.max_pool_percent + "%, " +
	                                       z.kernel.compressor));
	/*
	 * The only genuinely interesting state: what was asked for and what
	 * the kernel has are different. Silence when they agree — a panel
	 * that always says something teaches people to stop reading it.
	 */
	if (z.kernel.enabled !== null && z.kernel.enabled !== z.enabled) {
		divergence.hidden = false;
		divergence.textContent = "Configured " + (z.enabled ? "enabled" : "disabled") +
			", but the kernel reports " + (z.kernel.enabled ? "enabled" : "disabled") +
			" — the setting did not take.";
	} else {
		divergence.hidden = true;
	}
}

async function refreshZswap() {
	try {
		renderZswap(await apiRequest("GET", CIX_API.getZswap()));
	} catch (e) {
		/* Best-effort, same as every other panel here. */
	}
}

for (const id of ["zswap-enabled", "zswap-max-pool", "zswap-compressor"]) {
	document.getElementById(id).addEventListener("change", () => {
		zswapDirty = true;
	});
}

document.getElementById("zswap-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	try {
		const res = await apiRequest("PUT", CIX_API.setZswap(), {
			enabled: document.getElementById("zswap-enabled").checked,
			max_pool_percent: parseInt(document.getElementById("zswap-max-pool").value, 10),
			compressor: document.getElementById("zswap-compressor").value,
		});

		zswapDirty = false;
		showStatus("zswap saved.", false);
		renderZswap(res);
	} catch (e) {
		showStatus("Failed to save zswap: " + e.message, true);
	}
});

async function refreshSwap() {
	try {
		const s = await apiRequest("GET", CIX_API.getSystemSwap());

		cache.swap = s;
		document.getElementById("swap-status").textContent = s.enabled
			? "Swap: enabled (" + s.size_mb + " MB at " + s.path + ")"
			: "Swap: disabled";
	} catch (e) {
		/* Best-effort -- the status line just stays at whatever was last shown. */
	}
}

document.getElementById("swap-enable-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const sizeMb = parseInt(document.getElementById("swap-size-mb").value, 10);

	try {
		await apiRequest("POST", CIX_API.postSystemSwap(), { size_mb: sizeMb });
		clearStatus();
		showStatus("Swap enabled", false);
		await refreshSwap();
	} catch (e) {
		showStatus("Failed to enable swap: " + e.message, true);
	}
});

document.getElementById("swap-disable").addEventListener("click", async () => {
	try {
		await apiRequest("DELETE", CIX_API.deleteSystemSwap());
		clearStatus();
		showStatus("Swap disabled", false);
		await refreshSwap();
	} catch (e) {
		showStatus("Failed to disable swap: " + e.message, true);
	}
});

document.getElementById("sys-backup").addEventListener("click", async () => {
	try {
		const text = await apiRequestRaw("GET", CIX_API.getSystemBackup());
		const blob = new Blob([text], { type: "application/json" });
		const url = URL.createObjectURL(blob);
		const a = document.createElement("a");

		a.href = url;
		a.download = "cix-backup.json";
		a.click();
		URL.revokeObjectURL(url);
		clearStatus();
	} catch (e) {
		showStatus("Failed to download backup: " + e.message, true);
	}
});

document.getElementById("sys-restore-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const fileInput = document.getElementById("sys-restore-file");

	if (fileInput.files.length === 0)
		return;

	try {
		const raw = await readFileAsText(fileInput.files[0]);

		await apiRequestRaw("POST", CIX_API.postSystemRestore(), raw);
		clearStatus();
		showStatus("Restored. Reboot for it to take effect.", false);
		document.getElementById("sys-restore-form").reset();
	} catch (e) {
		showStatus("Failed to restore: " + e.message, true);
	}
});

/* ---------- Automatic backup snapshots (ADR-0141 Phase 5) ---------- */

async function refreshBackupConfig() {
	cache.backupConfig = await apiRequest("GET", CIX_API.getBackupConfig());
	if (onPageOf("backup"))
		renderBackupConfig();
}

async function refreshBackupStatus() {
	if (!onPageOf("backup"))
		return;
	try {
		cache.backupStatus = await apiRequest("GET", CIX_API.getBackupConfigStatus());
	} catch (e) {
		/* Transient -- next poll tick tries again. */
	}
	if (onPageOf("backup"))
		renderBackupConfig();
}

function renderBackupConfig() {
	const select = document.getElementById("bc-disk");
	const prevValue = select.value;

	select.textContent = "";
	{
		const opt = document.createElement("option");

		opt.value = "";
		opt.textContent = "(none -- automatic snapshots disabled)";
		select.appendChild(opt);
	}
	for (const d of cache.storage) {
		const role = storageRoleFor(d.name);

		if (d.protected || role === null || role.role !== "backup")
			continue;
		const opt = document.createElement("option");

		opt.value = d.name;
		opt.textContent = d.name + (d.model ? " (" + d.model + ")" : "");
		select.appendChild(opt);
	}
	select.value = cache.backupConfig.disk || prevValue;

	document.getElementById("bc-enabled").checked = !!cache.backupConfig.enabled;

	const s = cache.backupStatus;
	const statusP = document.getElementById("bc-status");

	if (!s || s.state === "never") {
		statusP.textContent = "Snapshots: no attempt has run yet";
	} else {
		statusP.textContent =
			"Snapshots: " +
			s.state +
			(s.last_attempt_unixtime
				? " (last attempt " + new Date(s.last_attempt_unixtime * 1000).toLocaleString() + ")"
				: "") +
			(s.state === "failed" && s.error ? " -- " + s.error : "");
	}
}

document.getElementById("bc-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const disk = document.getElementById("bc-disk").value;
	const enabled = document.getElementById("bc-enabled").checked;

	try {
		cache.backupConfig = await apiRequest("PUT", CIX_API.putBackupConfig(), {
			disk: disk === "" ? null : disk,
			enabled: enabled,
		});
		clearStatus();
		renderBackupConfig();
	} catch (e) {
		showStatus("Failed to save backup config: " + e.message, true);
	}
});

document.getElementById("bc-snapshot-now").addEventListener("click", async () => {
	try {
		cache.backupStatus = await apiRequest("POST", CIX_API.postBackupConfigSnapshotNow());
		if (cache.backupStatus.state === "failed")
			showStatus("Snapshot failed: " + cache.backupStatus.error, true);
		else
			showStatus("Snapshot written.", false);
		renderBackupConfig();
	} catch (e) {
		showStatus("Failed to trigger snapshot: " + e.message, true);
	}
});

document.getElementById("sys-update-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const imagePath = document.getElementById("sys-update-image").value.trim();
	const kernelPath = document.getElementById("sys-update-kernel").value.trim();
	const body = {};

	if (imagePath !== "")
		body.image_path = imagePath;
	if (kernelPath !== "")
		body.kernel_path = kernelPath;
	if (Object.keys(body).length === 0) {
		showStatus("Give at least an image path or a kernel path.", true);
		return;
	}

	try {
		const result = await apiRequest("POST", CIX_API.updateSystem(), body);
		clearStatus();
		showStatus("Staged to slot " + result.slot + " (" + result.updated.join(", ") + ").", false);
		document.getElementById("sys-update-form").reset();
	} catch (e) {
		showStatus("Failed to stage update: " + e.message, true);
	}
});

document.getElementById("menu-reboot").addEventListener("click", async () => {
	if (!confirm("Reboot this host now?"))
		return;
	try {
		await apiRequest("POST", CIX_API.rebootSystem());
		clearStatus();
		showStatus("Rebooting.", false);
	} catch (e) {
		showStatus("Failed to reboot: " + e.message, true);
	}
});

document.getElementById("menu-shutdown").addEventListener("click", async () => {
	if (!confirm("Shut down this host now?"))
		return;
	try {
		await apiRequest("POST", CIX_API.shutdownSystem());
		clearStatus();
		showStatus("Shutting down.", false);
	} catch (e) {
		showStatus("Failed to shut down: " + e.message, true);
	}
});

/* ---------- poll loop ---------- */

/*
 * ---------- polling ----------
 *
 * This used to fetch all 47 endpoints every two seconds, whatever the
 * operator was looking at: ~26 requests a second, forever, per open
 * tab. On a two-CPU box that is real, continuous load produced almost
 * entirely for data nobody was reading, and it is the load the status
 * bar's own LEDs made visible rather than caused.
 *
 * Three tiers now, and the fastest one is deliberately the cheapest
 * request there is:
 *
 *   health   -- every 2s. One tiny endpoint. It drives the TX/RX LEDs
 *               and the reachability colour, so the bar stays as live
 *               as it ever was.
 *   core     -- every 5s. Exactly what the tree renders (containers,
 *               networks, disks, volumes), because the tree is on
 *               screen no matter which page is open.
 *   view     -- every 5s. Only what the CURRENT page needs. Since every
 *               tab is its own address (ADR-0185), the route names the
 *               tab precisely, so this is a real per-page set rather
 *               than a per-section guess.
 *   sweep    -- every 30s. Everything, exactly as before. This is the
 *               safety net: if a page's own set below is missing an
 *               endpoint, that page is at most 30 seconds stale rather
 *               than broken, which is the right failure mode for a
 *               mapping maintained by hand.
 *
 * And nothing but health runs while the tab is hidden -- a dashboard
 * left open on another desktop should not cost the box anything.
 */
const CORE_REFRESHERS = [
	refreshContainers,
	refreshNetworks,
	refreshDisks,
	refreshDiskRoles,
	refreshDiskFormatStatuses,
	refreshVolumeCache,
];

/* Route category -> what that page actually reads. Keyed by the same
 * addresses the tree and the tab bars use. */
/* ---------- pipeline (ADR-0256) ---------- */

/*
 * The eleven-stage flow, the host's deploy position, and a worklist of
 * everything that needs someone to do something.
 *
 * Every stage is drawn even at zero. A pipeline rendered only from the
 * occupied stages is a filtered list, and the empty stages are exactly
 * the ones an operator wants to see are empty.
 */
/*
 * #371: the delivery graph.
 *
 * Three lanes -- deployments, images, packages -- laid left to right in
 * dependency order, with the host on its own. A box is a thing at a
 * stage; its colour is the status axis; an arrow is an edge the daemon
 * derived.
 *
 * DRAWN FROM THE DAEMON'S ANSWER, NOT FROM A COPY OF THE MODEL. The
 * lane order and each lane's stages come from data.kinds, so adding a
 * stage server-side changes this picture with no edit here. A second
 * copy of the stage table living in the browser is exactly the drift
 * ADR-0256 was written to end.
 *
 * Only rows that are NOT ok get a box by default: a host with 200 clean
 * packages is a wall of green nobody reads. Edges are still computed
 * over everything, so an unhealthy box always shows what it depends on.
 */
/*
 * #371: one row per thing, stages as columns, filled to where it stands.
 *
 * The previous version drew four lanes of boxes, one lane per KIND, with
 * the stage as a word inside each box -- so the page never drew a
 * pipeline at all, and reading it as "four columns, four pipelines" was
 * the natural and wrong conclusion. A row is a pipeline; reading a line
 * answers "where is this", which is the question the page exists for.
 */

/* Which stages actually apply to a row.
 *
 * One case is derivable today: a package with no pkg_upstream= is pinned
 * on purpose, so `discover` and `resolve` -- the two stages that poll an
 * upstream -- do not apply to it. That is 120 of 122 packages on a real
 * host, so drawing them as "not reached" would paint almost every row as
 * though it had stalled at the first step.
 *
 * Derived here rather than sent by the daemon because it is the only
 * case. If a second appears, this belongs in the payload instead -- one
 * place deciding which stages apply, not two. */
function stageApplies(kind, stage, row) {
	if (kind === "package" && !row.upstream)
		return stage !== "discover" && stage !== "resolve";
	return true;
}

function dgCells(kind, stages, row) {
	const wrap = document.createElement("div");
	const at = stages.indexOf(row.stage);

	wrap.className = "dg-cells";
	stages.forEach((st, i) => {
		const c = document.createElement("div");

		c.className = "dg-cell";
		if (!stageApplies(kind, st, row))
			c.classList.add("dg-cell-na");
		else if (i < at)
			c.classList.add("dg-cell-done");
		else if (i === at)
			c.classList.add("dg-cell-here", "dg-cell-" + (row.status || "ok"));
		else
			c.classList.add("dg-cell-todo");
		c.title = st + (i === at ? " ← " + (row.status || "ok") : "");
		wrap.appendChild(c);
	});
	return wrap;
}

function dgRow(kind, stages, row, opts) {
	const el = document.createElement("div");

	el.className = "dg-row" + (opts && opts.sub ? " dg-row-sub" : "");
	el.tabIndex = 0;

	const name = document.createElement("div");
	name.className = "dg-row-name";
	name.textContent = (opts && opts.label) || row.name;
	el.appendChild(name);

	const ver = document.createElement("div");
	ver.className = "dg-row-ver";
	ver.textContent = row.version || row.resolved_version || "";
	ver.title = ver.textContent;
	el.appendChild(ver);

	el.appendChild(dgCells(kind, stages, row));

	const why = document.createElement("div");
	why.className = "dg-row-why";
	/* The reason belongs on the row, not behind a click: a page that
	 * makes you open something to find out what is wrong has only moved
	 * the problem. */
	why.textContent = row.reason || "";
	el.appendChild(why);

	if (!(opts && opts.sub)) {
		const open = () => openPipelineDrawer(kind, row);
		el.addEventListener("click", open);
		el.addEventListener("keydown", (e) => {
			if (e.key === "Enter" || e.key === " ") {
				e.preventDefault();
				open();
			}
		});
	}
	return el;
}

function dgSection(kind, label, stages, rows, frag) {
	const sec = document.createElement("div");
	sec.className = "dg-section";

	const head = document.createElement("div");
	head.className = "dg-section-head";
	head.textContent = label + " (" + rows.length + ")";
	sec.appendChild(head);

	const hdr = document.createElement("div");
	hdr.className = "dg-row dg-row-header";
	hdr.appendChild(document.createElement("div")).className = "dg-row-name";
	hdr.appendChild(document.createElement("div")).className = "dg-row-ver";
	const hc = document.createElement("div");
	hc.className = "dg-cells";
	for (const st of stages) {
		const c = document.createElement("div");
		c.className = "dg-colhead";
		c.textContent = st;
		/* Ten stages in one row means the header text is truncated; the
		 * full name has to stay reachable somewhere. */
		c.title = st;
		hc.appendChild(c);
	}
	hdr.appendChild(hc);
	hdr.appendChild(document.createElement("div")).className = "dg-row-why";
	sec.appendChild(hdr);

	for (const r of rows) {
		sec.appendChild(dgRow(kind, stages, r, null));
		/*
		 * A package is not in one place -- it is installed into N
		 * images, each with its own position, and the row above is a
		 * fold of them. Showing the images it is really in is what
		 * makes the fold honest rather than a summary that quietly
		 * stands in for eleven different answers.
		 */
		if (kind === "package" && Array.isArray(r.images) && r.images.length > 1)
			for (const im of r.images)
				sec.appendChild(
					dgRow(kind, stages, Object.assign({}, im, { upstream: r.upstream }), {
						sub: true,
						label: "↳ in " + im.image,
					})
				);
	}
	frag.appendChild(sec);
}

function renderDeliveryGraph(data) {
	const root = document.getElementById("delivery-graph");

	if (!root)
		return;
	root.textContent = "";
	if (!data || !Array.isArray(data.kinds)) {
		root.textContent = "This daemon does not report the delivery graph.";
		return;
	}

	const q = (document.getElementById("dg-search") || {}).value || "";
	/*
	 * The control is "View all", and it is OFF by default -- so the
	 * page opens showing only what needs attention, and seeing
	 * everything is the deliberate act. This is a pipeline for
	 * operators: the common question is "what is wrong", and a page
	 * that answers it without being asked is worth more than one
	 * that opens on a full list nobody scrolled.
	 *
	 * Stated positively on purpose. "Problems only", unchecked, made
	 * the box's OFF state the one that showed more -- a negative
	 * checkbox, where reading the label tells you what you get by
	 * ticking it and leaves you to infer the rest.
	 */
	const viewAll = !!(document.getElementById("dg-view-all") || {}).checked;
	const problemsOnly = !viewAll;
	const needle = q.trim().toLowerCase();
	const stagesFor = {};
	for (const k of data.kinds)
		stagesFor[k.kind] = k.stages || [];

	/* A problem is blocked/failed/cancelled. `not-implemented` is NOT one
	 * -- it means the stage does not apply, which is an ordinary healthy
	 * state, and treating it as interesting is what drew 121 of 122
	 * packages as identical grey boxes. */
	const isProblem = (r) =>
		r.status === "blocked" || r.status === "failed" || r.status === "cancelled";

	const sections = [
		["deployment", "Deployments", data.deployments],
		["image", "Images", data.images],
		["package", "Packages", data.packages],
	];
	const frag = document.createDocumentFragment();
	let shown = 0;

	if (data.deploy) {
		const hostRow = Object.assign({ name: data.deploy.entry || "this host" }, data.deploy);
		if (!needle || hostRow.name.toLowerCase().includes(needle))
			if (!problemsOnly || isProblem(hostRow)) {
				dgSection("host", "Host", stagesFor.host || [], [hostRow], frag);
				shown++;
			}
	}
	for (const [kind, label, rowsIn] of sections) {
		let rows = Array.isArray(rowsIn) ? rowsIn : [];

		if (needle)
			rows = rows.filter((r) => r.name.toLowerCase().includes(needle));
		if (problemsOnly)
			rows = rows.filter(isProblem);
		if (rows.length === 0)
			continue;
		/* Problems first, then alphabetical: the page is read top-down
		 * and what needs attention should not be somewhere in the middle
		 * of 122 healthy rows. */
		rows = rows.slice().sort((a, b) => {
			const pa = isProblem(a) ? 0 : 1;
			const pb = isProblem(b) ? 0 : 1;
			return pa !== pb ? pa - pb : a.name.localeCompare(b.name);
		});
		dgSection(kind, label, stagesFor[kind] || [], rows, frag);
		shown += rows.length;
	}

	if (shown === 0) {
		root.textContent = needle
			? "Nothing matches “" + q + "”."
			: problemsOnly
			? "Nothing is blocked, failed or cancelled."
			: "Nothing to show.";
	} else {
		root.appendChild(frag);
	}

	const count = document.getElementById("dg-count");
	if (count) {
		const probs =
			(data.blocked || 0) + (data.failed || 0) + (data.cancelled || 0);
		count.textContent = probs
			? probs + " need attention"
			: "nothing blocked, failed or cancelled";
		count.className = "dg-count" + (probs ? " dg-count-bad" : "");
	}
	renderDrift(data.drift);
}

/*
 * Packages at more than one version across images.
 *
 * Its own panel because the package row above folds its images away, so
 * a package healthy at three different versions in eleven images reads
 * `install/ok` -- true, and silent about the one thing here that an
 * operator can act on.
 */
function renderDrift(drift) {
	const root = document.getElementById("dg-drift");

	if (!root)
		return;
	root.textContent = "";
	if (!Array.isArray(drift) || drift.length === 0) {
		root.textContent = "Every package is at one version everywhere.";
		return;
	}
	for (const d of drift) {
		const box = document.createElement("div");
		box.className = "dg-drift-row";

		const name = document.createElement("div");
		name.className = "dg-drift-name";
		name.textContent = d.name + " — " + d.versions + " versions";
		box.appendChild(name);

		const byVersion = new Map();
		for (const i of d.installs || []) {
			if (!byVersion.has(i.version))
				byVersion.set(i.version, []);
			byVersion.get(i.version).push(i.image);
		}
		for (const [v, images] of byVersion) {
			const line = document.createElement("div");
			line.className = "dg-drift-line";
			const chip = document.createElement("span");
			chip.className = "dg-chip";
			chip.textContent = v;
			line.appendChild(chip);
			line.appendChild(document.createTextNode(" " + images.join(", ")));
			box.appendChild(line);
		}
		root.appendChild(box);
	}
}

/*
 * Detail for one row: why it is where it is, every image it is really
 * installed into, and the actual build log.
 *
 * The log is the point. A red row with no route to WHY is the
 * true-but-useless diagnostic this platform keeps having to fix
 * elsewhere; the logs already exist (GET /pkg/build-logs) and the page
 * simply never asked for them.
 */
async function openPipelineDrawer(kind, row) {
	const drawer = document.getElementById("dg-drawer");
	const title = document.getElementById("dg-drawer-title");
	const body = document.getElementById("dg-drawer-body");

	if (!drawer || !body)
		return;
	drawer.hidden = false;
	title.textContent = kind + " · " + row.name;
	body.textContent = "";

	const line = (k, v) => {
		if (v === undefined || v === null || v === "")
			return;
		const d = document.createElement("div");
		const b = document.createElement("strong");
		b.textContent = k + ": ";
		d.appendChild(b);
		d.appendChild(document.createTextNode(String(v)));
		body.appendChild(d);
	};

	line("stage", row.stage);
	line("status", row.status);
	line("why", row.reason);
	if (row.blocked_on)
		line("blocked on", row.blocked_on.kind + " " + row.blocked_on.name);
	line("upstream", row.upstream);
	line("newest recipe", row.newest_recipe_version);
	line("resolves to", row.resolved_version);

	if (Array.isArray(row.images) && row.images.length) {
		const h = document.createElement("div");
		h.className = "dg-drawer-h";
		h.textContent = "Installed into";
		body.appendChild(h);
		for (const im of row.images)
			line(im.image, im.version + " — " + im.stage + "/" + im.status +
			     (im.reason ? " — " + im.reason : ""));
	}

	/* Actions. Cancel is the one control that already exists as an
	 * endpoint (POST /pkg/cancel); nothing else here acts yet. */
	if (kind === "package" && (row.status === "blocked" || row.status === "failed")) {
		const bar = document.createElement("div");
		const cancel = document.createElement("button");

		bar.className = "dg-drawer-actions";
		cancel.type = "button";
		cancel.className = "btn-small";
		cancel.textContent = "Cancel the in-flight build";
		cancel.addEventListener("click", async () => {
			cancel.disabled = true;
			try {
				await apiRequest("POST", CIX_API.pkgCancel(), {});
				cancel.textContent = "Cancelled";
				refreshPipeline();
			} catch (e) {
				cancel.textContent = "Could not cancel: " + (e && e.message ? e.message : e);
			}
		});
		bar.appendChild(cancel);
		body.appendChild(bar);
	}

	if (kind !== "package")
		return;

	/*
	 * History (ADR-0272). Above the log deliberately: the log is one
	 * run's output and is usually gone, while this is every run and
	 * survives. An operator asking "has this been failing?" is answered
	 * here; the log only answers "why did the last one fail".
	 */
	const hh = document.createElement("div");
	hh.className = "dg-drawer-h";
	hh.textContent = "History";
	body.appendChild(hh);
	const runsBox = document.createElement("div");
	runsBox.className = "dg-runs";
	runsBox.textContent = "loading…";
	body.appendChild(runsBox);

	try {
		const rr = await apiRequest("GET",
			CIX_API.getPipelineRuns() + "?name=" + encodeURIComponent(row.name) + "&limit=20");
		const runs = (rr && rr.runs) || [];

		runsBox.textContent = "";
		if (runs.length === 0) {
			runsBox.textContent = "Nothing has run for this package on this host yet.";
		} else {
			for (const run of runs) {
				const el = document.createElement("div");
				const bad = run.status && run.status !== "ok";
				const when = run.ended_at ? new Date(run.ended_at * 1000).toLocaleString() : "";
				const secs = run.duration_seconds || 0;

				el.className = "dg-run" + (bad ? " dg-run-bad" : "");
				/* The image is named on every line: a run is a
				 * (package, image) pair, and a history that omits the
				 * image reads as one stream of contradictory outcomes. */
				el.textContent = when + " · " + (run.image || "-") + " · " +
					(bad ? run.stage + "/" + run.status : "ok") +
					" · " + secs + "s · " + (run.trigger || "-") +
					(run.error ? " — " + run.error : "");
				runsBox.appendChild(el);
			}
		}
	} catch (e) {
		runsBox.textContent = "Could not read the run history: " + (e && e.message ? e.message : e);
	}

	const h = document.createElement("div");
	h.className = "dg-drawer-h";
	h.textContent = "Build log";
	body.appendChild(h);
	const pre = document.createElement("pre");
	pre.className = "dg-log";
	pre.textContent = "loading…";
	body.appendChild(pre);

	try {
		const list = await apiRequest("GET", CIX_API.listBuildLogs());
		const mine = (list.logs || [])
			.filter((l) => l.file.startsWith(row.name + "-"))
			.sort((x, y) => y.modified_at - x.modified_at);

		if (mine.length === 0) {
			pre.textContent = "No build log for this package on this host.";
			return;
		}
		/* apiRequestRaw(), not apiRequest(): a build log is plain text,
		 * and apiRequest() parses every non-204 body as JSON. Its own
		 * fourth argument is a timeout in ms, not options. */
		const text = await apiRequestRaw("GET", CIX_API.getBuildLog(mine[0].file));
		/* The tail, not the head: a build failure's cause is the last
		 * thing printed, which is the same reason the log store keeps
		 * the tail of captured output. */
		const s = typeof text === "string" ? text : JSON.stringify(text);
		const lines = s.split("\n");
		pre.textContent = lines.slice(Math.max(0, lines.length - 200)).join("\n");
		pre.scrollTop = pre.scrollHeight;
	} catch (e) {
		pre.textContent = "Could not read the build log: " + (e && e.message ? e.message : e);
	}
}

/*
 * The toolbar. Re-renders from the cached payload rather than re-fetching
 * -- filtering is a question about data already on screen, and a network
 * round trip per keystroke would make typing feel broken.
 */
function wirePipelineControls() {
	const search = document.getElementById("dg-search");
	const only = document.getElementById("dg-view-all");
	const close = document.getElementById("dg-drawer-close");
	const rerender = () => {
		if (cache.pipeline)
			renderDeliveryGraph(cache.pipeline);
	};

	if (search && !search.dataset.wired) {
		search.dataset.wired = "1";
		search.addEventListener("input", rerender);
	}
	if (only && !only.dataset.wired) {
		only.dataset.wired = "1";
		only.addEventListener("change", rerender);
	}
	if (close && !close.dataset.wired) {
		close.dataset.wired = "1";
		close.addEventListener("click", () => {
			document.getElementById("dg-drawer").hidden = true;
		});
	}
}

async function refreshPipeline() {
	let data;

	try {
		data = await apiRequest("GET", CIX_API.getPipeline());
	} catch (e) {
		return; /* best-effort, same as every other refresher here */
	}
	cache.pipeline = data;
	wirePipelineControls();
	renderDeliveryGraph(data);

	const flow = document.getElementById("pipeline-flow");
	const stages = data.stages || [];
	let busiest = 0;

	for (const st of stages)
		busiest = Math.max(busiest, st.packages);
	flow.textContent = "";
	for (const st of stages) {
		const cell = document.createElement("div");
		const name = document.createElement("div");
		const count = document.createElement("div");
		const bar = document.createElement("div");
		const fill = document.createElement("div");

		cell.className = "pipeline-stage";
		if (st.packages === 0)
			cell.classList.add("pipeline-stage-empty");
		name.className = "pipeline-stage-name";
		name.textContent = st.stage;
		count.className = "pipeline-stage-count";
		count.textContent = st.packages;
		bar.className = "pipeline-stage-bar";
		fill.className = "pipeline-stage-fill";
		fill.style.width = busiest > 0 ? (100 * st.packages / busiest) + "%" : "0";
		bar.appendChild(fill);
		/* The verb is what a failure here reads as -- worth having on
		 * hover, since the stage name alone does not say what it does. */
		cell.title = st.verb;
		cell.appendChild(name);
		cell.appendChild(count);
		cell.appendChild(bar);
		flow.appendChild(cell);
	}

	const deploy = document.getElementById("pipeline-deploy");

	deploy.textContent = "";
	if (data.deploy) {
		const badge = document.createElement("span");
		const text = document.createElement("span");

		badge.className = statusBadge(pipelineStatusKind(data.deploy.status));
		badge.textContent = "deploy / " + data.deploy.status;
		text.className = "pipeline-deploy-text";
		text.textContent = (data.deploy.entry ? data.deploy.entry + " -- " : "") +
		                   data.deploy.reason;
		deploy.appendChild(badge);
		deploy.appendChild(text);
	}

	const tbody = document.getElementById("pipeline-rows");
	const packages = data.packages || [];
	let shown = 0;

	tbody.textContent = "";
	for (const p of packages) {
		if (p.status === "ok" || p.status === "not-implemented")
			continue;
		shown++;
		const tr = document.createElement("tr");
		const cells = [p.name, p.stage, null, p.resolved_version || "-",
		               p.newest_recipe_version || "-", p.reason];

		for (let i = 0; i < cells.length; i++) {
			const td = document.createElement("td");

			if (i === 2) {
				const badge = document.createElement("span");

				badge.className = statusBadge(pipelineStatusKind(p.status));
				badge.textContent = p.status;
				td.appendChild(badge);
			} else {
				td.textContent = cells[i];
			}
			tr.appendChild(td);
		}
		tbody.appendChild(tr);
	}
	if (shown === 0) {
		const tr = document.createElement("tr");
		const td = document.createElement("td");

		td.colSpan = 6;
		td.className = "hint";
		td.textContent = "Nothing is blocked or failed.";
		tr.appendChild(td);
		tbody.appendChild(tr);
	}
	document.getElementById("pipeline-summary").textContent =
	    data.total + " package(s): " + data.ok + " ok, " + data.blocked + " blocked, " +
	    data.failed + " failed, " + data.cancelled + " cancelled, " + data.not_implemented +
	    " performed by hand";
}

/* ---------- Software > Build > Overview (#407) ---------- */

/*
 * What the build system is doing right now.
 *
 * Deliberately assembled from two existing endpoints rather than a new
 * one. GET /system/pkg-build-config already reports capacity and, since
 * #246, the slots actually in use -- chain_reap_stale() runs first there
 * specifically so an operator does not read "10 of 10 in use" on an idle
 * box. active_job_names is "name@image, name@image", which is exactly
 * the key GET /pkg/{name} takes, so the fan-out needs no new route.
 *
 * The only daemon change #407 needed was PkgEntry.build_container: the
 * container a build is running in existed inside the daemon and was
 * reported only when a FAILED build's container was deliberately kept
 * (kept_build_container, ADR-0175). The container of a build that was
 * working fine -- the one worth looking inside -- had no name in the API.
 */
function boCard(title, value, note) {
	const card = document.createElement("div");
	const h = document.createElement("h3");
	const v = document.createElement("div");

	card.className = "stats-card";
	h.textContent = title;
	v.className = "bo-figure";
	v.textContent = value;
	card.appendChild(h);
	card.appendChild(v);
	if (note) {
		const n = document.createElement("div");

		n.className = "hint bo-note";
		n.textContent = note;
		card.appendChild(n);
	}
	return card;
}

/*
 * cpu_max is the raw cgroup v2 "quota period" pair, in microseconds --
 * "100000 100000". Shown verbatim it was both the widest thing in the
 * capacity strip (~190px at 1.6rem, which is what forced those cards
 * wide enough to wrap) and the least readable: nothing about those two
 * numbers says "one core" until you divide them.
 *
 * So the figure is the quotient and the raw pair moves to the note,
 * where it stays available for anyone reconciling it against the
 * cgroup. "max <period>" is the kernel's own spelling for no limit.
 */
/*
 * A duration a person reads, from a count of seconds (#423). Minutes
 * and seconds up to an hour, then hours and minutes -- a build in its
 * fortieth minute should say so rather than "2413s".
 */
function boDuration(seconds) {
	const n = Math.max(0, Math.floor(Number(seconds) || 0));
	const h = Math.floor(n / 3600);
	const m = Math.floor((n % 3600) / 60);
	const sec = n % 60;

	if (h > 0)
		return h + "h " + String(m).padStart(2, "0") + "m";
	if (m > 0)
		return m + "m " + String(sec).padStart(2, "0") + "s";
	return sec + "s";
}

/*
 * Ticks every elapsed counter on the page once a second (#423).
 *
 * Each counter carries the daemon's own figure and the local clock
 * reading at the moment it was received, and advances by the
 * difference between local readings -- never by differencing a daemon
 * timestamp against this browser's clock, which would turn any skew
 * between the two into a wrong elapsed time from the first frame. Only
 * the ADVANCE is local, and a local clock measures its own elapsed
 * time correctly whatever it is set to.
 *
 * One interval for the whole document rather than one per card: the
 * build overview can hold ten of these at once, and they should all
 * step together.
 */
function boTickElapsed() {
	for (const el of document.querySelectorAll(".bo-elapsed")) {
		const base = Number(el.dataset.baseSeconds);
		const at = Number(el.dataset.baseAt);

		if (!Number.isFinite(base) || !Number.isFinite(at))
			continue;
		el.textContent = boDuration(base + (Date.now() - at) / 1000);
	}
}

setInterval(boTickElapsed, 1000);

/* #426: restore the remembered choice, then refresh on every change. */
document.addEventListener("DOMContentLoaded", () => {
	const box = document.getElementById("containers-show-internal");

	if (box === null)
		return;
	try {
		box.checked = storageGet("cix-containers-show-internal") === "1";
	} catch (e) {
		/* no stored preference available -- the default (off) stands */
	}
	box.addEventListener("change", () => {
		try {
			storageSet("cix-containers-show-internal", box.checked ? "1" : "0");
		} catch (e) {
			/* the choice just will not survive a reload */
		}
		refreshContainers();
	});
});

/*
 * #433: close the build-log viewer. Escape as well as the button,
 * because a panel covering what you were reading should close the way
 * every other dismissable thing on the page does -- and because the
 * bug being fixed is that it could not be closed at all.
 */
document.addEventListener("DOMContentLoaded", () => {
	const btn = document.getElementById("build-log-close");

	if (btn !== null)
		btn.addEventListener("click", closeBuildLog);
});

document.addEventListener("keydown", (event) => {
	const panel = document.getElementById("build-log-panel");

	if (event.key === "Escape" && panel !== null && !panel.hidden)
		closeBuildLog();
});

function boCpu(raw) {
	const parts = String(raw || "").trim().split(/\s+/);

	if (parts.length !== 2 || parts[0] === "max")
		return "unlimited";
	const quota = Number(parts[0]), period = Number(parts[1]);

	if (!isFinite(quota) || !isFinite(period) || period <= 0)
		return String(raw);
	const cores = quota / period;

	return (cores < 10 ? cores.toFixed(1) : String(Math.round(cores))) + " CPU";
}

/* Bytes as something a person reads. 0 means unlimited here, which is
 * the cgroup convention this field already uses. */
function boBytes(n) {
	const units = ["B", "KiB", "MiB", "GiB", "TiB"];
	let v = Number(n), i = 0;

	if (!isFinite(v) || v <= 0)
		return "unlimited";
	while (v >= 1024 && i < units.length - 1) {
		v /= 1024;
		i++;
	}
	return (v < 10 ? v.toFixed(1) : Math.round(v)) + " " + units[i];
}

async function boShowLog(name, image) {
	const wrap = document.getElementById("bo-log-wrap");
	const pre = document.getElementById("bo-log");
	const title = document.getElementById("bo-log-title");

	wrap.hidden = false;
	title.textContent = "Build log \u2014 " + name + "@" + image;
	pre.textContent = "Loading\u2026";
	try {
		const logs = await apiRequest("GET", CIX_API.listBuildLogs());
		/* Newest first, and a log is named "<pkg>-<version>-<epoch>.log",
		 * so prefix-match the package rather than guess its version. */
		const mine = (logs.logs || []).filter((l) => l.file.indexOf(name + "-") === 0);

		if (mine.length === 0) {
			pre.textContent =
			    "No build log for " + name + " yet. A log appears once the build writes its "
			    + "first output; until then the build is still composing its environment.";
			return;
		}
		const text = await apiRequestRaw("GET", CIX_API.getBuildLog(mine[0].file));
		const str = typeof text === "string" ? text : JSON.stringify(text);
		const lines = str.split("\n");

		/* The tail: a build's interesting output is the last thing it
		 * printed, and a full configure run is megabytes. */
		pre.textContent = lines.slice(Math.max(0, lines.length - 400)).join("\n");
		pre.scrollTop = pre.scrollHeight;
	} catch (e) {
		pre.textContent = "Could not read the build log: " + (e && e.message ? e.message : e);
	}
}

async function refreshBuildOverview() {
	const cap = document.getElementById("bo-capacity");
	const list = document.getElementById("bo-builds");
	const hint = document.getElementById("bo-builds-hint");
	let config;

	if (cap === null || list === null)
		return;
	try {
		config = await apiRequest("GET", CIX_API.getSystemPkgBuildConfig());
	} catch (e) {
		return; /* best-effort, same as every other refresher here */
	}

	const max = Number(config.max_concurrent_jobs) || 0;
	const busy = Number(config.active_jobs) || 0;

	cap.textContent = "";
	cap.appendChild(boCard("Build slots", String(max),
	    "max_concurrent_jobs -- set on the Log tab"));
	/* #422: the proportion, not just the pair. "7 of 10" needs mental
	 * arithmetic before it means anything; a percentage is the figure
	 * an operator actually wants when deciding whether to start
	 * another build, and 100% is the number that explains the 409 the
	 * next install would get. */
	cap.appendChild(boCard("In use",
	    busy + " of " + max + (max > 0 ? " (" + Math.round((busy / max) * 100) + "%)" : ""),
	    busy === 0 ? "nothing is building" : config.active_job_names));
	cap.appendChild(boCard("Free", String(Math.max(0, max - busy)),
	    "a further install is refused 409 at zero"));
	cap.appendChild(boCard("Memory budget", boBytes(config.memory_max),
	    "shared by every build at once, not per build"));
	cap.appendChild(boCard("CPU budget", boCpu(config.cpu_max),
	    config.cpu_max
	        ? "shared by every build at once -- cgroup v2 quota period: " + config.cpu_max
	        : "no cgroup cpu.max is set"));

	/* Fan out over the running jobs. Failures are per-card rather than
	 * fatal: one job disappearing between the two reads -- which is
	 * ordinary, a build can finish mid-refresh -- must not blank the
	 * whole page. */
	const names = (config.active_job_names || "").split(",")
	    .map((x) => x.trim()).filter((x) => x.length > 0);

	list.textContent = "";
	if (names.length === 0) {
		const idle = document.createElement("p");

		idle.className = "hint";
		idle.textContent = "No build is in flight.";
		list.appendChild(idle);
		if (hint)
			hint.hidden = true;
		document.getElementById("bo-log-wrap").hidden = true;
		return;
	}
	if (hint)
		hint.hidden = false;

	for (const key of names) {
		const at = key.lastIndexOf("@");
		const name = at < 0 ? key : key.slice(0, at);
		const image = at < 0 ? "" : key.slice(at + 1);
		const card = document.createElement("div");
		const h = document.createElement("h3");

		card.className = "stats-card bo-build";
		card.tabIndex = 0;
		card.setAttribute("role", "button");
		card.title = "Read this build's log";
		h.textContent = key;
		card.appendChild(h);

		const dl = document.createElement("dl");

		dl.className = "bo-kv";
		const add = (k, v) => {
			const dt = document.createElement("dt");
			const dd = document.createElement("dd");

			dt.textContent = k;
			dd.textContent = v;
			dl.appendChild(dt);
			dl.appendChild(dd);
		};

		try {
			const e = await apiRequest("GET", CIX_API.getPkg(key));

			add("state", e.state || "?");
			add("image", e.image || image);
			add("version", e.version || e.available_version || "\u2014");
			add("container", e.build_container || "\u2014 (not started)");
			/*
			 * #423: when it started, and how long it has been going.
			 *
			 * The counter ticks from run_seconds, the daemon's own
			 * figure, rather than from Date.now() - run_started_at:
			 * the second form differences this browser's clock against
			 * the daemon's, so any skew shows as a wrong elapsed time
			 * immediately. The absolute timestamp is used only to
			 * render the start time, which is what it is good for.
			 */
			if (e.run_started_at)
				add("started", new Date(e.run_started_at * 1000).toLocaleTimeString());
			if (e.run_seconds !== null && e.run_seconds !== undefined) {
				const dd = document.createElement("dd");
				const dt = document.createElement("dt");

				dt.textContent = "running";
				dd.className = "bo-elapsed";
				dd.dataset.baseSeconds = String(e.run_seconds);
				dd.dataset.baseAt = String(Date.now());
				dd.textContent = boDuration(e.run_seconds);
				dl.appendChild(dt);
				dl.appendChild(dd);
			}
			if (e.build_started_at)
				add("compiling since",
				    new Date(e.build_started_at * 1000).toLocaleTimeString());
			add("last output", e.last_output_seconds_ago === null ||
			    e.last_output_seconds_ago === undefined
			        ? "\u2014"
			        : e.last_output_seconds_ago + "s ago");
			add("hostbuild", e.is_hostbuild ? "yes" : "no");
			if (e.stage)
				add("stage", e.stage);
		} catch (err) {
			add("state", "could not read this job");
			add("why", err && err.message ? err.message : String(err));
		}
		card.appendChild(dl);
		card.addEventListener("click", () => boShowLog(name, image));
		card.addEventListener("keydown", (ev) => {
			if (ev.key === "Enter" || ev.key === " ") {
				ev.preventDefault();
				boShowLog(name, image);
			}
		});
		list.appendChild(card);
	}
}

const VIEW_REFRESHERS = {
	pipeline: [refreshPipeline],
	"pipeline-errors": [refreshPipeline],
	reconcile: [refreshImages, refreshPkgList],
	images: [refreshImages, refreshPkgList],
	recipes: [refreshPkgRecipes, refreshImageRecipesList, refreshContainerRecipesList],
	"pkg-recipes": [refreshPkgRecipes, refreshImageRecipesList, refreshContainerRecipesList],
	"image-recipes": [refreshImageRecipesList, refreshPkgRecipes, refreshContainerRecipesList],
	"container-recipes": [refreshContainerRecipesList, refreshPkgRecipes, refreshImageRecipesList],
	packages: [refreshPkgList, refreshImages],
	"pkg-repo": [refreshPkgRepoConfig, refreshPkgSyncStatus],
	"pkg-cache": [refreshPkgCacheConfig, refreshPkgCacheStatus, refreshPkgArtifactConfig],
	"build-overview": [refreshBuildOverview],
	"pkg-build-config": [refreshPkgBuildConfig],
	update: [refreshImages, refreshPkgList],
	devices: [refreshDevices, refreshDeviceMaps, refreshKmod, refreshKmodConfig],
	"dns-records": [refreshDnsRecords, refreshDnsServers, refreshDnsForwarders],
	"dns-servers": [refreshDnsRecords, refreshDnsServers, refreshDnsForwarders],
	"dns-forwarders": [refreshDnsRecords, refreshDnsServers, refreshDnsForwarders],
	"ldap-servers": [refreshLdapServers, refreshLdapGroups, refreshLdapUsers, refreshLdapConfig],
	"ldap-groups": [refreshLdapGroups, refreshLdapUsers],
	"ldap-users": [refreshLdapUsers, refreshLdapGroups],
	"ldap-config": [refreshLdapConfig, refreshLdapServers],
	"ntp-config": [refreshNtpConfig, refreshNtpServers],
	"dhcp-servers": [refreshDhcp],
	"dhcp-ranges": [refreshDhcp],
	"dhcp-static": [refreshDhcp],
	"dhcp-leases": [refreshDhcp],
	"ntp-servers": [refreshNtpServers, refreshNtpConfig],
	"ntp-time": [refreshNtpStatus, refreshNtpTime],
	"syslog-targets": [refreshSyslogTargets],
	"pki-ca": [refreshPkiCa, refreshPkiIntermediate, refreshPkiCerts],
	"pki-intermediate": [refreshPkiCa, refreshPkiIntermediate],
	"pki-certs": [refreshPkiCerts],
	"daemon-config": [refreshDaemonConfig, refreshSiteConfig],
	site: [refreshSiteConfig],
	routes: [refreshRoutes],
	sysctl: [refreshSysctl],
	"host-swap": [refreshSwap, refreshZswap],
	"rolling-restart": [refreshRollingConfig],
	"tls-throttle": [refreshTlsThrottleConfig, refreshTlsThrottleStatus],
	backup: [refreshBackupConfig, refreshBackupStatus],
	schedules: [refreshSchedules, refreshScheduleActions],
	iso: [refreshIso],
	volumes: [refreshVolumeCache],
};

const ALL_REFRESHERS = [
	refreshContainers, refreshNetworks, refreshImages, refreshDevices, refreshDeviceMaps,
	refreshDisks, refreshDiskRoles, refreshDiskFormatStatuses, refreshVolumeCache,
	refreshBackupConfig, refreshBackupStatus, refreshDnsRecords, refreshDnsServers,
	refreshLdapServers, refreshLdapGroups, refreshLdapUsers, refreshLdapConfig, refreshNtpConfig,
	refreshNtpServers, refreshSyslogTargets, refreshNtpStatus, refreshNtpTime, refreshPkiCa,
	refreshPkiIntermediate, refreshPkiCerts, refreshPkgRecipes, refreshImageRecipesList,
	refreshContainerRecipesList, refreshPkgList, refreshPkgRepoConfig, refreshPkgSyncStatus,
	refreshPkgCacheConfig, refreshPkgCacheStatus, refreshPkgArtifactConfig,
	refreshSiteConfig, refreshDaemonConfig, refreshRollingConfig,
	refreshPkgBuildConfig, refreshRoutes, refreshSysctl, refreshKmod, refreshKmodConfig,
	refreshSwap, refreshTlsThrottleConfig, refreshTlsThrottleStatus,
];

const SWEEP_INTERVAL_MS = 30000;
const VIEW_INTERVAL_MS = 5000;
let lastSweepAt = 0;
let lastViewRefreshAt = 0;

async function runRefreshers(list) {
	for (const fn of list) {
		try {
			await fn();
		} catch (e) {
			/* One endpoint failing must not stop the rest: a single
			 * unhappy subsystem should cost its own panel, not the
			 * whole dashboard's freshness. */
		}
	}
}

/*
 * Every refresher belonging to a PAGE, not just to the route that was
 * clicked.
 *
 * VIEW_REFRESHERS is keyed by route hash, which was right when one hash
 * meant one page. Now a page carries up to a dozen tabs, and clicking a
 * TAB does not change the route -- so its own refresher never ran and
 * the panel sat on "Loading" forever. Sysctl, Named Mappings, Discovery
 * and Kernel Modules all did exactly this.
 *
 * Derived from CATEGORY_VIEWS rather than declared separately, so a tab
 * added to a page is covered the moment its route is, and the two
 * cannot fall out of step the way the tree and the tab bars did.
 */
const VIEW_REFRESHERS_BY_VIEW = (() => {
	const byView = {};

	for (const hash of Object.keys(CATEGORY_VIEWS)) {
		const view = CATEGORY_VIEWS[hash];
		const fns = VIEW_REFRESHERS[hash];

		if (fns === undefined)
			continue;
		if (byView[view] === undefined)
			byView[view] = [];
		for (const fn of fns) {
			if (!byView[view].includes(fn))
				byView[view].push(fn);
		}
	}
	return byView;
})();

function refreshersForView(view) {
	return (view !== undefined && VIEW_REFRESHERS_BY_VIEW[view]) || [];
}

async function poll() {
	await refreshHealth();
	/* Health only while hidden: the LEDs and the reachability colour
	 * stay honest for anyone who looks back at the tab, and nothing
	 * else is being read by anybody. */
	if (document.hidden)
		return;

	const now = Date.now();
	const route = parseHash();
	const sweeping = now - lastSweepAt >= SWEEP_INTERVAL_MS;

	/* Core and view data every VIEW_INTERVAL_MS rather than on every
	 * tick: container status and disk state do not change faster than
	 * a person can read them, and the LEDs above are what make the page
	 * feel live. */
	if (!sweeping && now - lastViewRefreshAt < VIEW_INTERVAL_MS) {
		renderTree();
		renderCurrentView();
		return;
	}
	lastViewRefreshAt = now;

	if (sweeping) {
		lastSweepAt = now;
		await runRefreshers(ALL_REFRESHERS);
	} else {
		await runRefreshers(CORE_REFRESHERS);
		await runRefreshers(refreshersForView(CATEGORY_VIEWS[route.category]));
	}
	try {
		await pollServerLogs();
	} catch (e) {
		/* The log panel is best-effort like everything else here. */
	}
	renderTree();
	renderCurrentView();
}

/* ---------- tree right-click context menu ---------- */

const contextMenu = document.getElementById("tree-context-menu");

function hideContextMenu() {
	contextMenu.hidden = true;
	contextMenu.textContent = "";
}

function addContextMenuItem(item) {
	const li = document.createElement("li");
	const button = document.createElement("button");

	button.type = "button";
	button.textContent = item.label;
	if (item.danger)
		button.className = "button-danger";
	if (item.disabled) {
		/* Shown, not hidden: an action the daemon would refuse right now
		 * reads better greyed with its reason than silently absent -- the
		 * same choice the detail page makes for a mounted partition. */
		button.disabled = true;
		if (item.title)
			button.title = item.title;
	} else {
		button.addEventListener("click", () => {
			hideContextMenu();
			item.action();
		});
	}
	li.appendChild(button);
	contextMenu.appendChild(li);
}

/* Maps a tree link's own href to the actions a right-click menu offers.
 * Only the tree's individual leaves -- containers, networks, storage
 * (disks and partitions) and volumes -- have per-item actions; the
 * page-link nodes (Software, Services, Host and their children) get no
 * custom menu, because they are pages, not items. A leaf's actions come
 * from the same source its own detail page uses, never re-derived here
 * (diskActionEligibility() is the reference for storage). */
function contextMenuItemsFor(category, name) {
	if (category === "containers") {
		const c = cache.containers.find((x) => x.name === name);
		const status = c ? c.status : "running";
		const items = [
			{ label: "Open console", danger: false, action: () => { location.hash = "#containers/" + encodeURIComponent(name); } },
		];

		/*
		 * ADR-0181 (issue #73/#77): a container is no longer just
		 * running/paused/stopped. Since every container is persisted, one
		 * that exits on its own is RETAINED as "exited" (with its real exit
		 * code) rather than disappearing -- and it is startable again, just
		 * like a "stopped" one. "stopping"/"deleting" are transient
		 * teardown states with nothing useful to offer.
		 * Start  -> anything not currently alive (stopped OR exited).
		 * Stop   -> only something actually alive; stopping an already-dead
		 *           container is a no-op that reads like a broken button.
		 */
		const alive = status === "running" || status === "paused";
		const revivable = status === "stopped" || status === "exited";

		if (revivable) items.push({ label: "Start", danger: false, action: () => startContainer(name) });
		if (status === "running") items.push({ label: "Pause", danger: false, action: () => pauseContainer(name) });
		if (status === "paused") items.push({ label: "Unpause", danger: false, action: () => unpauseContainer(name) });
		if (alive) items.push({ label: "Stop", danger: false, action: () => stopContainer(name) });
		items.push({ label: "Remove", danger: true, action: () => removeContainer(name) });
		return items;
	}
	/*
	 * A disk or partition offers exactly what its own page's Role &
	 * Format tab offers, and nothing that would be refused: the OS disk
	 * gets no menu at all rather than a list of disabled entries, an
	 * unroled device can only be given a role, and delete is absent on
	 * a mounted partition because it would 409.
	 */
	if (category === "storage") {
		const d = cache.storage.find((x) => x.name === name);

		if (!d)
			return null;
		/*
		 * Every entry is gated by diskActionEligibility(d) -- the same
		 * source the detail page's Role & Format tab reads -- so the two
		 * offer exactly the same actions, and nothing here is re-derived.
		 */
		const e = diskActionEligibility(d);
		const items = [];

		if (e.addPartition)
			items.push({ label: "Add a partition\u2026", danger: false,
			             action: () => { location.hash = "storage/" + encodeURIComponent(name); } });
		if (e.assignRole)
			items.push({ label: "Assign a role\u2026", danger: false, action: () => openAssignRole(name) });
		if (e.removeRole)
			items.push({ label: "Remove role", danger: false, action: () => removeDiskRole(name) });
		if (e.format) {
			items.push({ label: "Format as btrfs", danger: true, action: () => formatDisk(name, "btrfs") });
			items.push({ label: "Format as ext4", danger: true, action: () => formatDisk(name, "ext4") });
		}
		if (e.unmount)
			items.push({ label: "Unmount", danger: false, action: () => unmountDisk(name) });
		if (e.grow)
			items.push({ label: "Grow\u2026", danger: false, action: () => growPartition(d) });
		if (e.del)
			items.push({ label: "Delete partition", danger: true, disabled: e.delDisabled,
			             title: e.delDisabled ? "Unmount it first." : "",
			             action: () => deletePartition(d.parent_disk, name) });
		return items.length ? items : null;
	}
	if (category === "volumes")
		return [{ label: "Delete volume", danger: true, action: () => deleteVolumeByName(name) }];
	if (category === "networks")
		return [{ label: "Remove", danger: true, action: () => removeNetwork(name) }];
	return null;
}

treeEl.addEventListener("contextmenu", (event) => {
	const link = event.target.closest("a.tree-item");

	if (link === null)
		return; /* category headers and non-leaf clicks keep the browser's own menu */

	const href = link.getAttribute("href").replace(/^#/, "");
	const slash = href.indexOf("/");

	if (slash < 0)
		return;
	const category = href.slice(0, slash);
	const name = decodeURIComponent(href.slice(slash + 1));
	const items = contextMenuItemsFor(category, name);

	if (items === null)
		return;

	event.preventDefault();
	contextMenu.textContent = "";
	for (const item of items)
		addContextMenuItem(item);

	const menuWidth = 180;
	const menuHeight = items.length * 32 + 8;
	const x = Math.min(event.clientX, window.innerWidth - menuWidth - 4);
	const y = Math.min(event.clientY, window.innerHeight - menuHeight - 4);

	contextMenu.style.left = Math.max(4, x) + "px";
	contextMenu.style.top = Math.max(4, y) + "px";
	contextMenu.hidden = false;
});

document.addEventListener("click", (event) => {
	if (!contextMenu.hidden && !contextMenu.contains(event.target))
		hideContextMenu();
});
document.addEventListener("keydown", (event) => {
	if (event.key === "Escape" && !contextMenu.hidden)
		hideContextMenu();
});
document.addEventListener("scroll", hideContextMenu, true);

restoreLastViewIfNoHash();
poll().then(ensureActiveCategoryExpanded);
/*
 * The loop itself still ticks at POLL_INTERVAL_MS, because that is the
 * cadence the health check (and therefore the LEDs) wants. poll() then
 * decides what else is worth fetching on this tick -- see its own
 * comment for the tiers. A hidden tab costs one small request per tick
 * and nothing else.
 */
setInterval(poll, POLL_INTERVAL_MS);

/* Coming back to the tab should feel instant, not "wait for the next
 * tick" -- and this is also when a tab that has been hidden for hours
 * catches up on everything it skipped. */
document.addEventListener("visibilitychange", () => {
	if (!document.hidden) {
		lastSweepAt = 0;
		poll();
	}
});

/*
 * The uptimes advance locally every second and are re-synced from the
 * daemon once a minute -- the display stays live while asking for it
 * sixty times less often. Load average is genuinely new information
 * each time, and one request a minute is the price of it. Skipped
 * entirely while the tab is hidden.
 */
refreshStatusVersion();
refreshStatusMeta();
setInterval(renderStatusMeta, 1000);
setInterval(() => {
	if (!document.hidden)
		refreshStatusMeta();
}, 60000);
