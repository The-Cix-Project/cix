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
/*
 * Each helper calls localStorage, never itself. From d566f879
 * (2026-09-13) until 0.2.57-518 all three called themselves -- the
 * sweep that replaced every localStorage call with these helpers reached
 * their own bodies too -- so each recursed until the stack overflowed,
 * the catch below swallowed the RangeError, and the dashboard stored
 * nothing: every reload signed the operator out. test_web_tree now
 * refuses a helper that does not reach localStorage.
 */
function storageGet(key) {
	try {
		return window.localStorage.getItem(key);
	} catch (e) {
		return null;
	}
}

function storageSet(key, value) {
	try {
		window.localStorage.setItem(key, value);
		return true;
	} catch (e) {
		return false;
	}
}

function storageRemove(key) {
	try {
		window.localStorage.removeItem(key);
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
	hostauthPermissions: null, /* #544: GET /v1/system/hostauth/permissions */
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
	pkgSyncStatus: null,
	pkgCacheConfig: null,
	pkgCacheStatus: null,
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
		document.body.classList.remove("resizing-col", "resizing-row");
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
		/* The cursor and the no-select for the whole page while the drag
		 * lasts -- a class, so what it looks like is the stylesheet's. */
		document.body.classList.add(axis === "x" ? "resizing-col" : "resizing-row");
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

	/* #544: a new app password's secret, which must not stay in the
	 * page once its modal is shut, whichever way it closed (submit, X,
	 * Escape, outside click). */
	appPasswordScope = null;
	document.getElementById("apf-secret-value").value = "";
	document.getElementById("apf-secret").hidden = true;

	/*
	 * The generated form is emptied rather than reset (ADR-0338): its
	 * controls were built from the operation's schema at open time, so
	 * removing them removes whatever was typed into any of them.
	 *
	 * That is also why this needs no entry per secret field. The eight
	 * the contract marks `format: password` -- and every one added
	 * later -- are covered by the fact that nothing survives the close,
	 * where the hand-written forms above each needed their own line
	 * here and the next one would have needed another.
	 */
	document.getElementById("generated-form").textContent = "";
}

function openModal(formId, title) {
	closeModal();
	modalTitle.textContent = title;
	document.getElementById("modal-" + formId).hidden = false;
	modalOverlay.hidden = false;
}

/*
 * ---------- the generated form, dashboard half (ADR-0338, #595) ----------
 *
 * `web/form.js` turns the contract's own description of a request into
 * controls; this is the part that needs the dashboard -- the modal, the
 * session, the status line and the refresh. The split is so the first
 * half can be EXECUTED by a test rather than only parsed (see that
 * file's own header).
 *
 * What is left here is the submit, and only because it is the one part
 * that cannot be stubbed meaningfully: it is an HTTP request and the
 * status line. Everything up to and including `openForm()` lives in
 * form.js, which is how `test_web_form` can assert that a form opens
 * with its fields present -- a property that was briefly broken in the
 * first draft, found by reading the code rather than by any gate,
 * because `openModal()` begins by calling `closeModal()` and the
 * fields were being built before it.
 */
/*
 * ONE submit for every generated form, wherever it is mounted (#592):
 * the modal's, and every settings form on a page. Delegated from the
 * document because a page's forms are bound as its data arrives, and a
 * listener per bind would multiply with every rebuild. A form without
 * `_cix` is hand-built and keeps its own listener.
 */
document.addEventListener("submit", (event) => {
	if (event.target._cix === undefined)
		return;
	event.preventDefault();
	submitForm(event.target);
});

async function submitForm(form) {
	const c = form._cix;
	const shape = CIX_API[c.opId + "_SHAPE"];
	const args = generatedFormArgs(form);
	let body;
	let result;

	/* A destructive operation confirms, because the contract says it is
	 * destructive -- not because whoever wrote this screen remembered.
	 * An authored `before` asks INSTEAD, never as well: it can say what
	 * is about to be destroyed, and two dialogs for one click teach
	 * people to click through both. */
	if (shape.destructive && c.before === null &&
	    !window.confirm("This cannot be undone. Continue?"))
		return;
	try {
		/* Read BEFORE the request, and inside the try: collecting the
		 * body can refuse it -- a free-form object field is typed as
		 * JSON and parsed here, so a mistyped brace must report
		 * itself and leave the form open with the text still in it,
		 * exactly like a 400 would. */
		body = generatedFormBody(form);
		/* The authored question, asked about what is actually about
		 * to be sent (#592): letting a source write to its forge is
		 * confirmed only when this save turns it on. Declining leaves
		 * the form open with everything still in it. */
		if (c.before !== null && !(await c.before(body)))
			return;
		result = await apiRequest(shape.method, CIX_API[c.opId](...args), body);
		clearStatus();
		/* Saved: what is on screen is the box's again, so the next
		 * read may refill it (syncForm()). */
		form.dataset.dirty = "";
		/* The modal closes, which says it worked. A form on a page
		 * stays where it is, so it has to say so in words -- a Save
		 * that changes nothing visible reads as a Save that did
		 * nothing. */
		if (form.id === "generated-form")
			closeModal();
		else
			showStatus(c.saved || "Saved", false);
		if (c.opts.clear === true)
			bindForm(form, c.opId, c.opts);
		/*
		 * The body, because an `after` often needs the name of the
		 * thing just written and on a create nobody knew it until it
		 * was typed -- and the response, because an operation with
		 * more than one success answers which one it took. applying a
		 * container recipe is the case: 201 created, 202 queued behind
		 * an image build (ADR-0270), and the hand-written form
		 * reported "created" for both.
		 */
		if (c.after !== null)
			await c.after(body, result);
	} catch (e) {
		showStatus(c.failed + ": " + e.message, true);
	}
}

document.getElementById("signing-keys-clear").addEventListener("click", clearSigningKeys);
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

/*
 * What a header-menu generated form needs beyond its contract (#592),
 * authored per operation, in the one place it is said:
 *
 *   choices  what really exists to pick from, read from the box when
 *            the form opens -- the disks with no role, the networks
 *            with no range, the recipe sources
 *   sections the layout of a long form, which is authored -- only New
 *            Container has one
 *   before   a question about what is about to be sent, as the edit
 *            buttons on those pages ask it
 *   after    more than refreshing the page: issuing a certificate has
 *            to SHOW its response, because that response is the only
 *            time its key exists anywhere but the host
 *
 * Each is the same function the page's own button uses, so the menu
 * and the page cannot drift apart.
 */
const GENERATED_FORM_HOOKS = {
	createContainer: {
		choices: containerCreateChoices,
		sections: containerFormSections,
		after: afterContainerCreate,
	},
	createPkiCert: { after: showIssuedCert },
	createStorageRole: { choices: diskRoleChoices, after: refreshDiskRoles },
	setDhcpNetwork: { choices: () => dhcpRangeChoices(true), after: afterDhcpRangeSave },
	addPkgRecipe: { choices: recipeSourceChoices, after: afterPkgRecipeSave },
	addPkgSource: { before: (body) => confirmPkgSourceTrust(body, null), after: refreshPkgSources },
	addPkgRepository: {
		before: (body) => confirmPkgRepositoryPush(body, null),
		after: refreshPkgRepositories,
	},
};

/* Create-action items (data-modal, shared across every topical menu
 * above) all open the same modal shell the old single +Create
 * dropdown already used -- only where they live changed. */
for (const item of document.querySelectorAll(".menu-bar button[data-modal]")) {
	/* `data-modal="generated-form"` means the form is built from the
	 * operation's request schema (ADR-0338) rather than written out in
	 * index.html -- the only per-entry difference is the three authored
	 * words on its button, which `data-submit` carries. */
	if (item.dataset.modal === "generated-form") {
		const hooks = GENERATED_FORM_HOOKS[item.dataset.op] || {};

		item.addEventListener("click", async () => {
			let choices;

			/* A list that could not be read opens NO form: a select of
			 * nothing would read as "there are none", which is the one
			 * thing nobody measured. */
			if (hooks.choices !== undefined) {
				try {
					choices = await hooks.choices();
				} catch (e) {
					showStatus("Could not read what " + item.dataset.title.toLowerCase() +
					           " can choose from: " + e.message, true);
					return;
				}
			}
			openForm(item.dataset.op, {
				title: item.dataset.title,
				submit: item.dataset.submit,
				choices: choices,
				sections: hooks.sections !== undefined ? hooks.sections() : undefined,
				before: hooks.before,
				after: hooks.after,
			});
		});
		continue;
	}
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
	/* ADR-0339: a stream opened with the old token goes now, not at the
	 * daemon's next heartbeat; the session check below opens one for the
	 * new token, if there is one. */
	stopEventStream();
	/* #544: what the new session may do (or that there is none). */
	refreshSessionPermissions();
}

/*
 * True while the periodic sweep is running its refreshers, so a 401
 * from one of them never opens the login modal (#490).
 *
 * The comment below used to say "several concurrent GETs are never
 * gated", and that was true when it was written. Gating the identity
 * reads made it false, and the result was a login prompt that
 * reappeared on a timer: the sweep runs refreshLdapUsers() for every
 * visitor on every page, it 401s when nobody is signed in, and every
 * 401 opened the form again -- including immediately after a
 * successful login, because a second gated read from the same cycle
 * lands after the modal closes.
 */
let inBackgroundSweep = false;

/* Opens the login modal on a 401 from any request the PERSON made, so
 * "why did my action just fail" has an immediate, actionable answer
 * instead of only a status-bar error -- the same reasoning cixctl's
 * own "authentication required -- POST /v1/login first" message
 * serves, adapted to a UI that can just show the form directly.
 *
 * Never from a background refresh. A poll is not the person's action,
 * and a modal it opens interrupts whatever they were actually doing
 * -- which is the bug this guard exists to prevent, not a
 * hypothetical. Guarded as well so a burst of 401s from one cycle
 * only ever opens it once. */
function promptReauth() {
	if (inBackgroundSweep)
		return;
	/*
	 * The shell converges (#597). No `modalOverlay.hidden` guard any
	 * more: that existed because one modal could not open over another,
	 * and the session shell is not a modal -- it goes over whatever is
	 * there, and closes it, because nothing under it is reachable now.
	 *
	 * "expired" only when there WAS a session. A first load against a
	 * gating host 401s too, and telling someone their session expired
	 * when they have never had one is a small lie that reads as a bug
	 * in the host. The first deployed render said exactly that, which
	 * is how this was found.
	 */
	lockSession(sessionEverAuthenticated ? "expired" : "first");
}

/*
 * ADR-0317 (#544): what this session may do -- the one place the
 * dashboard answers it, and every surface asks here.
 *
 * The permission an operation needs comes from the contract: apigen
 * emits `<opId>_PERMISSION` into CIX_API (web/api.js), so there is no
 * second copy of the policy in this file. What the session HOLDS comes
 * from GET /v1/whoami (public, and it never spends a single-use token);
 * whether the host enforces anything comes from GET /v1/health. An
 * ungated host allows everything, as the daemon does.
 *
 * Looked up by operation name rather than by `CIX_API.<op>_PERMISSION`,
 * so asking about an operation is not mistaken for calling it by
 * test_api_surfaces, which decides from the source which operations
 * the dashboard uses.
 */
let hostGated = false;
let sessionAuthenticated = false;
/*
 * Whether a session has EVER existed in this page's life (#597).
 *
 * Only this can tell "your session expired" apart from "this host
 * wants a login", because both arrive as the same 401. Never reset:
 * once a session has existed, a later refusal really is an expiry.
 */
let sessionEverAuthenticated = false;
let sessionPermissions = new Set();
let sessionChecked = false; /* whoami answered at least once this page load */

function permissionFor(opId) {
	return CIX_API[opId + "_PERMISSION"];
}

function sessionMay(opId) {
	const permission = permissionFor(opId);

	if (permission === undefined)
		return false; /* an operation the contract does not name: never offer it */
	if (permission === "public" || !hostGated)
		return true;
	if (!sessionAuthenticated)
		return false;
	return permission === "authenticated" || sessionPermissions.has(permission);
}

/* Why sessionMay(opId) said no, in words: the reason a disabled
 * control carries. */
function sessionRefusal(opId) {
	const permission = permissionFor(opId);

	if (!sessionAuthenticated)
		return "Log in first -- this host needs a session";
	return "Needs the " + permission + " permission, which none of your groups grants";
}

/*
 * Every control tagged data-op="<opId>" -- the header's "New ..." and
 * power entries -- is enabled exactly when the session may perform that
 * operation, and otherwise disabled with its reason (web-ux-guidelines:
 * disabled-with-reason, since logging in or a grant would unblock it).
 */
/*
 * #548: which operation a request is, from the contract's own path
 * templates (`<opId>_PATH`, generated into web/api.js). Matched with the
 * daemon's precedence (daemon/src/apiroute.c): among templates of the
 * same length, a literal segment beats a {parameter}, leftmost first --
 * so GET /v1/containers/recipes is listContainerRecipes, never
 * getContainer with name "recipes". Built once, by iterating the table,
 * so nothing here names an operation.
 */
const OP_INDEX = (() => {
	const list = [];

	for (const key of Object.keys(CIX_API)) {
		if (!key.endsWith("_PATH"))
			continue;

		const op = key.slice(0, -"_PATH".length);

		list.push({ op: op, method: CIX_API[op + "_METHOD"], segs: CIX_API[key].split("/") });
	}
	list.sort((a, b) => {
		for (let i = 0; i < Math.min(a.segs.length, b.segs.length); i++) {
			const la = !a.segs[i].startsWith("{");
			const lb = !b.segs[i].startsWith("{");

			if (la !== lb)
				return la ? -1 : 1;
		}
		return 0;
	});
	return list;
})();

/* ADR-0339 phase 2, release B (see runRefreshersInner()): each
 * refresher's reads, and the set the running one's requests go into. */
const REFRESHER_READS = new Map(); /* refresher -> Set of operationIds it has read */
let recordingReads = null;

function opForRequest(method, path) {
	const segs = path.split("?")[0].split("/");

	for (const entry of OP_INDEX) {
		if (entry.method !== method || entry.segs.length !== segs.length)
			continue;
		if (entry.segs.every((s, i) => s.startsWith("{") || s === segs[i]))
			return entry.op;
	}
	return undefined;
}

/*
 * #548: reads that did not succeed -- refused by the pre-check or by
 * the daemon, or failed outright -- keyed by operation, cleared when
 * the same read succeeds. What a panel's empty state reads
 * (emptyStateText()), so a read that did not happen is never shown as
 * "No <things>" (#538's claim about a host that was not asked).
 *
 * It was `refusedReads` and held 401/403 only, which left a 500, a
 * timeout or an unreachable host recorded nowhere -- so a panel whose
 * read really had failed fell through to its "No <things>" sentence,
 * the exact lie the name says it exists to prevent. `refusalText()`
 * has always had the branch for it ("Could not load X: ..."); nothing
 * reached it except four call sites passing the error by hand.
 */
const failedReads = new Map();

/*
 * EVERY LIST THAT IS ITS OWN READ HAS THREE STATES, and the one that
 * was missing is loading (#592, the Primitives board: "Loading is not
 * empty. Showing 'none' while a fetch is in flight is a lie, and it is
 * where much of today's bad experience lives").
 *
 * Measured in Chromium before this existed, with no backend and the
 * app driven the way a person drives it: navigating to Networks
 * rendered "No routes." -- a claim the host was asked and has none,
 * when nothing had been answered. 24 list caches start as `[]`, and
 * one render paints that as "none".
 *
 * The honest question is not "is this list empty" but "has this read
 * happened", and until now nothing recorded the answer. `answered`
 * does: one entry per operation that has returned at least once, set
 * beside the line that clears a failure. So the window this fixes is
 * the one the measurement found -- before the FIRST successful read --
 * and it needs no change to any of the 27 panels, because they already
 * route their empty state through here.
 *
 * Note what does NOT get a loading state: a list that is a field of an
 * already-loaded object (a container's `interfaces`, `env`, `files` --
 * six of `simpleTableRows`' fourteen callers). Those were never
 * separately read, so "empty" is the whole truth about them, and
 * giving them a loading state would invent a fetch that never happens.
 */
const answeredReads = new Set();

/*
 * WHICH of the three states a list is in, and its sentence, as one
 * value -- because they are not independent. The Primitives board
 * styles a refusal at full --text while loading and empty are muted
 * (`color: "#F3F0E7"` against `"#98A2A8"`), so a caller holding only
 * the sentence cannot render the state correctly, and for a while none
 * of them did: all three printed muted.
 *
 * The board also tints each state's CAPTION differently (loading blue,
 * empty muted, refused amber). That is the board labelling its own
 * specimens, not a fourth status colour -- inside the table only the
 * body colour differs, and only for refused.
 */
function listState(opId, what, normalText) {
	/*
	 * `null` -- deliberately null, never merely absent -- is a list
	 * that is a FIELD OF AN ALREADY-LOADED OBJECT: a network's
	 * `interfaces`, a container's `env`. The guidelines' List state row
	 * says it plainly: such a list has one state, not four, because it
	 * was never separately read, and a loading state would invent a
	 * fetch that never happens. Without this branch a table given no
	 * operation fell through to "Reading…" and stayed there.
	 */
	if (opId === null)
		return { kind: "empty", text: normalText };

	const failure = failedReads.get(opId);

	if (failure)
		return { kind: "refused", text: refusalText(failure, what) };
	if (!answeredReads.has(opId))
		return { kind: "loading", text: "Reading…" };
	return { kind: "empty", text: normalText };
}

function emptyStateText(opId, what, normalText) {
	return listState(opId, what, normalText).text;
}

/*
 * #548: a control the dashboard builds at render time names the
 * operation it performs here, the way a static one carries data-op.
 * Only ever DISABLES -- with sessionRefusal() as its reason -- so a
 * control that is already disabled for its own reason (a stopped
 * container's service buttons, a mounted partition's delete) stays
 * disabled whichever reason came first. Returns el, to chain.
 */
function gateAction(el, opId) {
	if (!sessionMay(opId)) {
		el.disabled = true;
		el.title = sessionRefusal(opId);
	}
	return el;
}

function applySessionEligibility() {
	for (const el of document.querySelectorAll("[data-op]")) {
		const may = sessionMay(el.dataset.op);

		el.disabled = !may;
		el.title = may ? "" : sessionRefusal(el.dataset.op);
	}
	const banner = document.getElementById("login-required-banner");

	if (banner)
		banner.hidden = !(hostGated && !sessionAuthenticated);
	/* Your own app passwords need a login session (ADR-0317 section 8). */
	document.getElementById("menu-own-app-passwords").hidden = !sessionAuthenticated;
}

/*
 * #544: what a panel says when its read fails -- in one place, so every
 * panel that uses it tells a refused read apart from a broken one. A 401
 * means there is no session; a 403 names the permission the session
 * lacks (the daemon puts it in the body, ADR-0317); anything else is a
 * real failure. `what` is the panel's subject, e.g. "LDAP users".
 * Never "no <things>": nothing was read, so that would be a claim about
 * the host that was never checked (#538).
 */
function refusalText(e, what) {
	if (e && e.status === 401)
		return "Log in to see " + what;
	if (e && e.status === 403)
		return "Your groups do not grant " + (e.permission || "the permission") +
		       ", which is needed to see " + what;
	return "Could not load " + what + ": " + (e && e.message ? e.message : "unknown error");
}

async function refreshSessionPermissions() {
	try {
		const me = await apiRequest("GET", CIX_API.getWhoami());
		const wasAuthenticated = sessionAuthenticated;

		sessionAuthenticated = !!(me && me.authenticated);
		if (sessionAuthenticated)
			sessionEverAuthenticated = true;
		/* The grants first: everything below asks sessionMay(), which
		 * reads them. Fetching before this line is how the status bar
		 * lost its version after every daemon restart -- both reads
		 * were refused locally for want of a permission the session
		 * held, and nothing asked again (seen in Chromium against the
		 * box's own 0.2.57-523 files, 2026-10-17). */
		sessionPermissions = new Set((me && me.permissions) || []);
		sessionChecked = true;
		/* A session just began: sweep on the next tick rather than up
		 * to SWEEP_INTERVAL_MS later, so the page fills in at once --
		 * and the status bar now, since both of its reads need a login
		 * (ADR-0317). */
		if (sessionAuthenticated && !wasAuthenticated) {
			lastSweepAt = 0;
			refreshStatusBar();
		}
	} catch (e) {
		/* Unanswered: keep what is known rather than sign someone out
		 * on a transport error. */
		return;
	}
	applySessionEligibility();
	/* ADR-0339: the event stream follows the session it was opened with. */
	syncEventStream();
}

/*
 * #541: the first health answer reporting gating on, while this page has
 * no live session, opens the login form -- once per page load. Not on
 * every poll: closing the form is the person's choice, and a form that
 * reopens on a timer is the bug #490 fixed. A stored token that has
 * lapsed counts as no session and is cleared. The standing reminder
 * after that is the login-required banner, and the background sweep
 * stays quiet meanwhile (poll()).
 */
let loginOfferedOnLoad = false;

async function noteHostGating(gatingActive) {
	const gated = gatingActive === true;
	const changed = gated !== hostGated;

	hostGated = gated;
	if (changed || !sessionChecked)
		await refreshSessionPermissions();
	if (loginOfferedOnLoad || !hostGated)
		return;
	loginOfferedOnLoad = true;
	if (sessionAuthenticated)
		return;
	if (authToken)
		setAuth(null, null);
	/* First load against a gating host: the shell is already closed in
	 * the markup, so this is what states WHY (#597). */
	lockSession("first");
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
		/* Logging out is the operator CHOOSING the boundary, so the
		 * shell converges and says so -- the same motion an expiry
		 * produces, because it is the same state (#597). */
		lockSession("manual");
	} else {
		lockSession("first");
	}
});


/*
 * ---------- the session shell (ADR-0338, #597) ----------
 *
 * Locking and unlocking the dashboard, which is the session boundary
 * and nothing else. Not a modal: web-ux-guidelines' session-shell row
 * exists to end `openModal("login-form", "Log in")`, the
 * create-a-resource widget doing a session's job.
 *
 * The right panel's fields are GENERATED from `postLogin`'s own request
 * schema, so the password field is obscured because the contract says
 * `format: password`. That is the whole reason those eight fields were
 * annotated: a login screen is the one form where getting the control
 * wrong is a security defect rather than an inconvenience.
 */
const LOCK_NOTICES = {
	first: "Enter the credentials for this host.",
	expired: "The session expired. The dashboard was cleared.",
	manual: "Locked. The dashboard was cleared.",
	refused: "That did not work. Check the username and password.",
};

let lockShellBuilt = false;

/*
 * The credentials, from the contract.
 *
 * `fieldControl()` gives the control the field's own constraints call
 * for -- an obscured input for `password`, a plain one for `username`,
 * both required -- and this authors only the layout around it. Built
 * once: the schema cannot change while the page is open.
 */
function lockFields() {
	const host = document.getElementById("lock-fields");
	const fields = CIX_API.FIELDS[CIX_API.postLogin_SHAPE.request] || [];

	if (lockShellBuilt)
		return;
	host.textContent = "";
	for (const f of fields) {
		const label = document.createElement("label");
		const name = document.createElement("span");
		const control = fieldControl(f);

		label.className = "lock-field";
		name.textContent = fieldLabel(f.name);
		label.appendChild(name);
		/* autocomplete is a browser affordance rather than a schema
		 * fact, and it is the one thing a password manager needs to
		 * fill this form at all. */
		control.autocomplete = f.format === "password" ? "current-password" : "username";
		label.appendChild(control);
		host.appendChild(label);
	}
	lockShellBuilt = true;
}

/*
 * Lock, for a stated reason.
 *
 * The dashboard is CLEARED on the way down, and that is deliberate:
 * leaving the last session's containers and addresses rendered behind a
 * login screen is #562's complaint in the other direction -- a surface
 * that looks right while being wrong. What the panels part to reveal
 * once a session exists is freshly read.
 */
function lockSession(reason) {
	const shell = document.getElementById("lock-shell");
	const notice = document.getElementById("lock-notice");

	lockFields();
	notice.textContent = LOCK_NOTICES[reason] || LOCK_NOTICES.first;
	/*
	 * Never leave a typed password in the DOM across a session
	 * boundary. The modal this replaced cleared one field by id; this
	 * resets the whole form, which covers every field the contract has
	 * or later gains -- the same reasoning as closeModal()'s treatment
	 * of the generated form.
	 */
	document.getElementById("lock-form").reset();
	shell.hidden = false;
	shell.classList.remove("lock-shell-open");
	refreshLockFacts();
	/* Anything that was open under the lock is not reachable now, so it
	 * does not stay open behind it. */
	if (!modalOverlay.hidden)
		closeModal();
	const first = document.querySelector("#lock-fields input:not([type=password])");

	if (first !== null)
		first.focus();
}

/*
 * Unlock. The panels leave along the axis; the shell is hidden only
 * once they have, so `display: none` never truncates the motion.
 */
function unlockSession() {
	const shell = document.getElementById("lock-shell");

	shell.classList.add("lock-shell-open");
	window.setTimeout(() => {
		if (shell.classList.contains("lock-shell-open"))
			shell.hidden = true;
	}, 320);
}

/*
 * The two facts the left panel states, and the reason there are only
 * two: `GET /v1/health` is the one read a caller with no session may
 * perform, so it is the only honest source here. Build, slot and kernel
 * all need a login, so they are absent rather than guessed.
 *
 * Reachability reads the counter refreshHealth() already keeps, rather
 * than a second flag: that counter encodes this dashboard's own policy
 * of not calling a host unreachable on one missed check, and a login
 * screen is the worst place to be more alarmist than the status bar.
 */
function refreshLockFacts() {
	const led = document.getElementById("lock-host-led");
	const state = document.getElementById("lock-host-state");
	const access = document.getElementById("lock-access");

	document.getElementById("lock-host").textContent = window.location.host;
	if (consecutiveHealthFailures === 0) {
		led.style.setProperty("--led-color", "var(--phosphor)");
		state.textContent = "reachable";
	} else if (consecutiveHealthFailures === 1) {
		led.style.setProperty("--led-color", "var(--amber)");
		state.textContent = "retrying";
	} else {
		led.style.setProperty("--led-color", "var(--fault)");
		state.textContent = "not answering";
	}
	access.textContent = hostGated ? "This host requires a login"
	                               : "This host is not gating reads";
}

document.getElementById("lock-form").addEventListener("submit", async (event) => {
	const form = event.target;
	const body = {};

	event.preventDefault();
	for (const el of form.querySelectorAll("[data-field]"))
		body[el.dataset.field] = el.value;
	try {
		const result = await apiRequest("POST", CIX_API.postLogin(), body);

		setAuth(result.token, body.username);
		form.reset();
		clearStatus();
		unlockSession();
		/*
		 * What the panels part to reveal is read FRESH -- the
		 * permissions this session has, the core panels, and the view
		 * the operator is actually looking at. Without the last one,
		 * the dashboard underneath would show the previous session's
		 * rows for up to one poll interval, which is the "looks right
		 * while being wrong" surface #562 was filed for.
		 */
		await refreshSessionPermissions();
		await runRefreshers(CORE_REFRESHERS);
		await refreshVisibleView();
	} catch (e) {
		document.getElementById("lock-notice").textContent = LOCK_NOTICES.refused;
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
 * ADR-0070/ADR-0126), a live read subscribed on the event stream and
 * polled only while the stream is down (ingestServerLogs(), via
 * startLiveRead(), ADR-0339 phase 2). The two are kept apart -- serverLogs is
 * replaced wholesale by each sample, clientLogs is only ever appended to
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
	/* A CLOCK, not formatInstant(): this is one line of a dense log of
	 * hundreds, every entry within minutes of its neighbours, so the
	 * date would be the same noise repeated on every row. The one
	 * deliberate exception to the instant format, along with the UTC
	 * line in the time panel and the pipeline's two "since" fields. */
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

/* One sample of the log panel's tail, pushed or polled (startLiveRead(),
 * started once at load). */
function ingestServerLogs(entries) {
	if (!Array.isArray(entries))
		return;

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
/*
 * A container's own four states, mapped to the shared badge kinds --
 * the same four `containerStatusColorClass()` already tints the tree
 * icon with, in one function so the two surfaces cannot disagree
 * (#592).
 *
 * They did disagree. The containers table mapped its badge inline as
 * `running ? ok : paused ? paused : unknown`, which makes an EXITED
 * container grey, while the tree icon has always shown it red --
 * ADR-0045's whole point being that "exited" is its own condition
 * (the process died unexpectedly) and the one state worth flagging at
 * a glance, as distinct from "stopped", which is deliberate and fine.
 * So the list said nothing was wrong and the tree said something was,
 * about the same container, at the same moment.
 */
function containerStatusKind(status) {
	switch (status) {
	case "running":
		return "ok";
	case "paused":
		return "paused";
	case "exited":
		return "error";
	default:
		return "unknown"; /* "stopped": deliberate, not a fault */
	}
}

/*
 * A SERVICE's seven states, mapped to the same four kinds -- and the
 * one word both vocabularies share means opposite things, which is why
 * this is its own function rather than containerStatusKind() reused.
 *
 * For a container, "exited" is the fault. For a service it is a
 * FINISHED ONESHOT, or a service whose declared on_exit is stop: the
 * contract's own definition is *"exited and not restarting (a finished
 * oneshot, or on_exit stop)"*. Measured on 192.168.15.95, 2026-10-16:
 * `jump`'s `mkdirs` and `waitkey` are both oneshots reporting
 * `"state":"exited"` with `last_exit` status 0 -- they did their job --
 * so colouring "exited" red would put two alarms on a healthy
 * container. The service fault is "failed": it *"cannot run"*.
 *
 * The three states that are on their way somewhere -- pending (waiting
 * for its `after` set), starting (running, probe not passed) and
 * restart-wait (respawning after its delay) -- are amber, as a paused
 * container is: not wrong, not settled.
 */
/*
 * Which lifecycle actions a container offers in a given status -- the
 * one decision the container page's buttons and the tree's right-click
 * menu both render from, as diskActionEligibility() is for a disk.
 *
 * They decided separately until 2026-10-17, and had drifted: the page
 * showed Start only for "stopped", so an exited container could not be
 * started from its own page, and showed Stop for everything else,
 * exited included, where it does nothing.
 *
 * ADR-0181 (issue #73/#77): a container is no longer just
 * running/paused/stopped. Since every container is persisted, one that
 * exits on its own is RETAINED as "exited" (with its real exit code)
 * rather than disappearing -- and it is startable again, just like a
 * "stopped" one. "stopping"/"deleting" are transient teardown states
 * with nothing useful to offer.
 *   Start -> anything not currently alive (stopped OR exited).
 *   Stop  -> only something actually alive; stopping an already-dead
 *            container is a no-op that reads like a broken button.
 * Remove is not here: it applies in every status.
 */
function containerActionEligibility(status) {
	return {
		start: status === "stopped" || status === "exited",
		pause: status === "running",
		unpause: status === "paused",
		stop: status === "running" || status === "paused",
	};
}

function serviceStateKind(state) {
	switch (state) {
	case "running":
		return "ok";
	case "pending":
	case "starting":
	case "restart-wait":
		return "paused";
	case "failed":
		return "error";
	default:
		return "unknown"; /* "exited" and "stopped": settled, not a fault */
	}
}

/*
 * A registered backend server's probe verdict (`ServerHealth.state`) as
 * a badge. "unknown" is the contract's "registered but not probed yet",
 * which is grey rather than amber: nothing is in progress, there is
 * simply no answer yet.
 */
function serverHealthStateKind(state) {
	switch (state) {
	case "healthy":
		return "ok";
	case "unhealthy":
		return "error";
	default:
		return "unknown";
	}
}

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

/*
 * The box said this session is over: a 401, or the event stream closing
 * with 4401 (ADR-0339). One place, so the two cannot disagree about what
 * it means. `tokenStillCurrent` is whether the refused token is the one
 * held now -- a 401 for a token already replaced by a fresh login must
 * not sign that new session out.
 */
function sessionLapsed(tokenStillCurrent) {
	if (tokenStillCurrent)
		setAuth(null, null);
	if (sessionAuthenticated) {
		/* #544: the session this page believed in is gone. */
		sessionAuthenticated = false;
		applySessionEligibility();
	}
}

async function apiRequest(method, path, body, timeoutMs) {
	/*
	 * #548: a request this session is known not to be allowed is refused
	 * here, without a request -- the same error shape a daemon 401/403
	 * has, plus `local`. Without it, a session narrower than a panel's
	 * read sent that read on every sweep, and the daemon audited every
	 * refusal. This is a mirror of the daemon's rule, not a second rule:
	 * sessionMay() reads the contract's permission and whoami's grants,
	 * both re-read every sweep (poll()), and where they could disagree
	 * -- a grant made a moment ago -- the next sweep catches up.
	 */
	const reqOp = opForRequest(method === "HEAD" ? "GET" : method, path);

	/* Release B: the running refresher reads this (refused or not). */
	if (recordingReads !== null && method === "GET" && reqOp !== undefined)
		recordingReads.add(reqOp);
	if (reqOp !== undefined && !sessionMay(reqOp)) {
		const err = new Error(sessionAuthenticated
			? "permission \"" + permissionFor(reqOp) + "\" is required for " + reqOp +
			  ", and none of your groups grants it"
			: "authentication required -- POST /v1/login first");

		err.status = sessionAuthenticated ? 403 : 401;
		err.permission = permissionFor(reqOp);
		err.local = true;
		if (method === "GET")
			failedReads.set(reqOp, err);
		throw err;
	}
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
	/* #562: which session this request speaks for, for its 401 below. */
	const sentToken = authToken;

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
		/* Nothing answered at all, which is a failed read like any
		 * other: without this the panel sits on "Reading…" while the
		 * host is unreachable, and `refusalText()` has the sentence
		 * for it ("Could not load X: ..."). */
		if (method === "GET" && reqOp !== undefined)
			failedReads.set(reqOp, e);
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
		/* #544: a 403 names the permission it wanted (ADR-0317). */
		err.permission = json && json.permission ? json.permission : undefined;
		/* Any failed GET, not only 401/403 -- a panel whose read
		 * returned 500 or never arrived must not fall through to
		 * "No <things>". refusalText()'s third branch names it. */
		if (method === "GET" && reqOp !== undefined)
			failedReads.set(reqOp, err);
		if (method !== "GET")
			logLine(method, path, "-> " + res.status + " " + message, "error");
		if (res.status === 401 && path !== CIX_API.postLogin())
			promptReauth();
		/*
		 * #562: the daemon refused the session this page holds, so drop
		 * it now. The header, the Log in button and the login-required
		 * banner then say what is true, instead of "logged in" over
		 * panels that are being refused. Only the token this request
		 * carried: a reply sent before a fresh login must not sign that
		 * login out. No modal from here, because a background poll
		 * never opens one (#490); the person's next action does.
		 */
		if (res.status === 401 && path !== CIX_API.postLogin())
			sessionLapsed(sentToken && sentToken === authToken);
		throw err;
	}
	if (method !== "GET")
		logLine(method, path, "-> " + res.status, "ok");
	if (method === "GET" && reqOp !== undefined) {
		failedReads.delete(reqOp);
		/* This read has now happened at least once, which is what
		 * tells a panel's empty state apart from its loading one. */
		answeredReads.add(reqOp);
	}
	return json;
}

/* Raw variant for /v1/system/backup|restore -- the response/request
 * body IS the bundle, written/read byte-for-byte, the same "exact
 * round trip, not re-serialized" guarantee cixctl's own backup/
 * restore commands already have (cli/src/main.c). */
async function apiRequestRaw(method, path, rawBody) {
	const opts = { method: method, headers: {} };
	const sentToken = authToken; /* #562, as in apiRequest() */
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
		if (res.status === 401 && sentToken && sentToken === authToken)
			setAuth(null, null);
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
		noteHostGating(health && health.auth_gating_active);

		/* Back after an absence: the daemon may have restarted into a
		 * different build with a fresh uptime, so re-ask rather than keep
		 * showing what was true before it went away. */
		if (consecutiveHealthFailures > 0)
			refreshStatusBar();
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
		/* Attention, not alarm (owner request): when a newer build is
		 * running than this page was loaded from, the version text stays
		 * gray and shines (see .status-version-skew) rather than turning
		 * accent + popping a verbose tooltip. The text still shows the
		 * running "Cix <ver> - slot <x>"; the tooltip is a one-liner. */
		statusVersionEl.classList.toggle("status-version-skew", versionIsSkewed());
		statusVersionEl.title = versionIsSkewed()
		    ? "A newer build is running — click to reload the dashboard."
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
 * The status bar's facts: the running build and slot, host and daemon
 * uptime, and load average. The two uptimes are separate on purpose:
 * they diverge after a control-plane restart that was not a reboot,
 * and "the box has been up for days but cixd restarted four minutes
 * ago" is exactly the thing worth noticing.
 *
 * Fetched together and shown together: the bar is either the whole
 * truth from one moment or the last whole truth it had, never half of
 * one. It used to be two fetchers on two schedules, so after a daemon
 * restart it could show an uptime with no version for a minute, or a
 * version beside an uptime still counting from before the reboot.
 * Asked when a session begins, when the daemon comes back after being
 * away, and once a minute for the load average.
 */
async function refreshStatusBar() {
	let b, s;

	try {
		[b, s] = await Promise.all([
			apiRequest("GET", CIX_API.getSystemBoot()),
			apiRequest("GET", CIX_API.getSystemStats()),
		]);
	} catch (e) {
		/* Unreachable is already said by the LEDs, and a refused read
		 * is retried when the session begins. The last whole state
		 * stands rather than blanking, since it is still the last true
		 * thing anyone told us. */
		return;
	}
	const up = s.uptime || {};
	const load = s.load || {};

	statusVersion = { version: b.build_version || "unknown", slot: b.slot || null };
	if (loadedVersion === null)
		loadedVersion = statusVersion.version;
	statusMeta.title = "Cix " + statusVersion.version +
	                   ", built " + (b.build_time || "unknown") +
	                   (statusVersion.slot !== null ? ", running from slot " + statusVersion.slot : "") +
	                   " on kernel " + (b.kernel_version || "unknown");
	statusMetaBase = {
		fetchedAt: Date.now(),
		host: up.host_seconds || 0,
		daemon: up.daemon_seconds || 0,
		load1: load.load1 || 0,
		load5: load.load5 || 0,
		load15: load.load15 || 0,
	};
	renderStatusMeta();
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
	"ldap-servers": "view-ldap",
	"ldap-groups": "view-identity",
	"ldap-users": "view-identity",
	"ldap-config": "view-ldap",
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
	pipeline: "view-integration",
	"pipeline-errors": "view-integration",
	"site": "view-configuration",
	"daemon-config": "view-control-plane",
	"host-swap": "view-configuration",
	"rolling-restart": "view-deployment",
	"build-overview": "view-integration",
	"pkg-build-config": "view-integration",
	"hostauth-sessions": "view-identity",
	"hostauth-permissions": "view-identity",
	"host-stats": "view-health",
	processes: "view-health",
	"syslog-targets": "view-syslog-targets",
	"tls-throttle": "view-control-plane",
	"control-plane-reservation": "view-control-plane",
	"boot-console": "view-bootloader",
	esp: "view-bootloader",
	"signing-keys": "view-bootloader",
	"kernel-policy": "view-kernel",
	logs: "view-health",
	kmsg: "view-health",
	"server-health": "view-server-health",
	stalls: "view-control-plane",
	schedules: "view-control-plane",
	volumes: "view-storage",
	/* Sources & Sync and Cache & Artifacts became tabs on the Catalogue
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
	/* An ALIAS, not a tab: Factory Reset stopped being a destination
	 * (#592, the board's call B). The route is kept so an existing
	 * bookmark lands on the page where the action now lives -- with
	 * no tab of that name, selectTabFor() leaves the page on its
	 * first tab and the danger zone is below it. Same shape as
	 * `recipes` above. */
	"factory-reset": "view-configuration",
	"storage-placement": "view-storage",
	"backup": "view-storage",
	"update": "view-deployment",
	"reconcile": "view-deployment",
	iso: "view-deployment",
	"running-config": "view-configuration",
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
		/* A container's page holds its own live read (its stats); the
		 * other pages' end here, as they do when leaving them below. */
		stopHostStatsPolling();
		if (netPortsName !== null)
			stopNetPortsPolling();
		stopLiveRead("kmsg");
		stopLiveRead("stalls");
		boSyncStats(new Set());
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
		/* The page, not the route: Host stats is one tab of a page,
		 * and stopping on any other tab's route cleared the charts'
		 * history and restarted their read on every render. */
		if (!onPageOf("host-stats"))
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
		/*
		 * ADR-0339 release B: a page's own READS are refreshers
		 * (VIEW_REFRESHERS), run on arrival and then only when the box
		 * announces something they read. They used to be called from
		 * here, which runs on every change whatever moved -- a site
		 * change re-read the build-log list. What stays here renders
		 * from what is already read.
		 */
		/* Streams, never announced: live while their page is open. */
		if (onPageOf("kmsg"))
			startKmsgLive();
		else
			stopLiveRead("kmsg");
		if (onPageOf("stalls"))
			startStallsLive();
		else
			stopLiveRead("stalls");
		/* #431: the build cards' charts hold a read per build; the page
		 * starts them as it draws its cards, and they end with it. */
		if (!onPageOf("build-overview"))
			boSyncStats(new Set());
		if (onPageOf("devices"))
			renderDevices();
		if (onPageOf("storage"))
			renderDisks();
		if (onPageOf("storage-placement"))
			renderAllStoragePlacements();
		if (onPageOf("backup"))
			renderBackupConfig();

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
			renderPackagesView(null);
			renderImages(cache.images);
		}
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
	/* ADR-0339: the page arrived at reads what it shows, once, live or
	 * not -- its reads are refreshers now (release B), so nothing else
	 * would read them before the next change or the next 5 s poll. A
	 * change while it is open is the stream's to report. */
	runRefreshers(refreshersForView(CATEGORY_VIEWS[parseHash().category]))
		.then(renderCurrentView);
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

/*
 * A GROUP LABEL THAT GOES NOWHERE, because it names a group and not a
 * destination (#592, the accepted IA board).
 *
 * All three of this tree's groups used to be links as well: Software
 * went to the Pipeline, Services to Server Health, Host to host stats.
 * So clicking the word "Services" landed an operator on a page about
 * none of the six things underneath it -- the tree's own rule, written
 * in its own comment, is that TABS LIVE ON THE PAGE AND THE TREE'S
 * CHILDREN ARE THE LIVE THINGS, and a group that is secretly one of
 * its children's pages breaks it.
 *
 * A real `<button>` rather than a span with a handler: it is operated
 * by keyboard, and `role`/`onClick` on a div is the accessibility
 * mistake the design guidelines name explicitly.
 */
function treeGroupLabel(text, className, icon) {
	const b = document.createElement("button");

	b.type = "button";
	b.className = className + " tree-group";
	if (icon)
		b.appendChild(treeIcon(icon, undefined));
	b.appendChild(treeLabel(text));
	return b;
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
	/* A group with no hash routes nowhere, so it must not claim a
	 * route either -- registering `undefined` here would make
	 * findCurrentNodeId() able to pick it. */
	if (item.hash !== undefined)
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
		item.hash === undefined
			? treeGroupLabel(item.label, "tree-category", item.icon)
			: item.status !== undefined
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

		/* A group label navigates nowhere, so ONE click is the whole
		 * gesture: the paragraph above argues against swallowing a
		 * link's first click to wait for a second, and with no link
		 * there is nothing to swallow. */
		if (item.hash === undefined)
			anchor.addEventListener("click", (event) => {
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
			 * Software is a group because its children are its
			 * STAGES, not instances of a thing. All three groups are
			 * like that -- Software, Services and Host each hold
			 * subjects, while Containers, Networks and Storage hold the
			 * things that exist on the box. That is deliberate: an
			 * operator asking "where did this stop?" is asking about the
			 * stage, so the stages are what the tree should offer.
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
			 *
			 * ...and the hash is GONE now (#592, the accepted IA
			 * board). It was "pipeline", so clicking the word
			 * "Software" landed on the build pipeline -- one of the
			 * subjects underneath it, wearing the group's name. The
			 * pipeline is reachable as what it is, a Build tab -- and
			 * since 2026-10-17 that is true: the commit that removed the
			 * hash wrote this sentence without adding the tab, so the
			 * stage board was reachable only by typing #pipeline for a
			 * week. Build lands on it now, and test_web_tree fails any
			 * view nothing reaches.
			 */
			label: "Software",
			group: true,
			icon: "software",
			children: [
				{ label: "Catalogue", hash: "pkg-recipes", icon: "recipes" },
				{ label: "Build", hash: "pipeline", icon: "packages" },
				{ label: "Deployment", hash: "update", icon: "update" },
			],
		},
		{
			/*
			 * This group used to be selectable and land on Server
			 * Health, argued at the time as "the overview for this
			 * group rather than a sibling of the services it
			 * summarises". The accepted IA board (#592) overrules
			 * that: an operator clicking "Services" got a page about
			 * none of the six things underneath, and the fix is not
			 * to pick a better destination but to have none -- the
			 * group expands, and Server Health takes its place among the
			 * children it was always a sibling of. It was labelled
			 * plain "Health" for a week, which is also Host's child for
			 * a different page; test_web_tree now refuses a tree label
			 * used twice.
			 */
			label: "Services",
			group: true,
			icon: "pki",
			children: [
				{ label: "PKI", hash: "pki-ca", icon: "pki" },
				{ label: "DNS", hash: "dns-records", icon: "dns" },
				/* LDAP the SERVICE: its servers and how containers
				 * are handed them. The board's call A had moved all
				 * of LDAP to Host > Identity; on 2026-10-17 the owner
				 * agreed to split it, because a group that holds DNS,
				 * NTP and DHCP and not the directory server reads as
				 * an item removed from one heading and not the
				 * others. Who exists in the directory -- users and
				 * groups -- stays on Identity. */
				{ label: "LDAP", hash: "ldap-servers", icon: "ldap" },
				{ label: "NTP", hash: "ntp-config", icon: "ntp" },
				{ label: "DHCP", hash: "dhcp-servers", icon: "dhcp" },
				{ label: "Syslog", hash: "syslog-targets", icon: "syslog" },
				{ label: "Server Health", hash: "server-health", icon: "system" },
			],
		},
		{
			/*
			 * This used to be selectable too -- "clicking Host opens
			 * the host's own page", which was host stats, one of the
			 * nine tabs that page carries. Same correction as the
			 * other two groups (#592): the group expands, and Health
			 * is a child like the rest.
			 *
			 * Health is where host stats, processes and BOTH log
			 * surfaces belong. The last of those is the board's
			 * fourth defect: Logs sat on Host and kmsg on Kernel.
			 * Measured before moving it, because a board states a
			 * cause the same way an issue does: GET /system/logs is
			 * the CONSOLIDATED store and its `source` enum really is
			 * [kernel, cixd, audit, container] (ADR-0070), so the
			 * kernel's lines were already on the Host page and the
			 * second page was the invention. GET /system/kmsg is
			 * still its own endpoint though -- the LIVE ring buffer
			 * rather than the persisted store -- so it keeps its own
			 * tab beside Log Store rather than collapsing into a
			 * filter of it.
			 */
			label: "Host",
			group: true,
			icon: "system",
			children: [
				{ label: "Health", hash: "host-stats", icon: "system" },
				{ label: "Identity", hash: "hostauth-sessions", icon: "ldap" },
				{ label: "Configuration", hash: "site", icon: "system" },
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

/*
 * Generated (#592) -- the dashboard's headline table, and the whole of
 * it is a link, a badge, a field, one derived cell and a button.
 *
 * The IP column is derived because `networks` is an array of OBJECTS:
 * `fieldText` has nothing useful to say about one, and `name:ip` joined
 * by commas is a judgement about what an operator wants at a glance.
 *
 * `containerStatusKind()` is what maps a container's own status word
 * to one of the four badge kinds, and it stays in app.js because that
 * mapping is this domain's knowledge -- `statusBadge` takes a kind,
 * never a domain's state (#592, where `exited` was rendering grey in
 * this very table while the tree showed it red).
 */
function renderContainers(containers) {
	renderTable(document.getElementById("containers"), "Container", containers, [
		{ field: "name", link: (c) => "#containers/" + encodeURIComponent(c.name) },
		{ field: "status", badge: (s) => containerStatusKind(s) },
		"image",
		{ label: "IP", text: (c) => (c.networks && c.networks.length > 0
			? c.networks.map((n) => n.name + ":" + n.ip).join(", ")
			: FIELD_ABSENT) },
		{ label: "", actions: (c) => {
			const rm = document.createElement("button");

			rm.textContent = "Remove";
			rm.className = "button-danger";
			gateAction(rm, "deleteContainer");
			rm.addEventListener("click", () => removeContainer(c.name));
			return [rm];
		} },
	], { op: "listContainers", what: "containers", empty: "No containers" });
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

/*
 * The one remove for a container, an image and a package each: the
 * list's button, the detail page's and the tree's right-click menu all
 * call these. They deleted on the first click, with no confirm and no
 * word on success, until 2026-10-17 -- the guidelines' four-part rule
 * for a destructive action, broken by the three most destructive
 * actions in the dashboard. The confirm lives here, once, so no surface
 * can skip it; each question says what the contract says goes.
 */
async function removeContainer(name) {
	if (!confirm("Remove container \"" + name + "\"? It is stopped and deleted with its own "
	             + "files. Volumes it mounts are kept."))
		return;
	try {
		await apiRequest("DELETE", CIX_API.deleteContainer(name));
		showStatus("Removed container " + name);
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

/* #447: net.* sysctls on an existing container. The daemon answers with
 * `applies`: "now" when it wrote the value into the running container,
 * "on next start" otherwise -- said in the status message rather than
 * guessed here. */
function sysctlAppliesText(result) {
	return result && result.applies === "now" ? "applied now" : "applies at the next start";
}

async function unsetContainerSysctl(name, key) {
	try {
		await apiRequest("PUT", CIX_API.setContainerSysctls(name), { [key]: null });

		showStatus("Unset " + key + " on " + name + " -- no longer set from the next start; " +
			"a running container keeps its current value until then");
		await refreshContainers();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to unset " + key + " on " + name + ": " + e.message, true);
	}
}

document.getElementById("cd-sysctl-set").addEventListener("click", () => {
	document.getElementById("container-sysctl-form").reset();
	openModal("container-sysctl-form", "Set a sysctl on " + currentContainerDetailName);
});

document.getElementById("container-sysctl-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = currentContainerDetailName;
	const key = document.getElementById("csf-key").value.trim();
	const value = document.getElementById("csf-value").value;

	try {
		const result = await apiRequest("PUT", CIX_API.setContainerSysctls(name), { [key]: value });

		closeModal();
		showStatus("Set " + key + "=" + value + " on " + name + " -- " + sysctlAppliesText(result));
		await refreshContainers();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to set " + key + " on " + name + ": " + e.message, true);
	}
});

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

function summarizeServices(c) {
	const svcs = Array.isArray(c.services) ? c.services : [];

	if (svcs.length === 0)
		return "-";
	return svcs.map((s) => s.name + ":" + (s.state || "?")).join(", ");
}

function formatServiceExit(s) {
	if (!s.last_exit)
		return FIELD_ABSENT;
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
/*
 * Generated (#592). It reads `Service` correctly only since the same
 * change: ten of its properties and two arrays' `items` were written as
 * YAML flow mappings, which apigen recorded with no type, so `pid` and
 * `restarts` reached the dashboard as strings and `type` and `on_exit`
 * without their enums. They are block form now and apigen refuses the
 * flow form.
 *
 * `state` is a badge, through serviceStateKind() -- whose comment says
 * why a service's "exited" is not a container's. `pid` and `restarts`
 * are plain fields: null renders "—" and 0 renders "0", which is what
 * the hand-written `s.pid ? ... : "-"` and `s.restarts || 0` did by
 * hand. Last exit and Command stay derived -- one is an object, the
 * other is argv, joined by spaces where the schema's join is ", ".
 *
 * The empty row also gains the `empty` class it never had, so a
 * container with no services no longer renders that sentence as an
 * unstyled data cell.
 */
function renderServicesTable(c) {
	renderTable(document.querySelector("#cd-config-services"), "Service",
	    Array.isArray(c.services) ? c.services : [], [
		{ field: "name", label: "Service" },
		"type",
		{ field: "state", badge: (st) => serviceStateKind(st) },
		{ field: "pid", label: "PID" },
		"restarts",
		{ label: "Last exit", text: (s) => formatServiceExit(s) },
		{ label: "Command", text: (s) => (s.cmd || []).join(" ") },
		{ label: "", actions: (s) => ["start", "stop", "restart"].map((action) => {
			const b = document.createElement("button");

			b.type = "button";
			b.className = "button-small";
			b.textContent = action;
			b.disabled = c.status !== "running";
			gateAction(b, action === "start" ? "startContainerService"
			              : action === "stop" ? "stopContainerService"
			              : "restartContainerService");
			b.addEventListener("click", () => containerServiceAction(c.name, s.name, action));
			return b;
		}) },
	], {
		op: null,
		key: "container-detail-services",
		/* The buttons are enabled by the CONTAINER's status, which
		 * is not in these rows -- so it has to be in the compared
		 * signature, or a container that stops while its services do
		 * not change keeps live-looking buttons that cannot work. */
		extra: c.name + "/" + c.status,
		what: "services",
		empty: "This container declares no service.",
	});
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
		 * Nothing to attach to. The console endpoint takes only a
		 * declared console's name, and answers 409 for a container that
		 * declares none (try_console_upgrade() in main.c). This used to
		 * keep the terminal on screen and point at a "Run" box for an
		 * explicit command, but the console tab has no such control and
		 * the daemon no such parameter (measured 2026-10-01, cix#557).
		 * The words below are the daemon's own.
		 */
		output.hidden = true;
		status.textContent =
			"This container declares no console, so there is nothing to attach to \u2014 add one to its definition.";
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
	/*
	 * #544: a refused WebSocket upgrade reaches the page only as a closed
	 * socket -- the browser exposes no status -- so a session without
	 * containers:console would see a console that just dies. Ask first,
	 * and say why instead of connecting.
	 */
	if (!sessionMay("consoleContainer")) {
		statusEl.textContent = sessionRefusal("consoleContainer");
		return;
	}
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
	/*
	 * #541: a browser WebSocket cannot set an Authorization header, so
	 * the session rides as a subprotocol: "cix" plus "cix.bearer.<token>".
	 * The daemon reads the token from the second and selects the first
	 * in its 101, which the browser requires to be one it offered. With
	 * no session there is nothing to offer, and an ungated host answers
	 * as before.
	 */
	const wsProtocols = authToken ? ["cix", "cix.bearer." + authToken] : [];
	const ws = new WebSocket(proto + "//" + location.host + CIX_API.consoleContainer(name) + query,
		wsProtocols);

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

let statsContainerName = null; /* the container whose live read the tab holds, or null */
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
	if (!owner) return FIELD_ABSENT;
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
	if (statsContainerName !== null)
		stopLiveRead("container-stats");
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

	/* #474: usage.bytes is null until the first background measurement
	 * lands. Zero keeps the series well-formed and self-corrects on the
	 * next poll; the label below says the truth rather than showing a
	 * number nobody measured. */
	const diskValues = h.map((s) => (s.diskBytes === null || s.diskBytes === undefined ? 0 : s.diskBytes));

	drawChart(document.getElementById("cd-stats-disk"), [{ values: diskValues, color: seriesColor(1) }], {
		times: gaugeTimes,
		formatY: formatBytes,
	});
	const lastDisk = h[h.length - 1].diskBytes;
	const lastDiskSource = h[h.length - 1].diskSource;

	/*
	 * #475: this said "(overlay diff)" for every figure, and on the
	 * btrfs substrate a container's writable tree is a subvolume seeded
	 * from its image rather than an upperdir over it -- so the number
	 * was frequently the whole rootfs presented as a per-container
	 * diff, and a 30 MB service container read as having written 30 MB.
	 * The two sources answer different questions, so the label names
	 * the one that answered instead of asserting the flattering one.
	 */
	const diskWhat =
		lastDiskSource === "qgroup"
			? " written beyond image"
			: lastDiskSource === "walk"
				? " (whole writable tree, incl. image)"
				: "";

	document.getElementById("cd-stats-disk-label").textContent =
		lastDisk === null || lastDisk === undefined
			? "measuring…"
			: formatBytes(lastDisk) + diskWhat;

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

/* One sample of GET /v1/containers/{name}/stats, pushed or polled
 * (startLiveRead()). A container that has just exited sends no sample
 * (a non-200 is never sent), and leaving the tab resolves it. */
function ingestContainerStats(name, stats) {
	if (statsContainerName !== name)
		return; /* the tab moved on to a different container */

	const netRx = (stats.networks || []).reduce((sum, n) => sum + n.rx_bytes, 0);
	const netTx = (stats.networks || []).reduce((sum, n) => sum + n.tx_bytes, 0);

	statsHistory.push({
		t: Date.now(),
		cpuUsageUsec: stats.cpu.usage_usec,
		memCurrent: stats.memory.current,
		memMax: stats.memory.max,
		/*
		 * disk.usage (ADR-0301), not the old flat disk.upper_*. No
		 * guard for a missing `usage` here, unlike cixctl's: the
		 * dashboard is served BY the daemon it queries, so the two are
		 * always the same build -- a locally built CLI is the one that
		 * can run ahead (#476).
		 */
		diskBytes: stats.disk.usage.bytes,
		/* #475: the number alone does not say what it measured, and the
		 * label below used to assert "overlay diff" for a figure that
		 * was often the whole writable tree. Carried through so the
		 * label reads what the API reported rather than re-deriving a
		 * claim about it. */
		diskSource: stats.disk.usage.source,
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
	if (statsContainerName === name)
		return; /* already live on this exact container -- a re-render of
		         * the same detail view must not reset history */

	stopStatsPolling();
	statsContainerName = name;
	startLiveRead("container-stats", CIX_API.getContainerStats(name),
	              (stats) => ingestContainerStats(name, stats));
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

let hostStatsLive = false; /* the page holds its live read (ADR-0339 phase 2) */
let hostStatsHistory = [];

function stopHostStatsPolling() {
	if (hostStatsLive)
		stopLiveRead("host-stats");
	hostStatsLive = false;
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

/* One sample of GET /v1/system/stats, pushed or polled (startLiveRead()). */
function ingestHostStats(stats) {
	if (!hostStatsLive)
		return; /* the page moved on before this sample arrived */

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
	if (hostStatsLive)
		return; /* already live -- a re-render of the same page must not
		         * reset history */

	hostStatsLive = true;
	startLiveRead("host-stats", CIX_API.getSystemStats(), ingestHostStats);
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
 * content to keep (#432).
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
 * would be skipped and stay empty.
 *
 * dataChanged() records the signature as a side effect, so this must
 * be called once per render attempt and its answer acted on.
 */
function unchangedAndRendered(tbody, key, data)
{
	const changed = dataChanged(key, data);

	/* firstElementChild rather than querySelector("tr"): the same guard
	 * has to serve panels that are not tables. Boot Console, Control
	 * Plane Reservation, Signing Keys and the release-key box render
	 * <p>/<pre> into a div, and Backup rebuilds a <select> of <option>s
	 * -- a tr-only check would answer "never rendered" for all of them
	 * and the guard would never fire. Identical answer for every tbody
	 * that already used it. */
	return !changed && tbody !== null && tbody.firstElementChild !== null;
}

/*
 * WHAT A GUARDED LIST COMPARES ON, once an empty list has three states.
 *
 * `unchangedAndRendered()` asks whether the DATA changed, which was the
 * whole truth while an empty list rendered one fixed sentence. It is
 * not any more: `[]` renders "Reading…", a refusal, or the authored
 * "No <things>" depending on whether the read has happened, and the
 * data is `[]` in all three. So a panel that rendered "Could not load
 * routes" would keep saying it after the read succeeded and returned
 * none -- the guard seeing `[]` both times and skipping the repaint.
 *
 * Found by driving the four outcomes in a browser rather than by
 * reading: all four printed the same sentence, because only the first
 * one ever painted.
 *
 * So when a list is empty the thing being rendered IS the sentence,
 * and the sentence is what the signature must carry.
 */
function listSignature(rows, opId, what, normalText) {
	return rows.length === 0
	    ? ["empty", emptyStateText(opId, what, normalText)]
	    : rows;
}

/*
 * "Reading" belongs to a panel that has never had content, not to every
 * refresh of one that has -- blanking a populated table before a fetch
 * is what once made Processes flash empty twice a second. That is now
 * how renderTable behaves by construction: it paints only what a
 * completed read produced, so nothing here clears the table first.
 */
async function refreshProcesses() {
	try {
		renderProcessesTable(await apiRequest("GET", CIX_API.listSystemProcesses()));
	} catch (e) {
		/* An empty list, so listState() can say what failed. Not when
		 * there are rows on screen, though: a transient failure should
		 * not blank a process list someone is reading, which is the one
		 * thing the hand-written catch here got right. */
		const body = document.querySelector("#processes tbody");

		if (body === null || body.querySelector("td:not(.empty)") === null)
			renderProcessesTable([]);
	}
}

/*
 * Generated (#592), from `HostProcess`. The container column links to
 * the container a process belongs to and is plain "—" for a host
 * process -- `link` returning nothing is plain text. The command keeps
 * its one-line monospace cell, with the whole command as the hover.
 *
 * It was the third copy of the list states: showReadingIfEmpty() drew
 * "Reading…" by innerHTML, and the refresher's catch wrote "Failed to
 * load." the same way -- a failure with no reason in it, where
 * refusalText() names the missing permission. Both are listState()'s
 * now, and the helper had no other caller and is gone.
 */
function renderProcessesTable(procs) {
	renderTable(document.getElementById("processes"), "HostProcess", procs || [], [
		{ field: "pid", label: "PID" },
		{ field: "ppid", label: "PPID" },
		{ field: "user_id", label: "UID" },
		{ field: "group_id", label: "GID" },
		{ field: "container", label: "Container",
		  link: (p) => (p.container ? "#containers/" + encodeURIComponent(p.container) : null) },
		{ field: "command_line", label: "Command", cls: "processes-cmdline",
		  title: (p) => p.command_line },
		{ label: "", actions: (p) => {
			const kill = document.createElement("button");

			kill.textContent = "Kill";
			kill.className = "button-danger button-small";
			gateAction(kill, "killSystemProcess");
			kill.addEventListener("click", () => killProcess(p.pid, p.command_line));
			return [kill];
		} },
	], { op: "listSystemProcesses", what: "processes", empty: "No processes." });
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
		 * holding whatever it was built with -- "Reading…", for a page
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
	statusEl.textContent = "Reading\u2026";
	textEl.hidden = true;
	try {
		const content = await loadContainerRecipeContent(name);

		/* Moved on to another container while this was in flight. */
		if (currentContainerDetailName !== name)
			return;

		if (!content) {
			statusEl.textContent = "This container has a recipe, but it is empty.";
			return;
		}
		statusEl.textContent =
			"Applied from this recipe. Secret tokens are shown unsubstituted \u2014 no value reaches this page.";
		textEl.textContent = content;
		textEl.hidden = false;
	} catch (err) {
		if (currentContainerDetailName !== name)
			return;
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
	const table = document.getElementById("cd-packages");
	const c = cache.containers.find((x) => x.name === name);

	if (statusEl === null || table === null)
		return;
	/*
	 * The status line above the table explains every case in which
	 * there is no manifest to show, and it explains each more exactly
	 * than a list's four states can -- no image, no recorded version, a
	 * version produced before per-version manifests existed. So the
	 * table appears only when a manifest was read, and is hidden rather
	 * than left as a header over nothing in every other case.
	 */
	table.hidden = true;
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
	statusEl.textContent = "Reading\u2026";
	try {
		const d = await apiRequest("GET",
		                           CIX_API.getImageVersionManifest(c.image, c.image_version));

		/* The operator may have moved to another container while this
		 * was in flight; its answer must not overwrite that one's. */
		if (currentContainerDetailName !== name)
			return;
		statusEl.textContent = "Installed into image " + c.image + " at version "
			+ c.image_version + " \u2014 the version this container is pinned to and runs from.";
		/* Generated (#592), from `ImageVersionPackage`, promoted out of
		 * this endpoint's inline items for it. */
		renderTable(table, "ImageVersionPackage",
		    Array.isArray(d.manifest) ? d.manifest : [], [
			"package",
			"version",
		], {
			op: "getImageVersionManifest",
			key: "container-detail-packages",
			extra: c.image + "@" + c.image_version,
			what: "this image version's packages",
			empty: "This image version holds no packages.",
		});
		table.hidden = false;
	} catch (err) {
		if (currentContainerDetailName !== name)
			return;
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
	const switched = currentContainerDetailName !== name;

	currentContainerDetailName = name;

	/*
	 * Configuration and Packages load when their tab is clicked, not on
	 * the two-second poll (see the tab handler). So moving to another
	 * container while one of them is open left the previous container's
	 * recipe or package list on screen under the new container's name.
	 * A change of container is the other moment they must load.
	 */
	if (switched && c) {
		if (document.getElementById("cd-tab-config").classList.contains("active"))
			loadContainerRecipe(name);
		if (document.getElementById("cd-tab-packages").classList.contains("active"))
			loadContainerPackages(name);
	}

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
	/* The image links to its own page (#592): an image is a Software
	 * subject with no tree leaf of its own, so the container that runs
	 * it is the natural way in, and this was plain text. */
	{
		const imageRow = fieldBlock("Image", c.image ? "" : "-");

		if (c.image)
			imageRow.lastChild.appendChild(
			    treeLink("#images/" + encodeURIComponent(c.image), c.image, ""));
		fields.appendChild(imageRow);
	}
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
	/*
	 * Generated (#592), both with `op: null`: each is a field of the
	 * container already in hand, so each has one state. Both also gain
	 * the header row they never had -- these were bare <tbody>s.
	 *
	 * A device links to the Devices page only when it has an exact name
	 * mapping, and its text then carries the name -- `link` returning
	 * nothing renders plain text, so an unmapped device is not a link
	 * to nowhere.
	 */
	{
		const mappedNames = exactMappedNames();

		renderTable(document.querySelector("#cd-devices"), "ContainerDeviceAttachment",
		    c.devices || [], [
			{ label: "Device",
			  text: (d) => (mappedNames[d.id] ? d.id + " (" + mappedNames[d.id] + ")" : d.id),
			  link: (d) => (mappedNames[d.id] ? "#devices" : null) },
			{ field: "dev_path", label: "Path" },
		], {
			op: null,
			key: "container-detail-devices",
			extra: c.name,
			what: "devices",
			empty: "No devices granted",
		});
	}
	/* A list of names, with no schema behind its rows -- one state, and
	 * nothing for a renderer to derive, so the primitive is right. */
	simpleTableRows(
		document.querySelector("#cd-interfaces tbody"),
		(c.interfaces || []).map((ifname) => [ifname]),
		1,
		"No host interfaces attached"
	);
	renderTable(document.querySelector("#cd-networks"), "ContainerNetworkAttachment",
	    c.networks || [], [
		{ field: "name", label: "Network",
		  link: (n) => "#networks/" + encodeURIComponent(n.name) },
		{ field: "ip", label: "Address" },
	], {
		op: null,
		key: "container-detail-networks",
		extra: c.name,
		what: "networks",
		empty: "Not attached to any network",
	});

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

		/* Its own table, because fieldBlock renders with textContent and
		 * a newline-joined list would collapse to one run-on line.
		 * Generated (#592) from `ContainerConsole`, promoted out of
		 * Container.consoles' inline items for exactly this. `cmd` is
		 * derived because it is argv: the schema's string-array join is
		 * ", ", and a command line is joined by spaces. */
		renderTable(document.querySelector("#cd-config-consoles"), "ContainerConsole",
		    c.consoles || [], [
			{ field: "name", label: "Console" },
			{ label: "Runs", text: (x) => (x.cmd || []).join(" ") },
		], {
			op: null,
			key: "container-detail-consoles",
			extra: c.name,
			what: "consoles",
			empty: "This container declares no console.",
		});
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
	/* #451: a dash here read as "nothing configured" when it means the
	 * container cannot resolve a name at all -- no /etc/resolv.conf is
	 * staged when the field is empty. The operator-visible symptom was
	 * a jump box where `ssh somehost` failed for no stated reason. */
	optionsFields.appendChild(fieldBlock("DNS servers",
		(c.dns_servers || []).join(", ") || "none - cannot resolve names"));
	{
		/* #447: each stored sysctl can be unset in place. Unset only
		 * drops the key from the definition -- nothing is lost, so it is
		 * a neutral compact button with no confirm. */
		const sysctlBody = document.querySelector("#cd-sysctls tbody");
		const sysctlEntries = Object.entries(c.sysctls || {});

		simpleTableRows(sysctlBody, sysctlEntries.map(([k, v]) => [k, v]), 3, "No sysctls set");
		sysctlEntries.forEach(([key], i) => {
			const cell = document.createElement("td");
			const unset = document.createElement("button");

			unset.type = "button";
			unset.className = "button-small";
			unset.textContent = "Unset";
			unset.addEventListener("click", () => unsetContainerSysctl(c.name, key));
			gateAction(unset, "setContainerSysctls");
			cell.appendChild(unset);
			sysctlBody.rows[i].appendChild(cell);
		});
	}
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
	{
		const e = containerActionEligibility(c.status);

		document.getElementById("cd-start").hidden = !e.start;
		document.getElementById("cd-pause").hidden = !e.pause;
		document.getElementById("cd-unpause").hidden = !e.unpause;
		document.getElementById("cd-stop").hidden = !e.stop;
	}
}

/* ---------- Networks ---------- */

/*
 * Generated (#592). `address` is a plain field and not a cell that
 * re-words two: the contract says it is *"the host-owned address, or
 * null when has_address is false"*, and the box agrees -- measured
 * 2026-10-11, `services` and `access` both answer `"address":null`.
 * So `fieldText` renders the absent case as the dashboard's own "—"
 * and the hand-written "(none)", a vocabulary only this table used,
 * goes with it.
 */
function renderNetworks(networks) {
	renderTable(document.getElementById("networks"), "Network", networks, [
		{ field: "name", link: (n) => "#networks/" + encodeURIComponent(n.name) },
		"subnet",
		{ field: "prefix_len", label: "Prefix" },
		"address",
		{ label: "", actions: (n) => {
			const rm = document.createElement("button");

			rm.textContent = "Remove";
			rm.className = "button-danger";
			gateAction(rm, "deleteNetwork");
			rm.addEventListener("click", () => removeNetwork(n.name));
			return [rm];
		} },
	], { op: "listNetworks", what: "networks", empty: "No networks" });
}

async function refreshNetworks() {
	const data = await apiRequest("GET", CIX_API.listNetworks());
	cache.networks = data.networks;
	renderNetworks(cache.networks);
}

async function removeNetwork(name) {
	if (!confirm("Remove network \"" + name + "\" and its bridge?"))
		return;
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

/*
 * The four DHCP tables, generated (#592).
 *
 * `dhcpFillTable()` lived here: this page's own private copy of "blank
 * the body, and if there are no rows write one cell across N columns".
 * Four tables used it, each passing its own column count, and none of
 * them could tell a refused read from an empty one. It is gone --
 * `renderTable` owns the empty state, the header and the guard, and a
 * page having its own version of any of the three is the parallel
 * implementation this epic exists to remove.
 */
function renderDhcpServers(servers) {
	renderTable(document.getElementById("dhcp-servers-table"), "DhcpServer", servers, [
		"container",
		{ field: "running", label: "Running", badge: (v) => (v ? "ok" : "unknown") },
		{ field: "resolves_leases", label: "Resolves leases" },
		{ label: "", actions: (srv) => [dhcpActionButton("Unregister",
		    "unregisterDhcpServer",
		    "Unregister DHCP server " + srv.container + "? It is dropped from every range that "
		    + "named it, and a range left with no server stops serving.", async () => {
			await apiRequest("DELETE", CIX_API.unregisterDhcpServer(srv.container));
			showStatus("Unregistered -- any range that named it has been re-split.", false);
			refreshDhcp();
		})] },
	], {
		op: "getDhcpServers",
		what: "DHCP servers",
		empty: "No DHCP server registered — register one from the Services menu",
	});
}

/*
 * One button for the three this page offers, because they differed
 * only in their words, their operation and their confirm -- and the
 * two that had no confirm were building the identical nine lines.
 * `confirm` is null for an action that needs none.
 */
function dhcpActionButton(text, opId, confirmText, run) {
	const b = document.createElement("button");

	b.type = "button";
	/* A button that asks first is a destructive one, and wears the
	 * danger style the guidelines give every destructive action. */
	b.className = confirmText !== null ? "button-danger button-small" : "button-small";
	b.textContent = text;
	gateAction(b, opId);
	b.addEventListener("click", async () => {
		if (confirmText !== null && !confirm(confirmText))
			return;
		try {
			await run();
		} catch (e) {
			showStatus("Failed to " + text.toLowerCase() + ": " + e.message, true);
		}
	});
	return b;
}

function renderDhcpRanges(networks) {
	const configured = networks.filter((n) =>
	    n.enabled || n.server_count !== 0 || n.range_start !== null);

	renderTable(document.getElementById("dhcp-ranges-table"), "DhcpNetwork", configured, [
		{ field: "network", link: (n) => "#networks/" + encodeURIComponent(n.network) },
		{ field: "enabled", label: "Serving", badge: (v) => (v ? "ok" : "unknown") },
		{ label: "Range", text: (n) => (n.range_start === null
		    ? FIELD_ABSENT : n.range_start + " – " + n.range_end) },
		{ field: "lease_seconds", label: "Lease" },
		/* The split, not the count: two servers are redundant only
		 * because their pools are disjoint, and that is the thing
		 * worth seeing. */
		{ label: "Slices", text: (n) => ((n.slices || []).length === 0
		    ? FIELD_ABSENT
		    : (n.slices || []).map((s) => s.server + " " + s.range_start +
		        "–" + s.range_end).join(", ")) },
		{ label: "", actions: (n) => [
			dhcpActionButton("Edit", "setDhcpNetwork", null,
			    async () => editDhcpRange(n.network)),
			/*
			 * Remove deletes the configuration outright, which is not
			 * what unticking "Serve DHCP" does -- that disables it and
			 * keeps the range. Until #592 the config could be created
			 * and edited here but never taken back from either channel.
			 */
			dhcpActionButton("Remove", "deleteDhcpNetwork",
			    'Remove the DHCP configuration for "' + n.network +
			    '"? The range and its server assignments are deleted. To stop ' +
			    'serving but keep them, edit the range and untick "Serve DHCP".',
			    async () => {
				await apiRequest("DELETE", CIX_API.deleteDhcpNetwork(n.network));
				showStatus("DHCP configuration removed for " + n.network + ".", false);
				refreshDhcp();
			}),
		] },
	], {
		op: "getDhcp",
		key: "dhcp-ranges",
		what: "DHCP ranges",
		empty: "No range configured — add one from the Services menu",
	});
}

function renderDhcpStatic(entries) {
	renderTable(document.getElementById("dhcp-static-table"), "DhcpReservation", entries, [
		{ field: "mac", label: "MAC" },
		{ field: "ip", label: "Address" },
		"hostname",
		{ label: "", actions: (e) => [dhcpActionButton("Remove",
		    "deleteDhcpReservation",
		    "Remove the reservation of " + e.ip + " for " + e.mac + "?", async () => {
			await apiRequest("DELETE", CIX_API.deleteDhcpReservation(e.mac));
			showStatus("Reservation removed.", false);
			refreshDhcp();
		})] },
	], {
		op: "getDhcp",
		key: "dhcp-static",
		what: "DHCP reservations",
		empty: "No reservations — every client gets an address from the range",
	});
}

/*
 * Two tables render leases -- the DHCP page's own and a network's --
 * so the guard needs a key per table, or the second would be skipped
 * whenever the two held the same rows.
 */
function renderLeaseRows(tbody, leases, key) {
	renderTable(tbody.closest("table"), "DhcpLease", leases, [
		{ field: "mac", label: "MAC" },
		{ field: "ip", label: "Address" },
		"hostname",
		{ label: "Expires", text: (l) =>
		    formatInstant(l.expires_at) },
		"server",
	], {
		op: "getDhcpLeases",
		key: key,
		what: "DHCP leases",
		empty: "No leases — nothing has asked for an address yet",
	});
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
		renderLeaseRows(document.querySelector("#dhcp-leases-table tbody"),
		    leases.leases || [], "dhcp-leases");
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

		renderLeaseRows(document.querySelector("#nd-dhcp-leases tbody"), mine,
		    "nd-dhcp-leases");
	} catch (e) {
		/* Best-effort. */
	}
}

/* ---- Creating things: from the Services menu, never from the page ---- */

/*
 * What a DHCP range may name, read from the box when the form opens
 * (#592): networks, and only REGISTERED DHCP servers, because only they
 * can serve one -- offering anything else would offer a choice the
 * daemon refuses. `newOnly` keeps to networks with no range yet, which
 * is what the menu's New Range is for; an existing range is edited from
 * its own row, where the form opens holding what it has.
 */
async function dhcpRangeChoices(newOnly) {
	const [nets, all, servers] = await Promise.all([
		apiRequest("GET", CIX_API.listNetworks()),
		apiRequest("GET", CIX_API.getDhcp()),
		apiRequest("GET", CIX_API.getDhcpServers()),
	]);
	const configured = (all.networks || []).map((n) => n.network);

	return {
		network: (nets.networks || [])
			.filter((n) => !newOnly || configured.indexOf(n.name) === -1)
			.map((n) => ({ value: n.name })),
		servers: (servers.servers || []).map((s) => ({
			value: s.container,
			label: s.container + (s.resolves_leases ? "" : " (leases will not resolve)"),
		})),
	};
}

/* Said every time rather than only when a range really changed: the
 * daemon decides that, and promising "no restart" from here would be
 * this page guessing at it. */
async function afterDhcpRangeSave() {
	showStatus("Range saved — if it changed, the servers serving it are being restarted.",
	           false);
	await refreshDhcp();
}

async function editDhcpRange(network) {
	const cfg = dhcpCache.networks.find((n) => n.network === network);
	let choices;

	try {
		choices = await dhcpRangeChoices(false);
	} catch (e) {
		showStatus("Could not read the DHCP servers: " + e.message, true);
		return;
	}
	openForm("setDhcpNetwork", {
		title: "DHCP range for " + network,
		submit: "Save",
		args: [network],
		values: cfg || {},
		choices: { servers: choices.servers },
		after: afterDhcpRangeSave,
		failed: "Failed to save the range for " + network,
	});
}


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

let netPortsName = null; /* the network whose live read the page holds, or null */
let netPortsHistory = [];

function stopNetPortsPolling() {
	if (netPortsName !== null)
		stopLiveRead("net-ports");
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

/* One sample of GET /v1/networks/{name}/ports, pushed or polled
 * (startLiveRead()). A failed read sends nothing, so the last drawn
 * panel stays put rather than blanking. */
function ingestNetPorts(name, data) {
	const byIf = {};

	if (netPortsName !== name)
		return; /* the page moved on to another network */
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
}

function startNetPortsPolling(name) {
	if (netPortsName === name)
		return; /* a re-render of the same network must not reset the
		         * window it has been building */
	stopNetPortsPolling();
	netPortsName = name;
	startLiveRead("net-ports", CIX_API.getNetworkPorts(name), (data) => ingestNetPorts(name, data));
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

	/*
	 * GENERATED, and `renderDetailRows`' first caller (#592 story 3,
	 * the half that had none). Schema order, every field, labelled by
	 * the contract's titles -- so this page now shows what it never
	 * did: whether this is the management network, and the
	 * auto-allocation pool that #137 exists to let an operator set on a
	 * bridged LAN. The daemon had always returned the pool; the
	 * contract had never declared it, which is why a page built from
	 * the contract was the thing that found the gap.
	 *
	 * `interfaces` is omitted because it has its own tab below, and
	 * nothing else is: the page names this network nowhere else in its
	 * own content, so `name` stays.
	 *
	 * What is lost is the "(none -- pure L2)" gloss on a missing
	 * address. "Has address: no" now states the same fact, in the
	 * vocabulary every other boolean on this dashboard uses.
	 */
	renderDetailRows(fields, "Network", n, { omit: ["interfaces"] });

	/* Containers on this network -- the reverse of what a container's
	 * own Hardware tab already shows (network name + IP, from its own
	 * `networks[]`). A client-side cross-reference against the cached
	 * container list; GET /networks/{name} has no per-container view of
	 * its own and needs none (#804). Generated (#592): the status is a
	 * badge now, as it is in the containers list itself, where it was a
	 * bare word here. */
	const attachment = (c) => (c.networks || []).find((att) => att.name === n.name);

	renderTable(document.querySelector("#nd-containers"), "Container",
	    cache.containers.filter((c) => attachment(c) !== undefined), [
		{ field: "name", label: "Container",
		  link: (c) => "#containers/" + encodeURIComponent(c.name) },
		{ label: "IP", text: (c) => attachment(c).ip },
		{ field: "status", badge: (s) => containerStatusKind(s) },
	], {
		op: "listContainers",
		key: "network-detail-containers",
		extra: n.name,
		what: "containers",
		empty: "No containers on this network",
	});

	/* A field of the network already in hand, so it has one state: it
	 * was never separately read (`op: null`). It also gains the header
	 * row it never had -- this table was a bare <tbody>. */
	renderTable(document.querySelector("#nd-interfaces"), "NetworkAttachedInterface",
	    n.interfaces || [], [
		{ field: "ifname", label: "Interface" },
		{ label: "VLAN", text: (att) => (att.vlan_id ? "VLAN " + att.vlan_id : "untagged") },
		{ label: "", actions: (att) => {
			const rm = document.createElement("button");

			rm.textContent = "Detach";
			rm.className = "button-danger button-small";
			gateAction(rm, "detachNetworkInterface");
			rm.addEventListener("click", () => detachInterface(n.name, att.ifname));
			return [rm];
		} },
	], {
		op: null,
		key: "network-detail-interfaces",
		extra: n.name,
		what: "interfaces",
		empty: "No interfaces attached",
	});

	/* The host interfaces free to attach, by bare kernel name. Rebuilt
	 * only when they change and never while one is being chosen -- the
	 * hand-built select was refilled on every render and lost a choice
	 * the operator had just made (#592). */
	syncForm(document.getElementById("nd-attach-form"), "attachNetworkInterface", {
		args: [n.name],
		choices: { ifname: cache.devices
			.filter((d) => d.bus === "net" && d.assignable)
			.map((d) => ({ value: d.id.replace(/^net:/, "") })) },
		submit: "Attach", saved: "Interface attached", clear: true,
		failed: "Failed to attach the interface to " + n.name,
		after: async () => {
			await refreshNetworks();
			await refreshDevices();
			renderCurrentView();
		},
	});

	/* The box's routing table, filtered to this network's bridge, with
	 * the same columns as the full list -- and, like the interfaces
	 * table above, the header row it never had. */
	renderTable(document.querySelector("#nd-routes"), "KernelRoute",
	    cache.routes.filter((r) => r.interface === n.name), routeColumns(), {
		op: "getSystemRoutes",
		key: "network-detail-routes",
		extra: n.name,
		what: "routes",
		empty: "No routes on this network",
	});

	document.getElementById("nd-remove").onclick = () => removeNetwork(n.name);
}

async function detachInterface(networkName, ifname) {
	if (!confirm("Detach \"" + ifname + "\" from network \"" + networkName + "\"? An interface is "
	             + "released from the bridge; a VLAN sub-interface is deleted."))
		return;
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

/*
 * The first table rendered from the contract (#592 story 3).
 *
 * What is gone from here, and is the point: the repaint guard (#432,
 * which shares a refresh tick with the build-log table), the empty
 * row and its colspan, the `<thead>` over in index.html, and the
 * derivation of each cell's text. What is left is the three things
 * that are genuinely this page's judgement -- which columns, where a
 * name links to, and what may be done to a row.
 */
function renderImages(images) {
	renderTable(document.getElementById("images"), "Image", images, [
		{ field: "name", link: (img) => "#images/" + encodeURIComponent(img.name) },
		{ label: "", actions: (img) => {
			const rm = document.createElement("button");

			rm.textContent = "Remove";
			rm.className = "button-danger button-small";
			gateAction(rm, "deleteImage");
			rm.addEventListener("click", () => removeImage(img.name));
			return [rm];
		} },
	], { op: "listImages", what: "images", empty: "No images" });
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
	if (!confirm("Remove image \"" + name + "\" and its whole root filesystem? The host refuses "
	             + "while a running container or an installed package still uses it."))
		return;
	try {
		await apiRequest("DELETE", CIX_API.deleteImage(name));
		showStatus("Removed image " + name);
		if (onPageOf("images") && parseHash().name === name)
			location.hash = "#images";
		await refreshImages();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove image " + name + ": " + e.message, true);
	}
}

/* The table half of renderImageDetail(), split out so it can be skipped
 * when nothing about it changed (#432) without skipping the handler
 * wiring and recipe panels that follow it. */
/*
 * Generated (#592). The status cell was plain text here and a badge in
 * the containers list -- the same fact, two renderings -- and it is
 * the badge in both now, through the one status map.
 *
 * This list is a field of an already-loaded image rather than its own
 * read, so its state is `listContainers`'s only in the sense that the
 * containers it filters came from there.
 */
function renderImageDetailContainers(name, containersBody, usingContainers) {
	renderTable(containersBody.closest("table"), "Container", usingContainers, [
		{ field: "name", link: (c) => "#containers/" + encodeURIComponent(c.name) },
		{ field: "status", badge: (s) => containerStatusKind(s) },
	], {
		op: "listContainers",
		key: "image-detail-containers",
		extra: name,
		what: "containers",
		empty: "No containers use this image",
	});
}

function renderImageDetail(name) {
	const usingContainers = cache.containers.filter((c) => c.image === name);
	const containersBody = document.querySelector("#imgd-containers tbody");

	/*
	 * #432, and deliberately NARROW: the rest of this function wires
	 * click handlers and fills the recipe panels from caches this
	 * signature does not cover, so guarding the whole of it would skip
	 * renders that should happen. Only the table that gets blanked is
	 * guarded. The image NAME is in the key because navigating from one
	 * image to another with an identical container list must still
	 * redraw.
	 */
	/* The guard lives inside renderTable now; the image name rides in
	 * as `extra` for the reason this comment already gave. */
	renderImageDetailContainers(name, containersBody, usingContainers);

	renderImageDetailPackages(name);
	renderImageDetailRecipes(name);
	refreshImageDetailVersioning(name);
	renderImageRecipeTab(name);

	document.getElementById("imgd-remove").onclick = () => removeImage(name);
	document.getElementById("imgd-add-recipe").onclick = () => openPkgRecipeForm("Add or update a recipe");
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

// What an image-recipe apply did, in one sentence (ADR-0320, ADR-0330).
// Shared by the image page and the recipe list, so the two cannot word it
// differently. An apply is synchronous since ADR-0209; there is no
// "started" state to report.
function imageApplySummary(name, result) {
	const declared = result && typeof result.declared === "number" ? result.declared : 0;
	const removed = result && typeof result.removed === "number" ? result.removed : 0;
	let msg = "Recipe applied for " + name + ": " + declared + (declared === 1 ? " entry" : " entries") + " declared";

	if (removed > 0)
		msg += ", " + removed + (removed === 1 ? " entry" : " entries") + " the recipe no longer lists removed";
	if (result && result.converging)
		msg += "; converging, installs follow one at a time";
	return msg;
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
		openForm("addImageRecipe", {
			title: "Edit image recipe",
			submit: "Save",
			fixed: { name: name },
			values: { content: content },
			after: afterImageRecipeSave,
			failed: "Failed to save the image recipe for " + name,
		});
	};
	document.getElementById("imgd-remove-recipe").onclick = async () => {
		if (await removeImageRecipe(name))
			renderImageRecipeTab(name);
	};
	document.getElementById("imgd-apply-recipe").onclick = async () => {
		try {
			const r = await apiRequest("POST", CIX_API.applyImageRecipe(name));

			clearStatus();
			showStatus(imageApplySummary(name, r), false);
			await refreshImageDetailVersioning(name);
		} catch (e) {
			showStatus("Failed to apply recipe for " + name + ": " + e.message, true);
		}
	};
}


/*
 * What a saved image recipe invalidates, which is all that was left of
 * this form's own submit handler once the generated form took the
 * request itself (#596).
 *
 * It takes the submitted body rather than reading a field, because on
 * an "Add" the name is whatever the operator typed and no call site
 * knows it in advance.
 *
 * `refreshVisibleView()` covers the recipes table -- it is in that
 * view's own VIEW_REFRESHERS -- but not the image detail page's recipe
 * tab, which is a tab of `images` and renders from a cache of its own.
 */
async function afterImageRecipeSave(body) {
	imageRecipeContentCache = { name: null, content: null };
	if (onPageOf("images") && parseHash().name === body.name)
		renderImageRecipeTab(body.name);
	await refreshVisibleView();
}

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
	const manifest = Array.isArray(data.manifest) ? data.manifest : [];

	/* Generated (#592). The form below is not table work and stays. */
	renderTable(document.querySelector("#imgd-manifest"), "ImageManifestEntry",
	    manifest, [
		"package",
		"mode",
		"version",
		{ label: "", actions: (e) => {
			const rm = document.createElement("button");

			rm.textContent = "Remove";
			rm.className = "button-danger";
			gateAction(rm, "unsetImageManifestEntry");
			rm.addEventListener("click", async () => {
				if (!confirm("Remove " + e.package + " from image " + name + "'s manifest?"))
					return;
				try {
					await apiRequest("DELETE",
					    CIX_API.unsetImageManifestEntry(name, e.package));
					showStatus("Removed " + e.package + " from " + name + "'s manifest");
					refreshImageDetailVersioning(name);
				} catch (err) {
					showStatus("Failed to remove manifest entry: " +
					    err.message, true);
				}
			});
			return [rm];
		} },
	], {
		op: "getImage",
		key: "image-detail-manifest",
		extra: name,
		what: "this image's declared intent",
		empty: "No manifest entries -- this image has no declared package intent",
	});

	/* An action rather than a setting: bound to this image, cleared
	 * once the entry is set (#592). */
	syncForm(document.getElementById("imgd-manifest-form"), "setImageManifestEntry", {
		args: [name], submit: "Set entry", saved: "Manifest entry set",
		failed: "Failed to set manifest entry", clear: true,
		after: () => refreshImageDetailVersioning(name),
	});
}

/* Full immutable version history (ADR-0107/0108) -- newest first,
 * current_version highlighted. Read-only: a version is only ever
 * produced by an install/upgrade/delete or the rolling auto-rebuild
 * trigger (task #720), never created or removed directly here. */
/*
 * Generated (#592). `created_at` needs no formatting here at all:
 * `ImageVersionHistoryEntry.created_at` carries `format:
 * unix-seconds`, so the renderer reads the contract and the hand-rolled
 * `formatInstant()` call goes with the cell.
 *
 * "current" was a bare `<strong>`, and the Primitives board has a
 * widget for it: a badge carries a value the daemon reported, and
 * which version is current is reported (`current_version`). So it is
 * a badge now -- derived, because currentness is a comparison between
 * two fields rather than a field of its own.
 */
function renderImageDetailVersions(name, data) {
	const current = data.current_version || "";

	renderTable(document.querySelector("#imgd-versions"),
	    "ImageVersionHistoryEntry",
	    Array.isArray(data.versions) ? data.versions : [], [
		"version",
		{ field: "created_at", label: "Created" },
		{ label: "Status",
		  text: (v) => (v.version === current ? "current" : ""),
		  badge: () => "ok" },
	], {
		op: "getImage",
		key: "image-detail-versions",
		extra: name,
		what: "this image's version history",
		empty: "No version history",
	});
}

/* Installed-on-this-image packages, with a per-row Remove -- same
 * DELETE /v1/pkg/{name@image} removePkg() already uses from the
 * Packages section, just scoped to whichever image is on screen. */
/*
 * Generated (#592). It also GAINS ITS OTHER THREE STATES, which is the
 * part worth stating: this is a filtered view of `cache.pkgList`, read
 * by `listPkg`, and it used to say "No packages installed into this
 * image" whatever had happened to that read -- including a refusal,
 * which is #538's own bug (the owner reported it with three LDAP users
 * present). Keying on the op is what gives it "Reading…" and the
 * refusal sentence for free.
 *
 * `key` and `extra` because the same operation feeds this table for
 * every image: two images with identical package lists must still
 * redraw when you navigate between them.
 */
function renderImageDetailPackages(name) {
	renderTable(document.querySelector("#imgd-packages"), "PkgEntry",
	    cache.pkgList.filter((p) => p.image === name), [
		{ field: "name", link: (p) => "#packages/" + encodeURIComponent(p.name) },
		"version",
		pkgStateColumn(),
		{ label: "", actions: (p) => {
			if (p.state !== "installed")
				return [];

			const rm = document.createElement("button");

			rm.textContent = "Remove";
			rm.className = "button-danger";
			gateAction(rm, "deletePkg");
			rm.addEventListener("click", async () => {
				await removePkg(p.name, p.image);
				renderImageDetailPackages(name);
				renderImageDetailRecipes(name);
			});
			return [rm];
		} },
	], {
		op: "listPkg",
		key: "image-detail-packages",
		extra: name,
		what: "installed packages",
		empty: "No packages installed into this image",
	});
}

/* The shared recipe catalog (ADR-0040), rendered here with a per-row
 * Install/Remove scoped to whichever image is on screen -- the "add/
 * delete recipes from images" convenience the user asked for, without
 * turning recipes into a per-image concept: POST /v1/pkg/install and
 * DELETE /v1/pkg/{name@image} already take an image, this just supplies
 * it for the recipe row being clicked instead of making the user type
 * a name/image pair into the separate Packages-section modal. */
/*
 * Generated (#592). The catalogue is shared across every image
 * (ADR-0040), so the ROW is the recipe and the only per-image thing is
 * which of the two buttons it carries -- install onto this image, or
 * remove from it.
 */
function renderImageDetailRecipes(name) {
	const installed = new Set(cache.pkgList
	    .filter((p) => p.image === name && p.state === "installed")
	    .map((p) => p.name));

	renderTable(document.querySelector("#imgd-recipes"), "PkgRecipe",
	    cache.pkgRecipes, [
		"name",
		"version",
		"depends",
		{ label: "", actions: (r) => {
			const b = document.createElement("button");

			if (installed.has(r.name)) {
				b.textContent = "Remove from image";
				b.className = "button-danger";
				gateAction(b, "deletePkg");
				b.addEventListener("click", async () => {
					await removePkg(r.name, name);
					await refreshPkgList();
					renderImageDetailPackages(name);
					renderImageDetailRecipes(name);
				});
				return [b];
			}
			b.textContent = "Install onto this image";
			gateAction(b, "pkgInstall");
			b.addEventListener("click", async () => {
				try {
					await apiRequest("POST", CIX_API.pkgInstall(),
					    { name: r.name, image: name });
					clearStatus();
					await refreshPkgList();
					renderImageDetailPackages(name);
					renderImageDetailRecipes(name);
				} catch (e) {
					showStatus("Failed to install " + r.name + " onto " +
					    name + ": " + e.message, true);
				}
			});
			return [b];
		} },
	], {
		op: "listPkgRecipes",
		key: "image-detail-recipes",
		/* The rows are the same for every image; the BUTTONS are not,
		 * so the image has to be in the compared signature or
		 * navigating between two images leaves the previous one's
		 * install/remove choices in place. */
		extra: name,
		what: "the recipe catalogue",
		empty: "No recipes found",
	});
}

/* ---------- Devices ---------- */

async function refreshDevices() {
	const data = await apiRequest("GET", CIX_API.listDevices());
	cache.devices = data.devices;
	if (onPageOf("devices"))
		renderDevices();
}

async function refreshDeviceMaps() {
	const data = await apiRequest("GET", CIX_API.listDeviceMaps());
	cache.deviceMaps = data.devicemaps;
	if (onPageOf("devices"))
		renderDevices();
}

const DEVICE_BUS_TABLES = { usb: "dev-usb", pci: "dev-pci", net: "dev-net", gpu: "dev-gpu" };

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

	for (const bus of Object.keys(DEVICE_BUS_TABLES))
		renderDeviceBus(bus, mappedNames);
}

/*
 * Generated (#592), from `Device`, one table per bus.
 *
 * A GPU is several device nodes (gpu:N:card0, gpu:N:renderD128, ...)
 * granted together by their gpu:N prefix -- exactly what
 * device_find_group() does server-side for a container's
 * devices:["gpu:0"] grant. The GPU table used to say so with a bold
 * header row above each group; it says it with a Group column now,
 * rows sorted so a group's nodes sit together, and the "grant the whole
 * id" sentence is the tab's hint. Same information, without a row shape
 * only this table had.
 */
function renderDeviceBus(bus, mappedNames) {
	const entries = cache.devices.filter((d) => d.bus === bus);
	const columns = [
		{ field: "id", label: "ID" },
		"description",
		"driver",
		{ field: "assignable", badge: (v) => (v ? "ok" : "unknown") },
		{ label: "Name", text: (d) => mappedNames[d.id] || FIELD_ABSENT },
		{ label: "", actions: (d) => {
			if (mappedNames[d.id])
				return [];

			const btn = document.createElement("button");

			btn.type = "button";
			btn.textContent = "Name…";
			gateAction(btn, "createDeviceMap");
			/* The selector and kind are facts about the row that was
			 * clicked, so they arrive as `values` -- a draft the
			 * operator may still edit, which is right here: naming a
			 * USB device "exact" pins it to this port, and switching
			 * to vendor/model is a reasonable second thought. `fixed`
			 * would be wrong, because nothing about this device is the
			 * request's key. */
			btn.addEventListener("click", () =>
				openForm("createDeviceMap", {
					title: "Name a device",
					submit: "Create mapping",
					values: { kind: "exact", selector: d.id },
				}));
			return [btn];
		} },
	];

	if (bus === "gpu") {
		entries.sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
		columns.unshift({ label: "Group", text: (d) => d.id.split(":").slice(0, 2).join(":") });
	}
	renderTable(document.getElementById(DEVICE_BUS_TABLES[bus]), "Device", entries, columns, {
		op: "listDevices",
		key: "devices-" + bus,
		/* #432: a row shows whether a mapping claims the device, so a
		 * new mapping must redraw it although the devices did not
		 * change. */
		extra: entries.map((d) => mappedNames[d.id] || "").join("\n"),
		what: "devices",
		empty: "No devices discovered on this bus",
	});
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
		cell.textContent = emptyStateText("listDeviceMaps", "device names", "No named devices yet");
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
		gateAction(removeBtn, "deleteDeviceMap");
		removeBtn.addEventListener("click", () => removeDeviceMap(m.name));
		actionCell.appendChild(removeBtn);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function removeDeviceMap(name) {
	if (!confirm("Remove device mapping \"" + name + "\"? Containers already granted it keep "
	             + "their devices."))
		return;
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
}

async function refreshDiskRoles() {
	const data = await apiRequest("GET", CIX_API.listStorageRoles());

	cache.storageRoles = data.storage_roles;
	if (onPageOf("storage"))
		renderDisks();
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
	logs: { showPath: CIX_API.getLogStorage, migrateOp: "migrateLogStorage",
	        migrateStatusPath: CIX_API.getLogStorageMigrateStatus,
	        cacheKey: "logStorage", statusCacheKey: "logStorageMigrate",
	        currentId: "ls-current", statusId: "ls-migrate-status",
	        formId: "ls-migrate-form", label: "Log storage", role: "log-storage" },
	rebuildable: { showPath: CIX_API.getRebuildableStorage,
	               migrateStatusPath: CIX_API.getRebuildableStorageMigrateStatus,
	               migrateOp: "migrateRebuildableStorage", cacheKey: "rebuildableStorage",
	               statusCacheKey: "rebuildableStorageMigrate", currentId: "rs-current",
	               statusId: "rs-migrate-status", formId: "rs-migrate-form",
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

	/* The disks carrying this kind's role; the blank sends null, the
	 * default OS-disk placement -- "" is refused (#592: the form now
	 * knows the field is nullable). */
	syncForm(document.getElementById(k.formId), k.migrateOp, {
		choices: { disk: (cache.storage || [])
			.filter((d) => { const r = storageRoleFor(d.name); return !d.protected && r !== null && r.role === k.role; })
			.map((d) => ({ value: d.name, label: d.name + (d.model ? " (" + d.model + ")" : "") })) },
		submit: "Migrate", failed: "Failed to start " + k.label.toLowerCase() + " migration",
		saved: k.label + " migration started",
		after: (body, result) => {
			cache[k.statusCacheKey] = result;
			renderStoragePlacement(kind);
		},
	});

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

	/* Disks with the container-storage role; the blank sends null, the
	 * default OS-disk placement. */
	syncForm(document.getElementById("cd-storage-migrate-form"), "migrateContainerStorage", {
		args: [name],
		choices: { disk: (cache.storage || [])
			.filter((d) => { const r = storageRoleFor(d.name); return !d.protected && r !== null && r.role === "container-storage"; })
			.map((d) => ({ value: d.name, label: d.name + (d.model ? " (" + d.model + ")" : "") })) },
		submit: "Migrate", saved: "Storage migration started",
		failed: "Failed to start storage migration for " + name,
		after: (body, result) => {
			cache.containerStorageMigrate = result;
			renderContainerStorage(name);
		},
	});

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

	/*
	 * #432, and this table is the one the plain guard could not fix.
	 * Its payload moves on every tick by itself -- measured on
	 * 192.168.15.95: reads_completed 230 -> 234, sectors_read
	 * 4480 -> 4560, io_time_ms 268 -> 272 across two ticks with the
	 * host otherwise idle -- and those counters are DISPLAYED, so the
	 * rows must show the new numbers. The guard was right not to fire
	 * and the table was rebuilt anyway.
	 *
	 * So the question the signature asks is narrowed to the one it can
	 * answer usefully: has anything STRUCTURAL changed? If not, the
	 * moving numbers are written into their own cells and no row is
	 * touched, which is what preserves the scroll position. The roles
	 * stay in the signature -- a row shows its disk's assigned role, so
	 * an assignment must still redraw.
	 */
	if (unchangedAndRendered(body, "disks",
	                         { disks: diskStructuralSignature(disks),
	                           roles: cache.storageRoles })) {
		if (updateDiskLiveCells(body, disks))
			return;
		/* A row went missing under us -- fall through and rebuild
		 * rather than leave the table half-stale. */
	}
	body.textContent = "";
	if (disks.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 9;
		cell.className = "empty";
		cell.textContent = emptyStateText("listStorage", "disks", "No disks found");
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}
	for (const d of disks)
		body.appendChild(diskListRow(d));
}

/*
 * The two cells of a disk row that move on their own (#432).
 *
 * A disk record carries live counters -- the kernel's I/O totals and
 * statvfs usage -- so this table's payload genuinely changes on almost
 * every 2s tick. The guard every other table uses cannot help here:
 * the data really did change and the rows really must show the new
 * numbers. But REBUILDING the tbody to do it is what costs the reader
 * their scroll position, so these two cells are written in place and
 * the rest of the row is left untouched.
 *
 * One definition of each cell's text, shared by diskListRow() and
 * updateDiskLiveCells(): two ways to render the same cell is how a
 * live update and a rebuilt row come to disagree.
 */
const DISK_VOLATILE_FIELDS = ["used_bytes", "free_bytes", "reads_completed",
                              "writes_completed", "sectors_read", "sectors_written",
                              "io_time_ms"];

function diskUsageText(d) {
	return d.mounted
		? formatBytes(d.used_bytes) + " used / " + formatBytes(d.free_bytes) + " free"
		: "-";
}

function diskIoText(d) {
	return "r=" + d.reads_completed + " w=" + d.writes_completed;
}

/*
 * The rows' payload minus the fields that move by themselves, so the
 * signature answers "has anything STRUCTURAL changed" -- a disk added
 * or removed, a role assigned, a filesystem appearing, a mount.
 *
 * Subtractive rather than a positive list of the fields the static
 * cells use: forgetting to list a new volatile field here means the
 * table rebuilds too often, which is merely the old behaviour, where
 * forgetting to add a new static field to a positive list would leave
 * a stale cell on screen.
 */
function diskStructuralSignature(disks) {
	return disks.map((d) => {
		const copy = {};

		for (const k of Object.keys(d))
			if (!DISK_VOLATILE_FIELDS.includes(k))
				copy[k] = d[k];
		return copy;
	});
}

/*
 * Writes the moving numbers straight into the cells diskListRow()
 * tagged, touching no structure. Returns false when a row it expected
 * is missing, so the caller falls back to a full rebuild rather than
 * silently leaving the table stale.
 */
function updateDiskLiveCells(body, disks) {
	for (const d of disks) {
		const row = body.querySelector('tr[data-disk="' + CSS.escape(d.name) + '"]');
		const usage = row && row.querySelector('[data-cell="usage"]');
		const io = row && row.querySelector('[data-cell="io"]');

		if (!usage || !io)
			return false;
		usage.textContent = diskUsageText(d);
		io.textContent = diskIoText(d);
	}
	return true;
}

/* One row in the disks list: identity and state only. Every action
 * lives on the disk's own page, so a destructive button is never one
 * stray click away in a list. */
function diskListRow(d) {
	const row = document.createElement("tr");
	const role = storageRoleFor(d.name);

	/* Tagged so updateDiskLiveCells() can find this row and its two
	 * moving cells without rebuilding anything (#432). */
	row.dataset.disk = d.name;
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

	usageCell.dataset.cell = "usage";
	usageCell.textContent = diskUsageText(d);
	row.appendChild(usageCell);

	const ioCell = document.createElement("td");

	ioCell.dataset.cell = "io";
	ioCell.textContent = diskIoText(d);
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
		gateAction(assignBtn, "createStorageRole");
		assignBtn.addEventListener("click", () => openAssignRole(d.name));
		actionCell.appendChild(assignBtn);
	} else if (formatStatus && formatStatus.state === "running") {
		actionCell.appendChild(document.createTextNode("(formatting…)"));
	} else {
		const removeRoleBtn = document.createElement("button");

		removeRoleBtn.type = "button";
		removeRoleBtn.className = "button-small";
		removeRoleBtn.textContent = "Remove role";
		gateAction(removeRoleBtn, "deleteStorageRole");
		removeRoleBtn.addEventListener("click", () => removeDiskRole(d.name));
		actionCell.appendChild(removeRoleBtn);

		const fsSelect = buildFsSelect(d.name);
		const formatBtn = document.createElement("button");

		formatBtn.type = "button";
		formatBtn.className = "button-danger button-small";
		formatBtn.textContent = "Format…";
		gateAction(formatBtn, "formatStorage");
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
		/* Bound to THIS disk: its path argument is the page's (#592). */
		syncForm(document.getElementById("dd-add-partition-form"), "addStoragePartition", {
			args: [d.name], submit: "Add partition", saved: "Partition added",
			failed: "Failed to add partition", clear: true,
			after: async () => {
				await refreshDisks();
				renderDiskDetail(d.name);
				renderTree();
			},
		});
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
		gateAction(assignBtn, "createStorageRole");
		assignBtn.addEventListener("click", () => openAssignRole(d.name));
		actions.appendChild(assignBtn);
	}
	if (e.removeRole) {
		const removeBtn = document.createElement("button");

		removeBtn.type = "button";
		removeBtn.textContent = "Remove role";
		gateAction(removeBtn, "deleteStorageRole");
		removeBtn.addEventListener("click", () => removeDiskRole(d.name));
		actions.appendChild(removeBtn);
	}
	if (e.format) {
		const fsSelect = buildFsSelect(d.name);
		const formatBtn = document.createElement("button");

		formatBtn.type = "button";
		formatBtn.className = "button-danger";
		formatBtn.textContent = "Format…";
		gateAction(formatBtn, "formatStorage");
		formatBtn.addEventListener("click", () => formatDisk(d.name, fsSelect.value));
		actions.appendChild(fsSelect);
		actions.appendChild(formatBtn);
	}
	if (e.unmount) {
		const unmountBtn = document.createElement("button");

		unmountBtn.type = "button";
		unmountBtn.textContent = "Unmount";
		gateAction(unmountBtn, "unmountStorage");
		unmountBtn.addEventListener("click", () => unmountDisk(d.name));
		actions.appendChild(unmountBtn);
	}
	if (e.grow) {
		const growBtn = document.createElement("button");

		growBtn.type = "button";
		growBtn.textContent = "Grow…";
		gateAction(growBtn, "resizeStoragePartition");
		growBtn.addEventListener("click", () => growPartition(d));
		actions.appendChild(growBtn);
	}
	if (e.del) {
		const delBtn = document.createElement("button");

		delBtn.type = "button";
		delBtn.className = "button-danger";
		delBtn.textContent = "Delete this partition";
		delBtn.disabled = e.delDisabled;
		gateAction(delBtn, "deleteStoragePartition");
		delBtn.title = e.delDisabled ? "Unmount it first." : "";
		delBtn.addEventListener("click", () => deletePartition(d.parent_disk, d.name));
		actions.appendChild(delBtn);
	}
}

async function openAssignRole(name) {
	let choices;

	try {
		choices = await diskRoleChoices();
	} catch (e) {
		showStatus("Could not read the disks: " + e.message, true);
		return;
	}
	openForm("createStorageRole", {
		title: "Assign a role to " + name,
		submit: "Assign role",
		values: { disk_name: name, role: "container-storage" },
		choices: choices,
		/* The roles, wherever the form was opened from: the tree's
		 * right-click menu reads them too, on every page. */
		after: refreshDiskRoles,
		failed: "Failed to assign a role to " + name,
	});
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
	/* Each segment's `kind` -- part, os or free -- is a class in
	 * style.css; the colour is a token there, defined for both themes. */
	let allocated = 0;
	const segments = [];

	if (wholeDisk) {
		const used = d.used_bytes || 0;
		const avail = d.free_bytes || 0;

		if (used > 0) {
			segments.push({
				label: "used",
				bytes: used,
				kind: "part",
				note: d.fs_type || "",
			});
		}
		if (avail > 0) {
			segments.push({
				label: "free",
				bytes: avail,
				kind: "free",
				note: "available",
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
				kind: "free",
				note: "metadata and reserved blocks",
			});
		}
	} else {
		parts.forEach((p) => {
			const bytes = p.size_bytes || 0;

			allocated += bytes;
			segments.push({
				label: p.name,
				bytes: bytes,
				kind: p.protected ? "os" : "part",
				note: p.protected ? "protected" : p.part_label || "",
			});
		});
		const free = Math.max(0, d.size_bytes - allocated);

		if (free > 0) {
			segments.push({
				label: "unallocated",
				bytes: free,
				kind: "free",
				note: "available",
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
	/* A thin gap between neighbouring segments: two partitions of the
	 * same class are one colour now, and without it they would read as
	 * one partition. */
	const gap = segments.length > 1 ? 1.5 : 0;

	for (const seg of segments) {
		const circle = document.createElementNS(SVG_NS, "circle");
		const len = (seg.bytes / d.size_bytes) * C;
		const drawn = Math.max(0, len - Math.min(gap, len / 2));

		circle.setAttribute("cx", "70");
		circle.setAttribute("cy", "70");
		circle.setAttribute("r", String(R));
		circle.setAttribute("fill", "none");
		circle.setAttribute("class", "alloc-seg-" + seg.kind);
		circle.setAttribute("stroke-width", "18");
		circle.setAttribute("stroke-dasharray", drawn + " " + (C - drawn));
		circle.setAttribute("stroke-dashoffset", String(-offset));
		/* Start at twelve o'clock; the default is three, which reads
		 * as an arbitrary rotation rather than a whole. */
		circle.setAttribute("transform", "rotate(-90 70 70)");
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

		swatch.className = "alloc-swatch alloc-swatch-" + seg.kind;
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

	/*
	 * Generated (#592). Four of its seven columns are derived and each
	 * for its own reason, which is worth stating because the `text`
	 * hatch is meant to stay narrow:
	 *
	 *  - Size is `formatBytes`, one of 39 call sites in this file. A
	 *    byte count rendered for a person is the same kind of fact as
	 *    an instant and belongs in the contract the same way; until it
	 *    is there, the cell derives it.
	 *  - Role prefers the ASSIGNED role over the partition's own
	 *    label, which is a lookup plus a fallback rather than a field.
	 *  - Filesystem says "unformatted" rather than "—", because that
	 *    is a statement about the partition and not a missing value:
	 *    `fs_type` is "" exactly when there is genuinely no filesystem.
	 *  - Usage is two fields and a division.
	 *
	 * `protected` becomes a plain boolean, so it reads yes/no like
	 * every other boolean here instead of "protected"/"-". It keeps
	 * its own column rather than being a suffix on the role:
	 * protection answers a different question from "what is this for",
	 * and hanging it off the label meant a partition WITH a role never
	 * showed it at all.
	 *
	 * `mount_path` is a plain field now, and the `p.mounted ? ... : "-"`
	 * guard is gone -- measured on 192.168.15.95, 2026-10-11: `sr0`
	 * reports `"mounted":false,"mount_path":""`, so an unmounted device
	 * carries no path and `fieldText` already says "—".
	 */
	renderTable(document.getElementById("dd-partitions"), "StorageDevice", parts, [
		{ field: "name", link: (p) => "#storage/" + encodeURIComponent(p.name) },
		{ label: "Size", text: (p) => formatBytes(p.size_bytes) },
		{ label: "Role", text: (p) => {
			const role = storageRoleFor(p.name);

			return role ? role.role : (p.part_label || FIELD_ABSENT);
		} },
		"protected",
		{ label: "Filesystem", text: (p) => p.fs_type || "unformatted" },
		{ field: "mount_path", label: "Mounted" },
		{ label: "Usage", text: (p) => (p.mounted && p.used_bytes + p.free_bytes > 0
			? Math.round((p.used_bytes / (p.used_bytes + p.free_bytes)) * 100) + "% used"
			: FIELD_ABSENT) },
	], {
		op: "listStorage",
		key: "disk-detail-partitions",
		extra: d.name,
		what: "this disk's partitions",
		empty: d.is_os_disk
			? "No partitions reported"
			: "No partitions — this disk is unpartitioned",
	});
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
	/* The generated form's size field; the bound below is this disk's
	 * largest free gap, which the contract cannot know. */
	const sizeInput = document.querySelector('#dd-add-partition-form [data-field="size_mib"]') ||
		document.createElement("input");

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

/*
 * The disks a role can go to, read from the box when the form opens
 * (#592): not part of the OS layout, and holding no role already. Read
 * fresh rather than from the page's cache, because the header menu
 * opens this from any page, including one that has never read a disk.
 */
async function diskRoleChoices() {
	const [storage, roles] = await Promise.all([
		apiRequest("GET", CIX_API.listStorage()),
		apiRequest("GET", CIX_API.listStorageRoles()),
	]);
	const assigned = (roles.storage_roles || []).map((r) => r.disk_name);

	return {
		disk_name: (storage.storage || [])
			.filter((d) => !d.protected && assigned.indexOf(d.name) === -1)
			.map((d) => ({ value: d.name, label: d.name + (d.model ? " (" + d.model + ")" : "") })),
	};
}

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

/*
 * Editing a record is a DIFFERENT OPERATION, not the same form in a
 * mode (#596).
 *
 * `updateDnsRecord` is `PUT /dns/records/{name}` with a body of `{ip}`
 * alone, so the generated form renders one field and takes the name as
 * a path argument. The hand-written form had to be both: one markup
 * block, a module-level `dnsRecordEditName` deciding POST from PUT, a
 * `readOnly` toggled on the name, a button whose words were rewritten,
 * and three lines in closeModal() to put all of that back -- a mode
 * that outlives its modal is exactly the state a form should not have.
 */
function editDnsRecord(rec) {
	openForm("updateDnsRecord", {
		title: "Edit DNS record " + rec.name,
		submit: "Save",
		args: [rec.name],
		values: { ip: rec.ip },
		after: refreshDnsRecords,
		failed: "Failed to update DNS record " + rec.name,
	});
}

/*
 * Generated (#592). `owner` defers to formatOwner() for the reason the
 * certificates table does: `__host` is a reserved sentinel (ADR-0128,
 * here the daemon's own record for its own FQDN) and which strings are
 * sentinels is daemon knowledge.
 */
function renderDnsRecords(records) {
	renderTable(document.getElementById("dns-records"), "DnsRecord", records, [
		"name",
		{ field: "ip", label: "IP" },
		{ label: "Owner", text: (r) => formatOwner(r.owner) },
		{ label: "", actions: (r) => {
			const edit = document.createElement("button");
			const rm = document.createElement("button");

			edit.textContent = "Edit";
			gateAction(edit, "updateDnsRecord");
			edit.addEventListener("click", () => editDnsRecord(r));

			rm.textContent = "Remove";
			rm.className = "button-danger";
			gateAction(rm, "deleteDnsRecord");
			rm.addEventListener("click", () => removeDnsRecord(r.name));
			return [edit, rm];
		} },
	], { op: "listDnsRecords", what: "DNS records", empty: "No DNS records" });
}

async function refreshDnsRecords() {
	const data = await apiRequest("GET", CIX_API.listDnsRecords());
	cache.dnsRecords = data.records;
	renderDnsRecords(cache.dnsRecords);
}

async function removeDnsRecord(name) {
	if (!confirm("Remove DNS record \"" + name + "\"?"))
		return;
	try {
		await apiRequest("DELETE", CIX_API.deleteDnsRecord(name));
		clearStatus();
		await refreshDnsRecords();
	} catch (e) {
		showStatus("Failed to remove DNS record " + name + ": " + e.message, true);
	}
}

/* ---------- DNS Servers ---------- */

/* Generated (#592). */
function renderDnsServers(servers) {
	renderTable(document.getElementById("dns-servers"), "DnsServerBinding", servers, [
		"container",
		{ field: "hosts_path", label: "Hosts path" },
		{ label: "", actions: (s) => [unregisterButton("deleteDnsServer", "DNS server " + s.container,
		    () => removeDnsServer(s.container))] },
	], { op: "listDnsServers", what: "DNS servers", empty: "No DNS server bindings" });
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

/* Generated (#592). `listLdapServers` has `response: null`, so the
 * contract describes neither column; the cells are still one code
 * path with one empty state. */
function renderLdapServers(servers) {
	renderTable(document.getElementById("ldap-servers"), "LdapServerBinding", servers, [
		"container",
		{ field: "config_path", label: "Config path" },
		{ label: "", actions: (s) => [unregisterButton("deleteLdapServer", "LDAP server " + s.container,
		    () => removeLdapServer(s.container))] },
	], { op: "listLdapServers", what: "LDAP servers", empty: "No LDAP server bindings" });
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

function editLdapGroup(group) {
	openForm("updateLdapGroup", {
		title: "Edit LDAP group " + group.name,
		submit: "Save",
		args: [group.name],
		values: { gidnumber: group.gidnumber },
		after: refreshLdapGroups,
		failed: "Failed to update LDAP group " + group.name,
	});
}

/* Generated (#592). Two fields and two buttons -- the shape the
 * renderer was built for, and the whole of it. */
function renderLdapGroups(groups) {
	renderTable(document.getElementById("ldap-groups"), "LdapGroup", groups, [
		"name",
		{ field: "gidnumber", label: "GID number" },
		{ label: "", actions: (g) => {
			const edit = document.createElement("button");
			const rm = document.createElement("button");

			edit.textContent = "Edit";
			gateAction(edit, "updateLdapGroup");
			edit.addEventListener("click", () => editLdapGroup(g));

			rm.textContent = "Remove";
			rm.className = "button-danger";
			gateAction(rm, "deleteLdapGroup");
			rm.addEventListener("click", () => removeLdapGroup(g.name));
			return [edit, rm];
		} },
	], { op: "listLdapGroups", what: "LDAP groups", empty: "No LDAP groups" });
}

async function refreshLdapGroups() {
	const data = await apiRequest("GET", CIX_API.listLdapGroups());
	cache.ldapGroups = data.groups;
	renderLdapGroups(cache.ldapGroups);
}

async function removeLdapGroup(name) {
	if (!confirm("Delete LDAP group \"" + name + "\" from the directory and every registered "
	             + "server?"))
		return;
	try {
		await apiRequest("DELETE", CIX_API.deleteLdapGroup(name));
		clearStatus();
		await refreshLdapGroups();
	} catch (e) {
		showStatus("Failed to remove LDAP group " + name + ": " + e.message, true);
	}
}

/* ---------- LDAP Users (task #726) ---------- */

/*
 * Editing a user is `updateLdapUser`, whose request schema IS
 * `LdapUserCreateRequest` -- a PUT here is a full-record replacement,
 * so the form has to carry every field and send every field. That is
 * what makes the generated version a bug fix rather than a port:
 *
 *   - **`can_search` was silently revoked by every edit.** The
 *     hand-written body never included it, `parse_ldap_user_body()`
 *     reads an absent `can_search` as FALSE, and `ldap_user_update()`
 *     writes `u->can_search` unconditionally -- so changing a service
 *     account's mail address dropped its search grant, which is what
 *     an nslcd binddn and a live AuthorizedKeysCommand bind need. The
 *     contract has said "Full-field-replacement on PUT like every
 *     other field here" the whole time; the form was the only thing
 *     that disagreed (cix#604).
 *   - **`disabled` rendered as a large grey square**, because its
 *     markup carried no `class="checkbox"` and the global
 *     `form input` rule gives every input `width: 100%`.
 *
 * `password` is left empty on purpose and omitted: `fill_user_fields()`
 * only overwrites the hash when the value is non-NULL and non-empty,
 * so an untouched password field keeps the existing credential --
 * measured, and the contract's own description says so.
 */
function editLdapUser(user) {
	openForm("updateLdapUser", {
		title: "Edit LDAP user " + user.name,
		submit: "Save",
		args: [user.name],
		fixed: { name: user.name },
		values: {
			uidnumber: user.uidnumber,
			primarygroup: user.primarygroup,
			secondary_groups: user.secondary_groups || [],
			givenname: user.givenname,
			sn: user.sn,
			mail: user.mail,
			loginshell: user.loginshell,
			homedirectory: user.homedirectory,
			ssh_public_key: user.ssh_public_key,
			can_search: user.can_search,
			disabled: user.disabled,
		},
		after: refreshLdapUsers,
		failed: "Failed to update LDAP user " + user.name,
	});
}

/*
 * Generated (#592). The two "set"/"unset" columns become yes/no, which
 * is what every other boolean in the dashboard says, and they now say
 * the same thing as each other: `has_password` is a boolean the
 * contract already answers that way, and the SSH column is asking the
 * same yes/no question.
 *
 * `ssh_public_key` is the one cell that cannot be a plain field, and
 * not because of wording: the contract returns the KEY ITSELF, *"as
 * given, unlike password -- an SSH public key is not a secret"*, so a
 * plain field would put a 400-character blob in a nine-column table.
 * What the column shows is whether there is one, which is derived.
 */
function renderLdapUsers(users) {
	renderTable(document.getElementById("ldap-users"), "LdapUser", users, [
		"name",
		{ field: "uidnumber", label: "UID" },
		{ field: "primarygroup", label: "Primary group" },
		{ field: "secondary_groups", label: "Secondary groups" },
		{ field: "mail", label: "Mail" },
		{ field: "has_password", label: "Password" },
		"disabled",
		{ label: "SSH key", text: (u) => (u.ssh_public_key ? "yes" : "no") },
		{ label: "", actions: (u) => {
			const edit = document.createElement("button");
			const apps = document.createElement("button");
			const rm = document.createElement("button");

			edit.textContent = "Edit";
			gateAction(edit, "updateLdapUser");
			edit.addEventListener("click", () => editLdapUser(u));

			/* #544: that user's machine credentials. */
			apps.textContent = "App passwords";
			gateAction(apps, "listLdapUserAppPasswords");
			apps.addEventListener("click", () => openAppPasswords({ user: u.name }));

			rm.textContent = "Remove";
			rm.className = "button-danger";
			gateAction(rm, "deleteLdapUser");
			rm.addEventListener("click", () => removeLdapUser(u.name));
			return [edit, apps, rm];
		} },
	], { op: "listLdapUsers", what: "LDAP users", empty: "No LDAP users" });
}

/*
 * The panel renders empty rather than stale on a refused read: showing
 * the last roster read before a session lapsed would be a list of real
 * people that is no longer being refreshed, which is worse than showing
 * nothing. It used to skip the read itself when this page held no
 * token (#490); since #544 the sweep pauses as a whole on a gated host
 * without a session (poll()), which covers that case once for every
 * panel -- and on an ungated host this read needs no token at all.
 */
async function refreshLdapUsers() {
	try {
		const data = await apiRequest("GET", CIX_API.listLdapUsers());

		cache.ldapUsers = data.users;
		renderLdapUsers(cache.ldapUsers);
	} catch (e) {
		/* A refresher's 401 does not open the login form (the sweep
		 * guard), so the panel says why it is empty. Never "No LDAP
		 * users": nothing was read (#538, reported by the owner
		 * 2026-09-29 with three users present).
		 *
		 * The sentence is no longer passed in. apiRequest() recorded
		 * this failure against `listLdapUsers` on its way out, and
		 * listState() reads it -- so handing refusalText() to the
		 * renderer was a second copy of one decision, and one that
		 * could only ever produce the REFUSED state: a panel whose
		 * read had not come back yet still said "No LDAP users". */
		cache.ldapUsers = [];
		renderLdapUsers(cache.ldapUsers);
	}
}

async function removeLdapUser(name) {
	if (!confirm("Delete LDAP user \"" + name + "\" from the directory and every registered "
	             + "server?"))
		return;
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
	try {
		const data = await apiRequest("GET", CIX_API.listBuildLogs());

		renderBuildLogs(data.logs || []);
	} catch (e) {
		/* listState() has the failure; an empty list lets it say so,
		 * where this best-effort catch used to leave whatever was last
		 * painted -- "Reading…", on a first failure, forever. */
		renderBuildLogs([]);
	}
}

/*
 * Generated (#592), from `BuildLogEntry`, promoted out of
 * GET /pkg/build-logs' inline items for this table. `modified_at`
 * needs no formatting here any more: the schema says it is an instant.
 *
 * #432's reason for this table's guard still holds and is now
 * renderTable's: a log list changes when a build writes one, which is
 * rare next to the refresh tick, so in practice the table stops being
 * rebuilt and the log you just opened stops taking your scroll position.
 */
function renderBuildLogs(logs) {
	renderTable(document.getElementById("build-logs"), "BuildLogEntry", logs, [
		{ field: "file", label: "Log" },
		{ label: "Size", text: (l) => formatBytes(l.size_bytes) },
		{ field: "modified_at", label: "Finished" },
		{ label: "", actions: (l) => {
			const view = document.createElement("button");

			view.textContent = "View";
			view.className = "button-small";
			view.addEventListener("click", () => showBuildLog(l.file));
			return [view];
		} },
	], { op: "listBuildLogs", what: "build logs", empty: "No builds recorded yet" });
}

/* ---------- #433: the build-log window ----------
 *
 * One floating window for every build log the dashboard shows: a saved
 * log from the Log tab, read once, and a build in flight from the Build
 * overview, followed live over GET /v1/pkg/build/log (a WebSocket, the
 * same one `cixctl pkg build-log` uses). These were two separate inline
 * viewers, and both were a snapshot of the moment they were opened.
 */

/* A live log keeps at most this many lines on screen. A long build
 * writes megabytes, and a DOM that grows for the whole build makes the
 * tab suffer. The cap is the window's, not the log's: the saved log on
 * the Log tab keeps every line. It is trimmed back to the cap once it
 * has run a fifth over, rather than on every chunk, so a busy build does
 * not re-split the whole text for each line it prints. */
const BUILD_LOG_MAX_LINES = 5000;

let buildLogSocket = null;
let buildLogText = "";
let buildLogLines = 0;
let buildLogFollowing = true;
let buildLogRenderPending = false;
let buildLogWindowPlaced = false;

/* The title bar's state control: "following", "paused -- follow" (the
 * one clickable state), "build finished", or hidden for a saved log,
 * which has nothing to follow. */
function setBuildLogState(text, clickable) {
	const state = document.getElementById("build-log-state");

	state.hidden = text === "";
	state.textContent = text;
	state.disabled = !clickable;
}

/*
 * Moving and resizing are makeResizable(), the splitters' own helper,
 * one instance per axis: the title bar carries two (left, top) and the
 * corner two (width, height). Registered the first time the window is
 * shown rather than at load, because the helper measures its starting
 * size then, and a hidden element measures zero.
 */
function placeBuildLogWindow(win) {
	const bar = document.getElementById("build-log-bar");
	const corner = document.getElementById("build-log-resize");
	const rect = win.getBoundingClientRect();

	/* Position by left/top from here on; the stylesheet's right/bottom
	 * corner default gives way. */
	win.style.left = rect.left + "px";
	win.style.top = rect.top + "px";
	win.style.right = "auto";
	win.style.bottom = "auto";
	makeResizable(bar, {
		axis: "x",
		storageKey: "cix-build-log-left",
		min: 0,
		max: Math.max(0, window.innerWidth - 160),
		getSize: () => win.getBoundingClientRect().left,
		apply: (v) => { win.style.left = v + "px"; },
	});
	makeResizable(bar, {
		axis: "y",
		storageKey: "cix-build-log-top",
		min: 0,
		max: Math.max(0, window.innerHeight - 60),
		getSize: () => win.getBoundingClientRect().top,
		apply: (v) => { win.style.top = v + "px"; },
	});
	makeResizable(corner, {
		axis: "x",
		storageKey: "cix-build-log-width",
		min: 320,
		max: window.innerWidth,
		getSize: () => win.getBoundingClientRect().width,
		apply: (v) => { win.style.width = v + "px"; },
	});
	makeResizable(corner, {
		axis: "y",
		storageKey: "cix-build-log-height",
		min: 160,
		max: window.innerHeight,
		getSize: () => win.getBoundingClientRect().height,
		apply: (v) => { win.style.height = v + "px"; },
	});
}

function closeBuildLogSocket() {
	const socket = buildLogSocket;

	buildLogSocket = null;
	if (socket !== null)
		socket.close();
}

function openBuildLogWindow(title) {
	const win = document.getElementById("build-log-window");

	closeBuildLogSocket();
	document.getElementById("build-log-title").textContent = title;
	win.hidden = false;
	if (!buildLogWindowPlaced) {
		buildLogWindowPlaced = true;
		placeBuildLogWindow(win);
	}
}

/* The one place that closes the window. The button and Escape both
 * route here, so there is one definition of "closed", and it stops a
 * live stream rather than leaving it running unseen. */
function closeBuildLog() {
	closeBuildLogSocket();
	document.getElementById("build-log-window").hidden = true;
}

function renderBuildLog() {
	const view = document.getElementById("build-log-view");

	buildLogRenderPending = false;
	view.textContent = buildLogText;
	if (buildLogFollowing)
		view.scrollTop = view.scrollHeight;
}

function appendBuildLog(chunk) {
	buildLogText += chunk;
	buildLogLines += chunk.split("\n").length - 1;
	if (buildLogLines > BUILD_LOG_MAX_LINES * 1.2) {
		const lines = buildLogText.split("\n");

		buildLogText = lines.slice(lines.length - BUILD_LOG_MAX_LINES).join("\n");
		buildLogLines = BUILD_LOG_MAX_LINES - 1;
	}
	/* One render per frame however many chunks arrive in it. */
	if (!buildLogRenderPending) {
		buildLogRenderPending = true;
		requestAnimationFrame(renderBuildLog);
	}
}

/* A saved log, read once: it is finished, so there is nothing to poll. */
async function showBuildLog(file) {
	const view = document.getElementById("build-log-view");

	openBuildLogWindow(file);
	setBuildLogState("", false);
	view.textContent = "Reading…";
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

/*
 * The newest saved log for a package (and, when given, an image), or
 * null: the one lookup the build window and the pipeline drawer share.
 * Matched on the `name` the daemon records for each log (#550), never
 * on the filename -- "cix-" also prefixes "cix-tests-...". A log the
 * daemon cannot attribute has a null name and matches nothing.
 */
async function latestBuildLogFile(name, image) {
	const list = await apiRequest("GET", CIX_API.listBuildLogs());
	const mine = (list.logs || [])
		.filter((l) => l.name === name && (!image || l.image === image))
		.sort((x, y) => y.modified_at - x.modified_at);

	return mine.length > 0 ? mine[0].file : null;
}

/* What a build that has already ended leaves behind. */
async function showLatestBuildLog(name, image) {
	const view = document.getElementById("build-log-view");

	try {
		const file = await latestBuildLogFile(name, image);

		if (file !== null) {
			await showBuildLog(file);
			return;
		}
		view.textContent = "No build of " + name + " is in progress, and it has no saved log yet.";
	} catch (e) {
		view.textContent = "Could not read the build logs: " + refusalText(e, "build logs");
	}
	setBuildLogState("", false);
}

/*
 * A build in flight, followed live. The daemon sends the tail it has
 * captured so far and then every chunk as it is produced, and closes the
 * socket when the build ends. A socket that closes having sent nothing
 * means there was no such build to attach to -- it finished between the
 * overview's refresh and the click, or never produced output -- so the
 * window falls back to the newest saved log rather than showing nothing.
 */
function followBuildLog(name, image) {
	const view = document.getElementById("build-log-view");
	const proto = location.protocol === "https:" ? "wss:" : "ws:";
	const query = "?name=" + encodeURIComponent(name) +
		(image ? "&image=" + encodeURIComponent(image) : "");
	/* #541: a browser WebSocket cannot send an Authorization header,
	 * so the session rides as a subprotocol, exactly as the console's. */
	const protocols = authToken ? ["cix", "cix.bearer." + authToken] : [];
	const decoder = new TextDecoder();
	let received = false;

	openBuildLogWindow(name + (image ? "@" + image : "") + " (live)");
	buildLogText = "";
	buildLogLines = 0;
	buildLogFollowing = true;
	view.textContent = "Connecting…";
	setBuildLogState("connecting", false);

	const ws = new WebSocket(proto + "//" + location.host + CIX_API.pkgBuildLog() + query,
		protocols);

	ws.binaryType = "arraybuffer";
	buildLogSocket = ws;
	ws.onopen = () => {
		if (ws === buildLogSocket)
			setBuildLogState("following", false);
	};
	ws.onmessage = (event) => {
		if (ws !== buildLogSocket)
			return; /* a stream this window has already left */
		received = true;
		/* stream: true -- a UTF-8 character can be split across two
		 * frames, as the console's decoder already knows. */
		appendBuildLog(typeof event.data === "string"
			? event.data
			: decoder.decode(new Uint8Array(event.data), { stream: true }));
	};
	ws.onclose = () => {
		if (ws !== buildLogSocket)
			return;
		buildLogSocket = null;
		if (!received) {
			showLatestBuildLog(name, image);
			return;
		}
		setBuildLogState("build finished", false);
	};
}

/* Following is a property of where the reader is: scrolled to the end
 * it follows, scrolled up it stops, so it never drags someone away
 * from the line they are reading. */
function onBuildLogScroll() {
	const view = document.getElementById("build-log-view");
	const atEnd = view.scrollTop + view.clientHeight >= view.scrollHeight - 4;

	if (buildLogSocket === null || atEnd === buildLogFollowing)
		return;
	buildLogFollowing = atEnd;
	setBuildLogState(atEnd ? "following" : "paused — follow", !atEnd);
}

function resumeBuildLogFollowing() {
	const view = document.getElementById("build-log-view");

	buildLogFollowing = true;
	view.scrollTop = view.scrollHeight;
	setBuildLogState("following", false);
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
		pre.textContent = "Reading…";
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
	try {
		const data = await apiRequest("GET", CIX_API.listSchedules());

		renderSchedules(data.schedules || []);
	} catch (e) {
		/* listState() has the failure and says why, where this catch
		 * used to write its own "Could not load schedules: <message>". */
		renderSchedules([]);
	}
}

/*
 * Generated (#592), from `Schedule` -- which apigen could not read
 * until the same change taught it `allOf`, so its own `next_run_at`
 * and `last_run_at`, both instants, never reached a renderer. Next run
 * is a plain field now; it is null when a job is disabled, because a
 * disabled job has no next run and inventing one would be a time
 * nothing will happen at.
 *
 * "Enabled" is a plain field too: the contract now SAYS `default: true`
 * rather than only describing it, so an absent value renders "yes" from
 * the schema instead of from a hand-written `s.enabled === false`.
 *
 * The last run's RESULT is its own column. It used to be a badge
 * appended inside the time cell; "has not run" and "ran and failed" are
 * different facts, which is why `last_ok` is nullable, and a column of
 * its own says which without a reader having to parse one cell for two.
 * A failure is an error badge with the daemon's reason as its hover; a
 * success is the ordinary word, so its badge kind is null.
 */
function renderSchedules(rows) {
	renderTable(document.getElementById("schedules-table"), "Schedule", rows, [
		"name",
		"action",
		{ label: "When", text: (s) => scheduleWhenText(s) },
		"enabled",
		{ label: "Last run", text: (s) => (s.last_run_at ? formatInstant(s.last_run_at) : "never") },
		{ label: "Result",
		  text: (s) => (s.last_ok === false ? "failed" : s.last_ok === true ? "ok" : ""),
		  badge: (t) => (t === "failed" ? "error" : null),
		  title: (s) => (s.last_ok === false ? s.last_reason || "" : "") },
		{ field: "next_run_at", label: "Next run" },
		{ label: "", actions: (s) => {
			const run = document.createElement("button");
			const toggle = document.createElement("button");
			const del = document.createElement("button");

			run.type = "button";
			run.textContent = "Run now";
			gateAction(run, "runSchedule");
			run.addEventListener("click", async () => {
				try {
					await apiRequest("POST", CIX_API.runSchedule(s.name), {});
					showStatus("Ran " + s.name + ".", false);
				} catch (e) {
					showStatus("Could not run " + s.name + ": " + e.message, true);
				}
				refreshSchedules();
			});

			toggle.type = "button";
			toggle.textContent = s.enabled === false ? "Enable" : "Disable";
			gateAction(toggle, "setSchedule");
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

			del.type = "button";
			del.className = "button-danger";
			del.textContent = "Delete";
			gateAction(del, "deleteSchedule");
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
			return [run, toggle, del];
		} },
	], { op: "listSchedules", what: "schedules", empty: "Nothing is scheduled." });
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
	let isoBuildButton;

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

		/* The generated form's own submit (#592): disabled while a build
		 * runs, and otherwise by the session gate alone, which this must
		 * not override in the other direction. */
		isoBuildButton = document.querySelector("#iso-form button[type=submit]");
		if (isoBuildButton !== null)
			isoBuildButton.disabled = s.state === "building" || !sessionMay("postSystemIso");
		document.getElementById("iso-publish").disabled = s.state !== "ready";
		/*
		 * #428: what this host has PUBLISHED, read from its own
		 * endpoint rather than from the status above. Two resources --
		 * the status is the ISO on this box, this is the ones that
		 * left it -- so they are two requests and one panel, not one
		 * request carrying a growing array (ADR-0272's split, the same
		 * as /pipeline vs /pipeline/runs).
		 *
		 * The gap is the point of showing it: a box can hold a ready
		 * ISO built at one release while the newest published is many
		 * releases older, and nothing said so (measured on
		 * 192.168.15.95, 2026-10-07: built 0.2.57-417, newest
		 * published 0.2.57-406). Publishing is the operator's act
		 * (ADR-0324 -- never push by default), so this surfaces the
		 * gap rather than closing it behind their back.
		 */
		const pub = await apiRequest("GET", CIX_API.getSystemIsoPublished());
		/*
		 * Inside the dataChanged() guard above on purpose: a publish
		 * changes publish_state and published_name in the status, so
		 * the guard opens exactly when this list has something new to
		 * show, and a quiet box re-renders neither.
		 */
		const entries = Array.isArray(pub.published) ? pub.published : [];

		if (entries.length === 0) {
			/* Never "nothing exists": this is what THIS host recorded,
			 * and a repository may hold ISOs from before the record. */
			box.appendChild(fieldBlock("Published", "none recorded by this host"));
		} else {
			box.appendChild(fieldBlock("Last published",
			    entries[0].version + " — " + entries[0].name +
			    " (" + entries[0].repository + ")"));
			if (s.built_version && entries[0].version !== s.built_version)
				box.appendChild(fieldBlock("Not published",
				    "the ISO on this box was built from " + s.built_version +
				    ", newer than anything published"));
			box.appendChild(fieldBlock("Publishes recorded", String(entries.length)));
		}
	} catch (e) {
		/* Best-effort, same as every other panel here. */
	}
}

document.getElementById("iso-publish").addEventListener("click", async () => {
	try {
		await apiRequest("POST", CIX_API.publishSystemIso(), {});
		showStatus("Publishing the installer to the artifact cache\u2026", false);
	} catch (e) {
		showStatus("Could not publish the ISO: " + e.message, true);
	}
	refreshIso();
});

/* Live while its page is open (ADR-0339 phase 2): stalls are a stream,
 * never announced. */
function startStallsLive() {
	startLiveRead("stalls", CIX_API.getStalls(), (data) => renderStalls(data.stalls || []));
}

/*
 * Generated (#592), from `Stall`, promoted out of GET /system/stalls'
 * inline items -- which is also what let `ts` say it is an instant, so
 * the cell no longer formats it by hand.
 *
 * `wchan` and `activity` stay derived because their empty value is a
 * STATEMENT, not an absence: the contract says an empty `wchan` "means
 * it was running", and an empty activity means no request was being
 * served. "—" would say "unknown" about both, which is wrong. The empty
 * sentence is the good case, worth saying rather than leaving a blank
 * table that reads as broken.
 */
function renderStalls(stalls) {
	renderTable(document.getElementById("stalls"), "Stall", stalls, [
		{ field: "ts", label: "When" },
		"event",
		{ label: "Quiet for", text: (s) => s.seconds + "s" },
		"state",
		{ label: "wchan", text: (s) => s.wchan || "(running)" },
		{ label: "Activity", text: (s) => s.activity || "(idle)" },
	], {
		op: "getStalls",
		what: "stall records",
		empty: "None recorded — the loop has not gone quiet",
	});
}

/* ---------- Kernel Line (issue #65) ---------- */

function renderKernelPolicy(data) {
	const grid = document.getElementById("kpf-status");

	syncForm(document.getElementById("kpf-form"), "setKernelPolicy", {
		values: { channel: data.channel || "pinned" }, submit: "Save",
		failed: "Failed to set kernel channel",
		after: (body, res) => {
			showStatus("Kernel channel set to " + res.channel + ".", false);
			renderKernelPolicy(res);
		},
	});

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
	                                 : "fetched " + formatInstant(data.releases_fetched_at)));
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

async function refreshBootConsole() {
	try {
		const data = await apiRequest("GET", CIX_API.getBootConsole());
		const cfg = data.config || {};
		const grid = document.getElementById("bcf-effect");

		/* One console per line: a console carries commas of its own
		 * (`ttyS0,115200n8`), so a comma list could not hold one. */
		syncForm(document.getElementById("bcf-form"), "setBootConsole", {
			values: { consoles: cfg.consoles || [], extra: cfg.extra || "" },
			submit: "Save", failed: "Could not save the boot console",
			after: async (body, res) => {
				showStatus("Boot console saved — " + (res.loader_entries_updated || 0) +
				           " loader entry/entries rewritten; takes effect at the next boot.", false);
				await refreshBootConsole();
			},
		});
		/* #432: an unchanged panel is left alone -- rebuilding it on every
		 * tick collapses the page and costs the reader their place. The
		 * table below has its own guard now, in renderTable. */
		if (!unchangedAndRendered(grid, "boot-console-effect", cfg.rendered)) {
			grid.textContent = "";
			grid.appendChild(fieldBlock("Rendered", cfg.rendered || FIELD_ABSENT));
		}
		renderBootEntries(data.loader_entries || []);
	} catch (e) {
		/* The read failed, which listState() now knows -- so the table
		 * says why instead of keeping the "Reading…" it was born with,
		 * which is what this best-effort catch used to leave on a first
		 * failure, indefinitely. */
		renderBootEntries([]);
	}
}

/* Generated (#592), from `BootLoaderEntry`. The empty sentence is not
 * an error: a dev daemon has no ESP, and saying so beats an empty table
 * that looks like something failed. */
function renderBootEntries(entries) {
	renderTable(document.getElementById("boot-entries"), "BootLoaderEntry", entries, [
		{ field: "entry", label: "Loader entry" },
		{ field: "options", label: "Options" },
	], {
		op: "getBootConsole",
		what: "loader entries",
		empty: "No loader entries visible from here — this is not an installed host",
	});
}

/* ---------- Control Plane Reservation (issue #86) ---------- */

async function refreshControlPlaneReservation() {
	try {
		const c = await apiRequest("GET", CIX_API.getControlPlaneReservation());
		const grid = document.getElementById("cpr-effect");

		syncForm(document.getElementById("cpr-form"), "setControlPlaneReservation", {
			values: c, submit: "Save",
			saved: "Control plane reservation saved — the workload ceiling is in effect now.",
			failed: "Could not save reservation", after: refreshControlPlaneReservation,
		});
		/* #432: an unchanged panel is left alone -- rebuilding it on every
		 * tick collapses the page and costs the reader their place. */
		if (unchangedAndRendered(grid, "control-plane-reservation", c))
			return;
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

/* ---------- LDAP Config: start_uid/start_gid auto-allocation floor (task #748) ---------- */


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
		/*
		 * The whole LDAP config, not only the allocation floors the
		 * hand-built form offered (#592) -- and CHANGED-ONLY, because
		 * this PUT is not idempotent for every value: echoing the
		 * server_* listener fields back unchanged would take the
		 * listeners into the daemon's management for good and rewrite
		 * every registered server's config. What the operator did not
		 * touch is not sent.
		 */
		syncForm(document.getElementById("ldap-config-form"), "updateLdapConfig", {
			values: config, secrets: { bind_password: config.bind_password_set === true },
			changedOnly: true, submit: "Save", saved: "LDAP config saved",
			failed: "Failed to save LDAP config", after: refreshLdapConfig,
		});
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

/* ---------- NTP (tasks #751-755) ---------- */

/* ---- ESP: loader config and boot entries (ADR-0202, issue #128) ---- */


/* Generated (#592). `is_running_slot` is a badge rather than a bare
 * "yes": it is the one row an operator must not delete, and the
 * daemon refuses it, so the table says so in the status vocabulary
 * rather than in a column of yeses. */
function renderEspEntries(data) {
	renderTable(document.getElementById("esp-entries"), "EspEntry",
	    data.entries || [], [
		{ field: "name", label: "Entry" },
		"title",
		{ field: "matches_default", label: "Matches default" },
		{ field: "is_running_slot", label: "Running slot",
		  badge: (v) => (v ? "ok" : "unknown") },
		{ label: "", actions: (e) => {
			const rm = document.createElement("button");

			rm.textContent = "Delete";
			rm.className = "button-danger button-small";
			/* The daemon refuses to delete the running slot's own
			 * entry; disabling it here makes that a visible rule
			 * rather than a surprise 409. gateAction only ever
			 * disables, so the order of these two does not matter. */
			rm.disabled = !!e.is_running_slot;
			gateAction(rm, "deleteEspEntry");
			if (e.is_running_slot)
				rm.title = "This is the entry the running system booted from";
			rm.addEventListener("click", () => removeEspEntry(e.name));
			return [rm];
		} },
	], { op: "getEsp", what: "loader entries", empty: "No loader entries" });
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

		/* #432: an unchanged panel is left alone -- rebuilding it on every
		 * tick collapses the page and costs the reader their place. */
		if (unchangedAndRendered(box, "signing-keys", data))
			return;
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

async function clearSigningKeys() {
	if (!confirm("Erase this host's Secure Boot signing key pair? All three files are deleted, "
	             + "and this host cannot sign installer media again until a pair is installed. "
	             + "If no copy exists elsewhere, the pair is gone."))
		return;
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

		/* #432: an unchanged panel is left alone -- rebuilding it on every
		 * tick collapses the page and costs the reader their place. */
		if (unchangedAndRendered(box, "release-key", data))
			return;
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

async function clearReleaseKey() {
	if (!confirm("Erase this host's release-signing key? Published signatures stay verifiable, "
	             + "but this host cannot sign another release. If no copy exists elsewhere, the "
	             + "key is gone."))
		return;
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
		/* #432: the ESP summary is rebuilt on every tick. Guarded on
		 * the payload; refreshBootManager() below is deliberately
		 * OUTSIDE it, because that is a separate fetch with its own
		 * freshness and skipping it here would freeze that panel. */
		if (unchangedAndRendered(box, "esp", data)) {
			await refreshBootManager();
			return;
		}
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
		/* A pattern matching no entry comes back 409 -- that guard is
		 * the whole point of the endpoint, and the failure line says it
		 * as the daemon put it. */
		syncForm(document.getElementById("esp-loader-form"), "putEsp", {
			values: { default: data.default, timeout: data.timeout }, submit: "Save",
			saved: "Loader configuration saved", failed: "Failed to save loader config",
			after: refreshEsp,
		});
		renderEspEntries(data);
		await refreshBootNext();
		await refreshBootManager();
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
	if (!confirm("Remove boot entry \"" + name + "\" from the ESP? The host refuses to remove the "
	             + "last entry for the slot it is running from."))
		return;
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


/*
 * The ESP's own \EFI\BOOT\BOOTX64.EFI (#467/#468, ADR-0290). Unlike
 * every other control on this page, there is no per-slot fallback for
 * what this writes -- both A/B slots share the one boot manager binary
 * -- so this is the one action on this tab styled and confirmed as
 * destructive even though the daemon itself keeps a same-directory
 * backup (issue #468: "an explicit, hard-to-misclick confirm step
 * given the stakes ADR-0290 describes").
 */
async function refreshBootManager() {
	const body = document.getElementById("boot-manager-body");

	try {
		const r = await apiRequest("GET", CIX_API.getBootManager());

		body.textContent = "";
		body.appendChild(fieldBlock("Size", r.size + " bytes"));
		body.appendChild(fieldBlock("SHA-256", r.sha256));
		body.appendChild(fieldBlock("Backup exists", r.has_backup ? "yes" : "no"));
	} catch (e) {
		body.textContent = "";
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 2;
		cell.className = "empty";
		cell.textContent = "No boot manager binary found at the expected ESP path.";
		row.appendChild(cell);
		body.appendChild(row);
	}
}


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

async function refreshDnsForwarders() {
	try {
		const config = await apiRequest("GET", CIX_API.getDnsForwarders());

		cache.dnsForwarders = config.forwarders;
		syncForm(document.getElementById("dns-forwarders-form"), "setDnsForwarders", {
			values: config, submit: "Save", failed: "Failed to save forwarders",
			/* Empty is a legitimate setting, not a no-op, so say which
			 * happened rather than a generic "saved". */
			after: async (body) => {
				showStatus((body.forwarders || []).length === 0
				                   ? "Forwarders cleared -- DNS is authoritative-only"
				                   : "Forwarders saved and applied to every registered server",
				           false);
				await refreshDnsForwarders();
			},
		});
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

/* ---- NTP Config: upstream server address list ---- */

async function refreshNtpConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getSystemNtp());

		cache.ntpConfig = config;
		syncForm(document.getElementById("ntp-config-form"), "putSystemNtp", {
			values: config, submit: "Save", saved: "NTP config saved",
			failed: "Failed to save NTP config", after: refreshNtpConfig,
		});
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

/* ---- NTP Servers: registered container time sources ---- */

/*
 * Generated (#592). `listNtpServers` has `response: null` in the
 * contract -- this list's shape is described nowhere -- so the schema
 * name finds no fields and the cell falls back to the value as text
 * and the column to its de-cased name. That is the right behaviour
 * and the wrong contract; the gap is worth closing, and the table
 * does not have to wait for it.
 */
function renderNtpServers(servers) {
	renderTable(document.getElementById("ntp-servers"), "NtpServerBinding", servers, [
		"container",
		{ label: "", actions: (s) => [unregisterButton("deleteNtpServer", "NTP server " + s.container,
		    () => removeNtpServer(s.container))] },
	], { op: "listNtpServers", what: "NTP servers", empty: "No NTP server bindings" });
}

/* The Unregister control these registration lists share, so the four
 * of them do not each build the same gated danger button -- and the one
 * place its confirm lives. `what` names the binding ("DNS server
 * dns-1"); every unregister operation's contract says it does not touch
 * the container itself, so the question says so. */
function unregisterButton(opId, what, onClick) {
	const b = document.createElement("button");

	b.textContent = "Unregister";
	b.className = "button-danger button-small";
	gateAction(b, opId);
	b.addEventListener("click", () => {
		if (confirm("Unregister " + what + "? The container itself is not touched."))
			onClick();
	});
	return b;
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

/* ---- Syslog forward targets: registered container log receivers
 * (ADR-0127) -- same shape as NTP Servers above, mirrored exactly. */

/* Generated (#592), the same shape as the NTP list above. */
function renderSyslogTargets(targets) {
	renderTable(document.getElementById("syslog-targets"), "SyslogTarget", targets, [
		"container",
		{ label: "", actions: (t) => [unregisterButton("deleteSyslogTarget", "syslog target " + t.container,
		    () => removeSyslogTarget(t.container))] },
	], {
		op: "listSyslogTargets",
		what: "syslog targets",
		empty: "No syslog forward targets registered",
	});
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


/* ---- TLS Throttle: per-source-IP HTTPS-handshake-failure throttling
 * (ADR-0134) ---- */

async function refreshTlsThrottleConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getSystemTlsThrottle());

		cache.tlsThrottleConfig = config;
		syncForm(document.getElementById("ttf-form"), "putSystemTlsThrottle", {
			values: config, submit: "Save", saved: "TLS throttle config saved",
			failed: "Failed to save TLS throttle config", after: refreshTlsThrottleConfig,
		});
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

/*
 * Generated (#592), and every one of its six columns is a plain field,
 * which it could not have been before `format: unix-seconds`.
 *
 * Both "blocked until" cells were guarded by hand -- `e.blocked &&
 * e.blocked_until ? ... : ""` -- because the daemon reports 0 for a
 * source that is not blocked, and 0 as a date is 1970. That rule is
 * the format's now (`formatInstant` treats 0 as absent), so the cells
 * say nothing at all, and they also gained the DATE: both printed
 * `toLocaleTimeString()`, which showed 09:04 for a block that lifts
 * tomorrow morning.
 */
function renderTlsThrottleStatus(entries) {
	renderTable(document.getElementById("tls-throttle-status"),
	    "TlsThrottleStatusEntry", entries, [
		{ field: "ip", label: "IP" },
		{ field: "fail_count", label: "Failed handshakes" },
		"blocked",
		{ field: "blocked_until", label: "Blocked until" },
		/* #547: the authentication class -- failed logins and app
		 * passwords from this address, and its own block. */
		{ field: "auth_fail_count", label: "Failed logins" },
		{ field: "auth_blocked_until", label: "Logins blocked until" },
	], {
		op: "getSystemTlsThrottleStatus",
		what: "throttled sources",
		empty: "No sources currently tracked",
	});
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
	/* The time to set, offered as THIS browser's clock and refreshed on
	 * every read until the operator types (#592) -- the hand-built form
	 * used the browser's clock for a blank field, invisibly; here it is
	 * on screen, and is what Save sends. */
	syncForm(document.getElementById("ntp-time-form"), "putSystemTime", {
		values: { unixtime: Math.floor(Date.now() / 1000) }, submit: "Set host clock",
		saved: "Host clock set", failed: "Failed to set host clock", after: refreshNtpTime,
	});
}

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

/*
 * Generated (#592). `sans` needs nothing: it is `type: array` in the
 * contract, so `fieldText` already joins it and already says "—" for
 * an empty one, where the hand-written cell said "".
 *
 * `owner` keeps `formatOwner()` and is the borderline case for the
 * `text` hatch -- it is one field, which the hatch is not for, but
 * what it renders is a RESERVED SENTINEL (`__host`, never a real
 * container name, ADR-0128) and which strings are sentinels is daemon
 * knowledge. formatOwner is the one place that knowledge lives, so the
 * cell defers to it rather than restating it.
 */
function renderPkiCerts(certs) {
	renderTable(document.getElementById("pki-certs"), "PkiCert", certs, [
		"name",
		"serial",
		"not_after",
		{ field: "sans", label: "SANs" },
		{ label: "Owner", text: (c) => formatOwner(c.owner) },
		{ label: "", actions: (c) => {
			const rm = document.createElement("button");

			rm.textContent = "Remove";
			rm.className = "button-danger";
			gateAction(rm, "deletePkiCert");
			rm.addEventListener("click", () => removePkiCert(c.name));
			return [rm];
		} },
	], {
		op: "listPkiCerts",
		what: "certificates",
		empty: "No certificates issued",
	});
}

async function refreshPkiCerts() {
	const data = await apiRequest("GET", CIX_API.listPkiCerts());
	cache.pkiCerts = data.certs;
	renderPkiCerts(cache.pkiCerts);
}

async function removePkiCert(name) {
	if (!confirm("Remove certificate \"" + name + "\"? Its key and certificate files are deleted."))
		return;
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
/*
 * THE STATE OF A PACKAGE ENTRY, as a column, because three tables show
 * it and all three showed it identically (#592).
 *
 * Issue #101's rule is the whole of it: a package that could not be
 * REACHED and one that failed to BUILD both read "failed", and they
 * call for opposite responses -- retry the first, fix the second. So
 * the kind sits next to the state in the cell and the message is the
 * hover, which keeps the table readable at seven columns.
 *
 * A column rather than three copies of one, for the reason the
 * renderer exists: this is the vocabulary a reader learns once, and
 * three definitions of it are three places for it to drift.
 */
function pkgStateColumn() {
	return {
		label: "State",
		text: (p) => (p.stage
			? p.state + " (" + p.stage + "/" + p.status + ")"
			: p.state),
		title: (p) => p.error,
	};
}

/*
 * Generated (#592), and the first caller of the `title` modifier.
 *
 * The repaint guard is the renderer's now: #432's reason for it (a
 * rebuild on every tick costs the reader their scroll position) is
 * exactly why renderTable has one, and it keys on `op` so this caller
 * no longer names a cache slot or composes its own signature.
 */
function renderPackagesList() {
	renderTable(document.getElementById("packages"), "PkgEntry", cache.pkgList, [
		{ field: "name", link: (p) => "#packages/" + encodeURIComponent(p.name) },
		"image",
		"version",
		pkgStateColumn(),
		{ label: "", actions: (p) => {
			if (p.state !== "installed")
				return [];

			const rm = document.createElement("button");

			rm.textContent = "Remove";
			rm.className = "button-danger";
			gateAction(rm, "deletePkg");
			rm.addEventListener("click", () => removePkg(p.name, p.image));
			return [rm];
		} },
	], {
		op: "listPkg",
		what: "installed packages",
		empty: "Nothing installed yet",
	});
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

	/*
	 * ONE ROW PER NAME, carrying that name's newest version -- the
	 * grouping and the filter stay here, because they are this
	 * screen's judgement and not the contract's: `GET /pkg/recipes`
	 * returns every version as its own entry (ADR-0107) and the
	 * renderer renders rows it is given.
	 *
	 * `extra` is the filter, so a search that narrows to the same
	 * number of rows still repaints; the guard compares data, and two
	 * different filters can select identical-looking lists.
	 */
	const rows = names.map((n) => groups.get(n)[0]);

	renderTable(document.getElementById("recipes"), "PkgRecipe", rows, [
		{ field: "name", link: (r) => "#packages/" + encodeURIComponent(r.name) },
		"version",
		"depends",
		{ label: "", actions: (r) => {
			const rm = document.createElement("button");

			rm.textContent = "Delete";
			rm.className = "button-danger";
			gateAction(rm, "deletePkgRecipe");
			rm.addEventListener("click", () => removePkgRecipe(r.name, r.version));
			return [rm];
		} },
	], {
		op: "listPkgRecipes",
		what: "recipes",
		/* A filter that matches nothing is not an empty host: the read
		 * succeeded and there are recipes, just not these. The other
		 * three states still come from listState(), so a refused read
		 * says so even with a filter typed. */
		empty: cache.pkgRecipes.length > 0 ? "No match" : "No recipes",
		extra: filter,
	});
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
			contentEl.textContent = "Reading…";
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
		openPkgRecipeForm(recipe ? "Edit recipe " + name : "Add a recipe for " + name,
		                  name, content);
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
/*
 * Generated (#592), and the rolling-candidate mark becomes a badge in
 * its own column rather than a `<strong>` appended inside the version
 * cell -- the vocabulary's widget for "a value the daemon reported",
 * which is exactly what this is.
 *
 * WHAT IT MARKS IS WORTH KEEPING: the DAEMON's own ADR-0107
 * resolution, cached from the name-only recipe GET, never a
 * client-side guess. The first version of this mark sat on the
 * newest-published row (`i === 0` after the created_at sort) on the
 * assumption that versions are only ever published in increasing
 * order, and that broke the session it shipped -- gcc published
 * 6.4.0-5 after 16.2.0-5, the daemon resolves 16.2.0-5, and the mark
 * sat on 6.4.0-5 until the owner asked what the criteria actually
 * were. Until the resolution has loaded there is no mark at all: an
 * honest blank over a guess, and the post-load re-render fills it in.
 */
function renderPackageDetailVersions(name, allVersions) {
	renderTable(document.querySelector("#pkgd-versions"), "PkgRecipe",
	    [...allVersions].sort((a, b) => b.created_at - a.created_at), [
		"version",
		{ label: "Rolling",
		  text: (v) => (pkgRecipeContentCache.name === name &&
		                v.version === pkgRecipeContentCache.version
			? "candidate" : ""),
		  badge: () => "ok" },
		"changelog",
		{ field: "created_at", label: "Created" },
		{ label: "", actions: (v) => {
			const view = document.createElement("button");
			const rm = document.createElement("button");

			view.textContent = "View…";
			view.addEventListener("click", () => viewPkgRecipeVersion(v.name, v.version));

			rm.textContent = "Delete";
			rm.className = "button-danger";
			gateAction(rm, "deletePkgRecipe");
			rm.addEventListener("click", () => removePkgRecipe(v.name, v.version));
			return [view, rm];
		} },
	], {
		op: "listPkgRecipes",
		key: "package-detail-versions",
		extra: name,
		what: "this package's recipe versions",
		empty: "No recipe on file for this name",
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
			await apiRequest("GET", CIX_API.getPkgRecipe(name)
			                 + "?version=" + encodeURIComponent(version))
		).content;
	} catch (e) {
		showStatus("Failed to load recipe content for " + name + "@" + version + ": " + e.message, true);
		return;
	}
	openPkgRecipeForm(name + "@" + version, name, content);
}

/* The recipe source a new package may be published to, read from the
 * box when the form opens (ADR-0324). The blank choice lets the host
 * decide: the package's owner, or the only source. */
async function recipeSourceChoices() {
	const data = await apiRequest("GET", CIX_API.listPkgSources());

	return { source: (data.sources || []).map((s) => ({ value: s.name })) };
}

/*
 * The one recipe form (#592), for the four places that publish one: the
 * header menu, the Recipes page, an image's page and a package's own.
 * Opened on a package, its name is FIXED -- an edit publishes a new
 * version of that package, and typing over the name would publish a
 * different package rather than rename this one.
 */
async function openPkgRecipeForm(title, name, content) {
	let choices;

	try {
		choices = await recipeSourceChoices();
	} catch (e) {
		showStatus("Could not read the recipe sources: " + e.message, true);
		return;
	}
	openForm("addPkgRecipe", {
		title: title,
		submit: "Publish",
		fixed: name ? { name: name } : {},
		values: content ? { content: content } : {},
		choices: choices,
		after: afterPkgRecipeSave,
		failed: "Failed to publish the recipe",
	});
}

async function afterPkgRecipeSave(body) {
	if (pkgRecipeContentCache.name === body.name)
		pkgRecipeContentCache = { name: null, content: null };
	await refreshPkgRecipes();
	renderTree();
}

/*
 * Generated (#592), and the conversion found a REAL BUG in its own
 * last column: the Remove button was gated on `deletePkgRecipe` while
 * the thing it calls is `removePkg`. So the control a session is
 * allowed to use and the request it sends were decided by two
 * different permissions -- a session that may remove a package but not
 * a recipe saw it disabled, and one with the reverse saw it enabled
 * and got a 403 on the click. That is exactly what #548's gating
 * exists to prevent, and the identical button two tables away is
 * gated on `deletePkg`. Fixed here.
 *
 * `files` is a count rather than the field, because `fieldText` on
 * `type: array` joins it -- and this array is every path the package
 * installed, which for coreutils is hundreds.
 *
 * Also gains its other three states, as the image's own package table
 * does: this is a filtered view of a read that can be refused.
 */
function renderPackageDetailInstalled(name) {
	renderTable(document.querySelector("#pkgd-installed"), "PkgEntry",
	    cache.pkgList.filter((p) => p.name === name), [
		{ field: "image", link: (p) => "#images/" + encodeURIComponent(p.image) },
		"version",
		pkgStateColumn(),
		{ field: "available_version", label: "Available" },
		"error",
		{ label: "Files", text: (p) => String((p.files || []).length) },
		{ label: "", actions: (p) => {
			if (p.state !== "installed")
				return [];

			const rm = document.createElement("button");

			rm.textContent = "Remove";
			rm.className = "button-danger";
			gateAction(rm, "deletePkg");
			rm.addEventListener("click", async () => {
				await removePkg(p.name, p.image);
				renderPackageDetailInstalled(name);
			});
			return [rm];
		} },
	], {
		op: "listPkg",
		key: "package-detail-installed",
		extra: name,
		what: "installed packages",
		empty: "Not installed on any image",
	});
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
/*
 * The query used to be a literal "?version=%s" -- left by the
 * 2026-08-31 conversion to generated paths (34b5647d), which turned
 * `"?version=" + encodeURIComponent(version)` into a printf placeholder
 * nothing fills. The box answers that 404 "no such recipe" (measured
 * 2026-10-17), so removing or viewing one recipe version from the
 * dashboard failed for seven weeks.
 */
/*
 * An image's recipe is removed from two places -- the catalogue's list
 * and the image's own Recipe tab -- and each had its own copy of this,
 * neither asking first. One copy now, which asks; the caller redraws
 * whatever it shows. Resolves true when the recipe went.
 */
async function removeImageRecipe(name) {
	if (!confirm("Delete the image recipe for " + name + "? The image and anything installed "
	             + "through it are not affected."))
		return false;
	try {
		await apiRequest("DELETE", CIX_API.deleteImageRecipe(name));
		showStatus("Deleted the image recipe for " + name);
		imageRecipeContentCache = { name: null, content: null };
		return true;
	} catch (e) {
		showStatus("Failed to remove the image recipe for " + name + ": " + e.message, true);
		return false;
	}
}

async function removePkgRecipe(name, version) {
	if (!confirm("Remove recipe " + name + "@" + version + "? Anything already installed from it "
	             + "stays installed."))
		return;
	try {
		await apiRequest("DELETE", CIX_API.deletePkgRecipe(name)
		                 + "?version=" + encodeURIComponent(version));
		showStatus("Removed recipe " + name + "@" + version);
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
	const filter = document.getElementById("recipes-image-search").value.trim().toLowerCase();
	const rows = cache.imageRecipes.filter((r) => r.name.toLowerCase().includes(filter));

	/* Generated (#592). The FILTER goes in `extra` for the reason the
	 * hand-written guard put it in its signature: it is an input to
	 * this render as much as the data is, and guarding on the payload
	 * alone freezes the table while someone types. */
	renderTable(document.getElementById("image-recipes"), "ImageRecipe", rows, [
		{ field: "name", label: "Image",
		  link: (r) => "#images/" + encodeURIComponent(r.name) },
		{ label: "", actions: (r) => {
			const apply = document.createElement("button");
			const edit = document.createElement("button");
			const rm = document.createElement("button");

			apply.textContent = "Apply";
			gateAction(apply, "applyImageRecipe");
			apply.addEventListener("click", async () => {
				try {
					const result = await apiRequest("POST",
					    CIX_API.applyImageRecipe(r.name));

					clearStatus();
					showStatus(imageApplySummary(r.name, result), false);
				} catch (e) {
					showStatus("Failed to apply recipe for " + r.name +
					    ": " + e.message, true);
				}
			});

			edit.textContent = "Edit";
			gateAction(edit, "addImageRecipe");
			edit.addEventListener("click", async () => {
				let content = "";

				try {
					content = await loadImageRecipeContent(r.name);
				} catch (e) {
					/* Shouldn't happen for a name the list just returned. */
				}
				openForm("addImageRecipe", {
					title: "Edit image recipe",
					submit: "Save",
					fixed: { name: r.name },
					values: { content: content },
					after: afterImageRecipeSave,
					failed: "Failed to save the image recipe for " + r.name,
				});
			});

			rm.textContent = "Delete";
			rm.className = "button-danger";
			gateAction(rm, "deleteImageRecipe");
			rm.addEventListener("click", async () => {
				if (await removeImageRecipe(r.name))
					await refreshImageRecipesList();
			});
			return [apply, edit, rm];
		} },
	], {
		op: "listImageRecipes",
		what: "image recipes",
		empty: cache.imageRecipes.length > 0 ? "No match" : "No image recipes",
		extra: filter,
	});
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
	const filter = document.getElementById("recipes-container-search").value.trim().toLowerCase();
	const rows = cache.containerRecipes.filter((r) => r.name.toLowerCase().includes(filter));

	/* Generated (#592), with the filter in `extra` as the image
	 * catalogue's is. */
	renderTable(document.getElementById("container-recipes"), "Deployment", rows, [
		"name",
		{ label: "", actions: (r) => {
			const apply = document.createElement("button");
			const edit = document.createElement("button");
			const rm = document.createElement("button");

			apply.textContent = "Apply…";
			gateAction(apply, "applyDeployment");
			apply.addEventListener("click", () => {
				openForm("applyDeployment", {
					title: "Apply recipe: " + r.name,
					submit: "Apply -- create container",
					args: [r.name],
					after: (b, result) => afterContainerRecipeApply(r.name, result),
					failed: "Failed to apply recipe " + r.name,
				});
			});

			edit.textContent = "Edit";
			gateAction(edit, "addDeployment");
			edit.addEventListener("click", async () => {
				let content = "";

				try {
					content = await loadContainerRecipeContent(r.name);
				} catch (e) {
					/* Shouldn't happen for a name the list just returned. */
				}
				openContainerRecipeForm("Edit container recipe " + r.name, r.name, content);
			});

			rm.textContent = "Delete";
			rm.className = "button-danger";
			gateAction(rm, "deleteDeployment");
			rm.addEventListener("click", async () => {
				if (!confirm("Delete the container recipe for " + r.name + "? A container already "
				             + "created from it is not affected."))
					return;
				try {
					await apiRequest("DELETE", CIX_API.deleteDeployment(r.name));
					showStatus("Deleted the container recipe for " + r.name);
					containerRecipeContentCache = { name: null, content: null };
					await refreshContainerRecipesList();
				} catch (e) {
					showStatus("Failed to remove container recipe for " +
					    r.name + ": " + e.message, true);
				}
			});
			return [apply, edit, rm];
		} },
	], {
		op: "listDeployments",
		what: "container recipes",
		empty: cache.containerRecipes.length > 0 ? "No match" : "No container recipes",
		extra: filter,
	});
}

document.getElementById("recipes-pkg-search").addEventListener("input", renderRecipesList);
document.getElementById("recipes-image-search").addEventListener("input", renderImageRecipesTable);
document.getElementById("recipes-container-search").addEventListener("input", renderContainerRecipesTable);

document.getElementById("recipes-pkg-add").addEventListener("click", () => openPkgRecipeForm("Add a recipe"));
document.getElementById("recipes-image-add").addEventListener("click", () => {
	openForm("addImageRecipe", {
		title: "Add image recipe",
		submit: "Add",
		after: afterImageRecipeSave,
		failed: "Failed to add the image recipe",
	});
});
/*
 * The container recipe form (#592): one generated form for adding a
 * recipe and for editing one. On an edit the name is FIXED -- it is also
 * the container the recipe creates, so typing over it would write a
 * different recipe rather than rename this one. The content field is a
 * document, so it can be filled from a file.
 */
function openContainerRecipeForm(title, name, content) {
	openForm("addDeployment", {
		title: title, submit: "Save",
		fixed: name ? { name: name } : {},
		values: content ? { content: content } : {},
		failed: "Failed to save the container recipe",
		after: async () => {
			containerRecipeContentCache = { name: null, content: null };
			await refreshContainerRecipesList();
		},
	});
}

document.getElementById("recipes-container-add").addEventListener("click",
	() => openContainerRecipeForm("Add container recipe"));

/*
 * Which of an apply's two successes happened, said out loud.
 *
 * 201 creates the container; 202 queues it behind an image that is not
 * built yet and creates it when that image is realized (ADR-0270). The
 * hand-written form this replaces reported "created" for both, which
 * was simply false in the second case -- and that case is the one an
 * operator most needs to be told about, because nothing appears in the
 * container list for a while and the apply looks like it did nothing.
 *
 * The discriminator is the contract's own `state`, a declared enum of
 * one value, rather than the absence of some field on the 201 body.
 */
async function afterContainerRecipeApply(name, result) {
	if (result !== null && result.state === "awaiting-image")
		showStatus("Recipe " + name + " is queued: image " +
			(result.awaiting_image || "it names") + " is being built, and the " +
			"container is created once it is ready (ADR-0270).", false);
	else
		showStatus("Container " + name + " created from recipe.", false);
	await refreshContainers();
	renderTree();
}

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

	if (!confirm("Remove package \"" + name + "\" from image \"" + (image || "base") + "\"? Any "
	             + "files it installed leave the image, as a new image version."))
		return;
	try {
		await apiRequest("DELETE", CIX_API.deletePkg(key));
		showStatus("Removed package " + key);
		await refreshPkgList();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove package " + key + ": " + e.message, true);
	}
}

/* ---- Recipe sources (ADR-0324) + sync (ADR-0121) ---- */

/*
 * ADR-0324: recipe sources, and what is held because more than one
 * source offers it. One list table per resource; add and edit are the
 * contract's two operations, each a generated form (#592).
 *
 * Committing to a source is a deploy to every host that follows it,
 * and trusting its keys lets it vouch for packages (ADR-0323/0324), so
 * turning either ON is confirmed -- and only turning it on: a save that
 * leaves an already-trusted source trusted asks nothing. `prior` is the
 * source as it stood, or null for a new one.
 */
function confirmPkgSourceTrust(body, prior) {
	const url = body.url || (prior ? prior.url : "");

	if (body.write && !(prior && prior.write) &&
	    !confirm("Let this host commit the recipes it writes to " + url +
	             "? Every host that syncs from it will pick them up."))
		return false;
	if (body.trust_keys && !(prior && prior.trust_keys) &&
	    !confirm("Trust the signing keys " + url +
	             " carries? Packages signed by them will be accepted as genuine."))
		return false;
	return true;
}

function editPkgSource(source) {
	openForm("updatePkgSource", {
		title: "Edit source " + source.name,
		submit: "Save",
		args: [source.name],
		values: source,
		secrets: { token: source.token_set === true },
		before: (body) => confirmPkgSourceTrust(body, source),
		after: refreshPkgSources,
		failed: "Failed to save the recipe source " + source.name,
	});
}

/*
 * Generated (#592), and two things it stopped doing.
 *
 * THE ROLE WAS AN AMBER BADGE, and amber is "paused" -- the Primitives
 * board's four colours have a fixed meaning and are never repurposed,
 * and a source this host may write to is not paused, or a state at all.
 * It was also built as a bare `"badge " + ...` class string, past
 * statusBadge(), the one function that turns a kind into a badge. A
 * write role is a property the contract types as a boolean, so it reads
 * as one: "Writes: yes". The package repositories table did the same
 * with "push", and is the same now.
 *
 * FOUR FLAGS, FOUR VOCABULARIES. "write"/"read", "trusted"/"no",
 * "signed"/"unsigned" and "set"/"unset" under one-word headers became
 * yes/no under headers that ask the question, as every other boolean
 * on this dashboard reads. `catalogue_key` is a key, not a flag, so its
 * cell is derived: the question is whether one is set.
 */
function renderPkgSourcesTable(sources) {
	renderTable(document.getElementById("pkg-sources"), "PkgSource", sources, [
		"name",
		{ field: "url", label: "Repository" },
		{ field: "ref", label: "Ref" },
		{ field: "write", label: "Writes" },
		{ field: "trust_keys", label: "Trusts keys" },
		{ label: "Signed index", text: (s) => (s.catalogue_key ? "yes" : "no") },
		{ field: "token_set", label: "Token set" },
		{ label: "", actions: (s) => {
			const edit = document.createElement("button");
			const remove = document.createElement("button");

			edit.type = "button";
			edit.className = "button-small";
			edit.textContent = "Edit";
			edit.addEventListener("click", () => editPkgSource(s));
			gateAction(edit, "updatePkgSource");

			remove.type = "button";
			remove.className = "button-small button-danger";
			remove.textContent = "Remove";
			remove.addEventListener("click", async () => {
				if (!confirm("Remove recipe source " + s.name + "? Its packages stop syncing, and recipes already published stay."))
					return;
				try {
					await apiRequest("DELETE", CIX_API.deletePkgSource(s.name));
					showStatus("Removed recipe source " + s.name, false);
					await refreshPkgSources();
				} catch (e) {
					showStatus("Failed to remove " + s.name + ": " + e.message, true);
				}
			});
			gateAction(remove, "deletePkgSource");
			return [edit, remove];
		} },
	], { op: "listPkgSources", what: "recipe sources", empty: "No recipe sources" });
}

/* Generated (#592), from `PkgSourceConflict` -- promoted out of
 * PkgSourceOwnership's inline items for this table, which is also what
 * finally put a conflict's required keys in front of the
 * contract-vs-daemon gate (#605). */
function renderPkgSourceHeld(ownership) {
	renderTable(document.getElementById("pkg-source-held"), "PkgSourceConflict",
	    ownership.conflicts, [
		{ field: "item", label: "Item" },
		{ field: "offered_by", label: "Offered by" },
		{ label: "", actions: (c) => {
			const choose = document.createElement("button");

			choose.type = "button";
			choose.className = "button-small";
			choose.textContent = "Choose source";
			/* Generated (#592): the item is fixed, and the sources to
			 * choose from are the ones this row says offer it. */
			choose.addEventListener("click", () => openForm("setPkgSourceOwnership", {
				title: "Choose the source of " + c.item, submit: "Choose",
				fixed: { item: c.item },
				choices: { source: c.offered_by.split(", ").map((s) => ({ value: s })) },
				failed: "Failed to choose the source of " + c.item,
				after: async (body) => {
					showStatus(c.item + " now comes from " + body.source, false);
					await refreshPkgSources();
				},
			}));
			gateAction(choose, "setPkgSourceOwnership");
			return [choose];
		} },
	], {
		op: "getPkgSourceOwnership",
		what: "source ownership",
		empty: "Nothing is held",
	});
}

async function refreshPkgSources() {
	try {
		const data = await apiRequest("GET", CIX_API.listPkgSources());

		renderPkgSourcesTable(data.sources);
	} catch (e) {
		/* apiRequest() has recorded this failure against the op, and
		 * renderTable's listState() turns it into the refusal sentence,
		 * so an empty list is all either table needs here. Both used to
		 * write the sentence themselves through simpleTableRows -- a
		 * second copy of the decision, and one that skipped the
		 * renderer's guard. */
		renderPkgSourcesTable([]);
	}
	try {
		const ownership = await apiRequest("GET", CIX_API.getPkgSourceOwnership());

		renderPkgSourceHeld(ownership);
	} catch (e) {
		renderPkgSourceHeld({ conflicts: [] });
	}
}


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

async function refreshPkgCacheConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getPkgCacheConfig());

		cache.pkgCacheConfig = config;
		syncForm(document.getElementById("pcc-form"), "putPkgCacheConfig", {
			values: config, submit: "Save", saved: "Cache config saved",
			failed: "Failed to save cache config", after: refreshPkgCacheConfig,
		});
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

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


/* ---- ADR-0152: hostauth session listing/revoke -- real admin
 * visibility into a table that previously had none (no endpoint, no
 * CLI, no web panel) ---- */

/* The other credentialed read (#490) -- see refreshLdapUsers() for
 * why an unauthenticated tab must not ask. This one is called from a
 * view refresher rather than the global sweep, but the reasoning and
 * the cost are the same. */
/*
 * The session idle timeout, on the Sessions tab: the setting decides
 * when every row below it expires.
 *
 * PUT /system/hostauth-config replaces the whole config, and an
 * omitted ldap_* field reverts to its default -- disabled. So Save
 * reads the config afresh and writes every field back with only the
 * timeout changed, rather than sending the timeout alone, which would
 * have switched off LDAP login as a side effect. The fields copied are
 * the ones the PUT reads (api_hostauth.c); gating_active is computed
 * and not one of them.
 */
async function refreshHostauthConfig() {
	try {
		const c = await apiRequest("GET", CIX_API.getHostauthConfig());

		/*
		 * The whole host-auth config (#592), where the hand-built form
		 * offered only the idle timeout and copied the rest from a
		 * fresh read on every save -- because this PUT REPLACES the
		 * config, and an omitted LDAP setting falls back to off. The
		 * generated form sends every field, prefilled from the read, so
		 * a save changes what was changed and keeps the rest.
		 */
		syncForm(document.getElementById("hostauth-timeout-form"), "putHostauthConfig", {
			values: c, submit: "Save", failed: "Failed to save host authentication",
			/* 0 is legitimate but changes how this page itself works:
			 * every token is spent by its first request, so each action
			 * needs a new login. Worth one question before it lands. */
			before: (body) => body.idle_timeout_seconds !== 0 ||
				confirm("With an idle timeout of 0, every login is good for exactly one " +
				        "request, including this dashboard's. Set it anyway?"),
			after: async (body) => {
				showStatus("Host authentication saved; sessions now idle out after " +
				           body.idle_timeout_seconds + "s", false);
				await refreshHostauthConfig();
				await refreshHostauthSessions();
			},
		});
	} catch (e) {
		/* A refused read leaves the form as it was; the Save button
		 * already carries the session's reason when it cannot act. */
	}
}

async function refreshHostauthSessions() {
	try {
		const data = await apiRequest("GET", CIX_API.listHostauthSessions());

		renderHostauthSessions(data.sessions || []);
	} catch (e) {
		/* The panel says why, never "No active sessions", which is a
		 * claim nothing was asked about -- but it no longer says it
		 * from here. apiRequest() has already recorded this failure
		 * against `listHostauthSessions`, and renderTable reads it
		 * through listState(), so passing refusalText() in as the
		 * empty sentence was a second copy of the same decision --
		 * and it covered only the refused state, never "Reading…". */
		renderHostauthSessions([]);
	}
}

/*
 * Generated (#592). `source_ip` and `logged_in_at` are plain fields:
 * both are recorded at login and never moved by use, so a token
 * carried to another machine still shows where it was issued, and the
 * contract says each is null when unknown -- which "—" now states
 * where the hand-written cell said "-".
 *
 * `expires_in_seconds` keeps the `text` hatch, and this is the case
 * the hatch is for even though it reads one field: null does not mean
 * "unknown" here, it means EVERY TOKEN IS SINGLE-USE, which is a
 * sentence about how this host is configured and not a missing value.
 * A duration has no contract format and should not borrow the
 * instant's -- `expires_in_seconds` is a count, not a moment.
 */
function renderHostauthSessions(sessions) {
	renderTable(document.getElementById("hostauth-sessions"), "HostauthSession",
	    sessions, [
		"username",
		{ field: "source_ip", label: "From" },
		{ field: "logged_in_at", label: "Logged in" },
		{ label: "Expires in", text: (s) =>
			s.expires_in_seconds === null || s.expires_in_seconds === undefined
				? "single-use (no idle timeout configured)"
				: s.expires_in_seconds + "s" },
		{ label: "", actions: (s) => {
			const revoke = document.createElement("button");

			revoke.textContent = "Log out everywhere";
			revoke.className = "button-danger";
			gateAction(revoke, "revokeHostauthSessions");
			revoke.addEventListener("click", async () => {
				if (!confirm("Log " + s.username + " out of every session? Each must log in "
				             + "again."))
					return;
				try {
					await apiRequest("DELETE",
					    CIX_API.revokeHostauthSessions(s.username));
					showStatus("Logged " + s.username + " out everywhere");
					await refreshHostauthSessions();
				} catch (e) {
					showStatus("Failed to revoke sessions for " +
					    s.username + ": " + e.message, true);
				}
			});
			return [revoke];
		} },
	], {
		op: "listHostauthSessions",
		what: "active sessions",
		empty: "No active sessions",
	});
}

/* ---------- ADR-0317 (#544): group permissions ---------- */

async function refreshHostauthPermissions() {
	try {
		cache.hostauthPermissions = await apiRequest("GET", CIX_API.getHostauthPermissions());
		renderHostauthPermissions(cache.hostauthPermissions);
	} catch (e) {
		cache.hostauthPermissions = null;
		renderHostauthPermissions(null, refusalText(e, "group permissions"));
	}
}

function renderHostauthPermissions(data, emptyText = "No group holds any permission") {
	const body = document.getElementById("hostauth-permissions-body");
	const groups = data && data.groups ? Object.keys(data.groups) : [];

	body.textContent = "";
	if (groups.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 3;
		cell.className = "empty";
		cell.textContent = emptyText;
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const group of groups) {
		const words = data.groups[group] || [];
		const row = document.createElement("tr");
		const nameCell = document.createElement("td");
		const wordsCell = document.createElement("td");
		const actionCell = document.createElement("td");
		const editButton = document.createElement("button");
		const rmButton = document.createElement("button");

		nameCell.textContent = group;
		wordsCell.textContent = data.vocabulary && words.length === data.vocabulary.length
			? "every permission (" + words.length + ")"
			: words.join(", ");

		editButton.textContent = "Edit";
		editButton.className = "button-small";
		gateAction(editButton, "putHostauthGroupPermissions");
		editButton.addEventListener("click", () => openPermissionsForm(group));
		actionCell.appendChild(editButton);

		/* Destructive, so last, danger-styled and confirmed (web-ux-guidelines). */
		rmButton.textContent = "Remove";
		rmButton.className = "button-small button-danger";
		gateAction(rmButton, "deleteHostauthGroupPermissions");
		rmButton.addEventListener("click", async () => {
			if (!confirm("Remove every permission group \"" + group + "\" grants? Its members lose them on their next request."))
				return;
			try {
				await apiRequest("DELETE", CIX_API.deleteHostauthGroupPermissions(group));
				showStatus("Removed the permissions of " + group);
				await refreshHostauthPermissions();
				await refreshSessionPermissions();
			} catch (e) {
				showStatus("Failed to remove the permissions of " + group + ": " + e.message, true);
			}
		});
		actionCell.appendChild(rmButton);

		row.appendChild(nameCell);
		row.appendChild(wordsCell);
		row.appendChild(actionCell);
		body.appendChild(row);
	}
}

/*
 * The one form for granting: a new group from the Services menu, or an
 * existing one from its row's Edit. The checkboxes are the vocabulary
 * the daemon returned -- the contract's list, never one kept here.
 */
async function openPermissionsForm(group) {
	try {
		if (cache.hostauthPermissions === null || cache.hostauthPermissions === undefined)
			cache.hostauthPermissions = await apiRequest("GET", CIX_API.getHostauthPermissions());
	} catch (e) {
		showStatus(refusalText(e, "the permission vocabulary"), true);
		return;
	}
	/* Generated (#592). An existing group is the path argument, so it
	 * is not asked for; a new one is, under the contract's own name
	 * rule. The choices are the vocabulary the daemon returned. */
	openForm("putHostauthGroupPermissions", {
		title: group === null ? "Grant permissions" : "Permissions of " + group,
		submit: group === null ? "Grant" : "Save",
		args: group === null ? [] : [group],
		values: { permissions: group !== null ? cache.hostauthPermissions.groups[group] || [] : [] },
		choices: { permissions: (cache.hostauthPermissions.vocabulary || []).map((w) => ({ value: w })) },
		/* A 409 is the daemon refusing to leave nobody holding
		 * identity:write; its message says so, shown as it is. */
		failed: "Failed to save the permissions",
		after: async (body) => {
			showStatus((body.permissions || []).length === 0 ? "Removed the group's permissions"
			                                                 : "Saved the group's permissions");
			await refreshHostauthPermissions();
			await refreshSessionPermissions();
		},
	});
}

document.getElementById("menu-grant-permissions").addEventListener("click", () => openPermissionsForm(null));

/* ---------- ADR-0317 (#544): app passwords ---------- */

/*
 * One modal for both scopes: a user's (from the LDAP users table,
 * identity:read/write) and your own (from the Cix menu, a login
 * session). The scope decides only which operations are called, and
 * sessionMay() is asked about exactly those.
 */
let appPasswordScope = null;

function appPasswordOps(scope) {
	if (scope.own)
		return {
			list: () => CIX_API.listOwnAppPasswords(),
			create: () => CIX_API.createOwnAppPassword(),
			remove: (app) => CIX_API.deleteOwnAppPassword(app),
			listOp: "listOwnAppPasswords",
			createOp: "createOwnAppPassword",
			removeOp: "deleteOwnAppPassword",
		};
	return {
		list: () => CIX_API.listLdapUserAppPasswords(scope.user),
		create: () => CIX_API.createLdapUserAppPassword(scope.user),
		remove: (app) => CIX_API.deleteLdapUserAppPassword(scope.user, app),
		listOp: "listLdapUserAppPasswords",
		createOp: "createLdapUserAppPassword",
		removeOp: "deleteLdapUserAppPassword",
	};
}

function openAppPasswords(scope) {
	openModal("app-passwords-form", scope.own ? "My app passwords" : "App passwords of " + scope.user);
	appPasswordScope = scope;
	document.getElementById("apf-scope").textContent = scope.own
		? "Machine credentials for your own account. Each carries your permissions, and managing them needs a login session."
		: "Machine credentials for " + scope.user + ". Each carries that user's permissions.";
	document.getElementById("apf-secret").hidden = true;
	document.getElementById("apf-secret-value").value = "";

	/* Generated (#592), bound inside this modal rather than the
	 * generated-form one, because the modal is a list AND a create: it
	 * stays open after a create, to show the one secret. The operation
	 * is the scope's -- your own account's, or a named user's. */
	bindForm(document.getElementById("app-passwords-form"), appPasswordOps(scope).createOp, {
		args: scope.own ? [] : [scope.user], submit: "Create", clear: true,
		saved: "Created -- copy it now, it is not shown again",
		failed: "Failed to create the app password",
		after: async (body, created) => {
			/* The secret goes into the modal's own field, where it can
			 * be copied, and nowhere else -- not a status message. */
			document.getElementById("apf-secret-value").value = created.password;
			document.getElementById("apf-secret").hidden = false;
			await refreshAppPasswords();
		},
	});
	refreshAppPasswords();
}

async function refreshAppPasswords() {
	const scope = appPasswordScope;
	const ops = appPasswordOps(scope);
	let list = [];

	try {
		const data = await apiRequest("GET", ops.list());

		list = data.app_passwords || [];
	} catch (e) {
		/* listState() has the failure, keyed on the op below. */
	}
	if (appPasswordScope !== scope)
		return; /* the modal moved on while this was in flight */
	renderAppPasswords(scope, list);
}

/*
 * Generated (#592), from `AppPassword`. Two calls rather than one
 * because the list is read by a different operation for your own
 * account and for another user's, and the refused/loading states are
 * keyed on the op -- which test_web_tree requires to be written as a
 * literal, so that a typo in it cannot pass.
 */
function renderAppPasswords(scope, list) {
	const ops = appPasswordOps(scope);
	const table = document.getElementById("apf-table");
	const columns = [
		"name",
		{ field: "created", label: "Created" },
		{ label: "", actions: (ap) => {
			const revokeButton = document.createElement("button");

			revokeButton.type = "button";
			revokeButton.textContent = "Revoke";
			revokeButton.className = "button-small button-danger";
			gateAction(revokeButton, ops.removeOp);
			revokeButton.addEventListener("click", async () => {
				if (!confirm("Revoke app password \"" + ap.name + "\"? Anything using it is refused from its next request."))
					return;
				try {
					await apiRequest("DELETE", ops.remove(ap.name));
					showStatus("Revoked app password " + ap.name);
					await refreshAppPasswords();
				} catch (e) {
					showStatus("Failed to revoke app password " + ap.name + ": " + e.message, true);
				}
			});
			return [revokeButton];
		} },
	];

	if (scope.own)
		renderTable(table, "AppPassword", list, columns, {
			op: "listOwnAppPasswords",
			key: "app-passwords",
			extra: "own",
			what: "these app passwords",
			empty: "No app passwords",
		});
	else
		renderTable(table, "AppPassword", list, columns, {
			op: "listLdapUserAppPasswords",
			key: "app-passwords",
			extra: "user:" + scope.user,
			what: "these app passwords",
			empty: "No app passwords",
		});
}

document.getElementById("menu-own-app-passwords").addEventListener("click", () => openAppPasswords({ own: true }));

async function refreshRollingConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getSystemRollingConfig());

		cache.rollingConfig = config;
		/* Both settings now: the rollback window was in the contract
		 * and the GET all along, and the hand-built form never offered
		 * it (#592). */
		syncForm(document.getElementById("rc-form"), "putSystemRollingConfig", {
			values: config, submit: "Save", saved: "Rolling-restart config saved",
			failed: "Failed to save rolling-restart config", after: refreshRollingConfig,
		});
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

/* ---- ADR-0157 Phase 3: pkg-build concurrency config ---- */

async function refreshPkgBuildConfig() {
	try {
		const config = await apiRequest("GET", CIX_API.getSystemPkgBuildConfig());

		cache.pkgBuildConfig = config;
		syncForm(document.getElementById("pbc-form"), "putSystemPkgBuildConfig", {
			values: config, submit: "Save", saved: "Package-build config saved",
			failed: "Failed to save package-build config", after: refreshPkgBuildConfig,
		});
		/* cix#558: what the build parent has now, which differs from
		 * memory_max only while a build that declared more is running. */
		const effective = Number(config.memory_max_effective) || 0;
		document.getElementById("pbc-memory-effective").textContent =
		    "Build memory budget right now: " + (effective === 0 ? "unlimited" : formatBytes(effective)) +
		    (effective > (Number(config.memory_max) || 0)
		         ? " -- raised for a running build's declared resources { memory }"
		         : "");
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

/* ---------- Package repositories (ADR-0324 step B) ---------- */

/* #129: publishing is outward-facing, so turning it on is confirmed --
 * and only turning it on. `prior` is the repository as it stood, or
 * null for a new one. */
function confirmPkgRepositoryPush(body, prior) {
	const url = body.url || (prior ? prior.url : "");

	return !(body.push && !(prior && prior.push)) ||
		confirm("Publish every package this host builds to " + url + "?");
}

function editPkgRepository(repo) {
	openForm("updatePkgRepository", {
		title: "Edit repository " + repo.name,
		submit: "Save",
		args: [repo.name],
		values: repo,
		secrets: { token: repo.token_set === true },
		before: (body) => confirmPkgRepositoryPush(body, repo),
		after: refreshPkgRepositories,
		failed: "Failed to save the package repository " + repo.name,
	});
}

/* Generated (#592). "Pushes" is a plain boolean for the reason the
 * sources table's "Writes" is -- see renderPkgSourcesTable(). */
function renderPkgRepositoriesTable(repos) {
	renderTable(document.getElementById("pkg-repositories"), "PkgRepository", repos, [
		"name",
		{ field: "url", label: "URL" },
		{ field: "push", label: "Pushes" },
		{ field: "token_set", label: "Token set" },
		{ label: "", actions: (r) => {
			const edit = document.createElement("button");
			const remove = document.createElement("button");

			edit.type = "button";
			edit.className = "button-small";
			edit.textContent = "Edit";
			edit.addEventListener("click", () => editPkgRepository(r));
			gateAction(edit, "updatePkgRepository");

			remove.type = "button";
			remove.className = "button-small button-danger";
			remove.textContent = "Remove";
			remove.addEventListener("click", async () => {
				if (!confirm("Remove package repository " + r.name + "? Artifacts already fetched stay in this host's cache."))
					return;
				try {
					await apiRequest("DELETE", CIX_API.deletePkgRepository(r.name));
					showStatus("Removed package repository " + r.name, false);
					await refreshPkgRepositories();
				} catch (e) {
					showStatus("Failed to remove " + r.name + ": " + e.message, true);
				}
			});
			gateAction(remove, "deletePkgRepository");
			return [edit, remove];
		} },
	], {
		op: "listPkgRepositories",
		what: "package repositories",
		empty: "No package repositories -- every package builds from source",
	});
}

async function refreshPkgRepositories() {
	try {
		const data = await apiRequest("GET", CIX_API.listPkgRepositories());

		renderPkgRepositoriesTable(data.repositories);
	} catch (e) {
		/* listState() already has this failure; see refreshPkgSources(). */
		renderPkgRepositoriesTable([]);
	}
}

/* ---------- New Container: a generated form (#592) ---------- */

/*
 * What a new container may name, read from the box when the form opens:
 * its image, the networks its attachments join, the containers it may
 * start after, the devices and host interfaces it may take and the
 * volumes it may mount. Read fresh, because the header menu opens this
 * from any page, including one that has read none of them.
 *
 * A device is offered as what can be granted: an assignable one, and a
 * GPU as its whole group (`gpu:0`), since one of its nodes alone is not
 * the intended unit (device_find_group()). A network interface is a
 * device on the `net` bus, offered by its bare kernel name, which is
 * what `interfaces` takes. A dependency must be a container with a
 * restart policy, which is what the contract asks of one.
 */
async function containerCreateChoices() {
	const [images, nets, containers, devices, volumes] = await Promise.all([
		apiRequest("GET", CIX_API.listImages()),
		apiRequest("GET", CIX_API.listNetworks()),
		apiRequest("GET", CIX_API.listContainers()),
		apiRequest("GET", CIX_API.listDevices()),
		apiRequest("GET", CIX_API.listVolumes()),
	]);
	const assignable = (devices.devices || []).filter((d) => d.assignable);
	const grants = [];
	const gpuGroups = {};

	for (const d of assignable) {
		if (d.bus === "net")
			continue;
		if (d.bus === "gpu") {
			const group = d.id.split(":").slice(0, 2).join(":");

			if (gpuGroups[group])
				continue;
			gpuGroups[group] = true;
			grants.push({ value: group, label: group + " (whole GPU group)" });
			continue;
		}
		grants.push({ value: d.id, label: d.id + (d.description ? " (" + d.description + ")" : "") });
	}
	return {
		image: (images.images || []).map((i) => ({ value: i.name })),
		"networks.name": (nets.networks || []).map((n) => ({ value: n.name })),
		depends_on: (containers.containers || [])
			.filter((c) => c.restart && c.restart !== "no")
			.map((c) => ({ value: c.name })),
		"devices.id": grants,
		interfaces: assignable
			.filter((d) => d.bus === "net")
			.map((d) => ({
				value: d.id.replace(/^net:/, ""),
				label: d.id.replace(/^net:/, "") + (d.description ? " (" + d.description + ")" : ""),
			})),
		"volumes.name": (volumes.volumes || []).map((v) => ({ value: v.name })),
	};
}

/*
 * The New Container form's layout, which ADR-0338 leaves to a person:
 * forty-seven fields, grouped as the hand-built form grouped them. What
 * belongs to running something comes first; a field no section names
 * still appears, under "Other", so a property added to the contract is
 * offered rather than waiting to be placed here.
 *
 * A function rather than a constant because GENERATED_FORM_HOOKS, near
 * the top of this file, names it while the file is still loading, and
 * a `const` declared down here does not exist yet at that point.
 */
function containerFormSections() {
	return [
		{ title: "Identity", fields: ["name", "image", "services"] },
		{ title: "Resources", fields: ["memory_max", "memory_swap_max", "pids_max", "cpu_max",
		                               "cpuset_cpus", "disk_quota_bytes", "disk", "ksm"] },
		{ title: "Networking", fields: ["networks", "routes", "dns_servers", "dns_register",
		                                "ip_forward", "sysctls", "interfaces"] },
		{ title: "Devices and access", fields: ["devices", "cap_add", "userns", "procfuse",
		                                        "consoles", "capture_output"] },
		{ title: "Files and storage", fields: ["files", "volumes", "env"] },
		{ title: "Lifecycle", fields: ["restart", "restart_delay_seconds", "follow_rolling",
		                               "follow_rolling_jitter_seconds", "depends_on"] },
		{ title: "Service roles", fields: ["dns_server", "ldap_server", "ntp_server", "syslog_target"] },
		{ title: "TLS", fields: ["pki_issue", "pki_cert", "pki_cert_dir", "pki_days"] },
		{ title: "LDAP", fields: ["ldap_client", "ldap_allow_groups", "ldap_provision", "ldap_user",
		                          "ldap_group", "ldap_uid", "ldap_secret_dir"] },
	];
}

async function afterContainerCreate() {
	await refreshContainers();
	renderTree();
}

/* ---------- form submit handlers ---------- */







/*
 * After a certificate is issued (createPkiCert, a generated form): the
 * response is the ONLY place its leaf key is ever returned, so it is
 * shown once, in a modal of its own that stays until the operator
 * closes it. The generated form closes on success; this opens straight
 * after it. Built with textContent, never innerHTML -- cert_pem and
 * key_pem are PEM text, not markup.
 */
async function showIssuedCert(body, issued) {
	const panel = document.getElementById("pki-cert-issued");
	const heading = document.createElement("h3");
	const certLabel = document.createElement("div");
	const certPre = document.createElement("pre");
	const keyLabel = document.createElement("div");
	const keyPre = document.createElement("pre");

	panel.textContent = "";
	heading.textContent = "Copy the private key now -- it is not shown again";
	certLabel.textContent = "Certificate:";
	certPre.textContent = issued.cert_pem;
	keyLabel.textContent = "Private key:";
	keyPre.textContent = issued.key_pem;
	panel.appendChild(heading);
	panel.appendChild(certLabel);
	panel.appendChild(certPre);
	panel.appendChild(keyLabel);
	panel.appendChild(keyPre);
	openModal("pki-cert-issued", "Issued " + issued.name);
	showStatus("Issued certificate " + issued.name);
	await refreshPkiCerts();
}

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
		syncForm(document.getElementById("sys-site-form"), "putSystemSite", {
			values: site, submit: "Save", saved: "Site config saved",
			failed: "Failed to save site config", after: refreshSiteConfig,
		});
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

/*
 * On leaving a bare-label name field (no dot typed -- an FQDN the
 * operator already fully typed is left alone), auto-expand it to this
 * site's suggested FQDN. Still a plain text field afterward -- fully
 * editable, never enforced.
 *
 * form.js calls this for every `format: hostname` field (#596), which
 * is how the convenience reaches a field nobody wired by hand. It used
 * to be attached to two specific element ids, so only those two had
 * it; the site's own suffix is this file's knowledge, which is why the
 * behaviour lives here and the DECISION to apply it lives with the
 * field's own format.
 */
function qualifyHostname(input) {
	input.addEventListener("blur", () => {
		const value = input.value.trim();

		if (value === "" || value.includes("."))
			return;
		const fqdn = suggestedFqdn(value);
		if (fqdn)
			input.value = fqdn;
	});
}




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
/*
 * A kernel route, as columns -- the box's routing table and a
 * network's own routes tab are the same rows filtered differently, so
 * they share one definition rather than one row-builder each table
 * appends through (which is what `appendRouteRow` was, #592).
 *
 * Destination joins two fields (`dest` and `prefix`), and "default"
 * stays itself rather than "default/0", which is the one route an
 * operator looks for by name. Gateway and interface are plain fields:
 * the hand-written cell tested them against null and printed "-", and
 * `fieldText` already says "—" for null.
 */
function routeColumns() {
	return [
		{ label: "Destination", text: (r) => (r.dest === "default"
			? "default" : r.dest + "/" + r.prefix) },
		"gateway",
		"interface",
		{ label: "", actions: (r) => {
			const rm = document.createElement("button");

			rm.textContent = "Remove";
			rm.className = "button-danger";
			gateAction(rm, "deleteSystemRoute");
			rm.addEventListener("click", () => removeRoute(r));
			return [rm];
		} },
	];
}

async function removeRoute(route) {
	const body = route.dest === "default" ? {} : { dest: route.dest, prefix: route.prefix };
	const which = route.dest === "default" ? "the default route" : route.dest + "/" + route.prefix;

	if (!confirm("Remove " + which + " from the host's routing table?"))
		return;

	try {
		await apiRequest("DELETE", CIX_API.deleteSystemRoute(), body);
		clearStatus();
		await refreshRoutes();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to remove route: " + e.message, true);
	}
}

/* Generated (#592). This table is its own read, and was the one that
 * reproduced the Primitives board's lie -- "No routes." in a browser
 * with no backend, where nothing had been asked -- so its four states
 * are the renderer's now rather than a guard and an empty row of its
 * own. */
function renderRoutesList() {
	renderTable(document.getElementById("routes"), "KernelRoute", cache.routes,
	    routeColumns(), { op: "getSystemRoutes", what: "routes", empty: "No routes." });
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
	if (!confirm("Stop reapplying " + key + " at boot? Its live value is left as it is."))
		return;
	try {
		await apiRequest("DELETE", CIX_API.deleteSystemSysctl(key));
		clearStatus();
		await refreshSysctl();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to remove sysctl: " + e.message, true);
	}
}

/* Generated (#592). The key is monospace because it is an identifier
 * (net.ipv4.ip_forward), and the value goes through
 * formatSysctlValue() because a multi-token value is an array in the
 * response and a space-joined string to a reader. */
function renderSysctlList() {
	renderTable(document.getElementById("sysctl-table"), "SysctlEntry", cache.sysctls, [
		{ field: "key", cls: "processes-cmdline" },
		{ label: "Value", text: (s) => formatSysctlValue(s.value) },
		{ label: "", actions: (s) => {
			const rm = document.createElement("button");

			rm.textContent = "Remove";
			rm.className = "button-danger button-small";
			gateAction(rm, "deleteSystemSysctl");
			rm.addEventListener("click", () => removeSysctl(s.key));
			return [rm];
		} },
	], {
		op: "listSystemSysctl",
		what: "persisted sysctls",
		empty: "No sysctls persisted.",
	});
}

/*
 * Generated (#592). The action list is a closed registry (ADR-0257) --
 * populated from the daemon, never typed, because a free-text command
 * field on a shell-less host is a shell-exec endpoint -- so it is the
 * action field's choices. "When" is the contract's ScheduleSpec: three
 * groups, of which the operator fills one; the daemon refuses two rather
 * than resolving by precedence, and says so.
 */
document.getElementById("schedule-add").addEventListener("click", async () => {
	await refreshScheduleActions();
	openForm("setSchedule", {
		title: "Add schedule", submit: "Create",
		values: { enabled: true },
		choices: { action: scheduleActions.map((a) => {
			const name = typeof a === "string" ? a : a.name;
			const summary = typeof a === "string" ? "" : a.summary;

			return { value: name, label: summary ? name + " — " + summary : name };
		}) },
		failed: "Failed to create schedule", after: refreshSchedules,
	});
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
	if (!confirm("Unload kernel module \"" + name + "\" now? Modules only it was using are "
	             + "unloaded too."))
		return;
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
	if (!confirm("Clear the saved options and autoload for \"" + name + "\"? Whether it is "
	             + "loaded now does not change."))
		return;
	try {
		await apiRequest("DELETE", CIX_API.deleteKmodConfig(name));
		clearStatus();
		await refreshKmodConfig();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to remove kmod config: " + e.message, true);
	}
}

/*
 * Generated (#592), both tables. The loaded list's "Used by" is the
 * count with the users beside it -- two fields, so derived -- and the
 * config's options are an object of `name=value` pairs, which is also
 * derived; an empty one says "—" where it said "(none)".
 */
function renderKmodList() {
	renderTable(document.getElementById("kmod-table"), "KmodEntry", cache.kmodModules, [
		"name",
		"size",
		{ label: "Used by", text: (m) => m.used_by_count +
			(m.used_by && m.used_by.length > 0 ? " [" + m.used_by.join(",") + "]" : "") },
		"state",
		{ label: "", actions: (m) => {
			const unload = document.createElement("button");

			unload.textContent = "Unload";
			unload.className = "button-danger";
			gateAction(unload, "deleteKmod");
			unload.addEventListener("click", () => unloadKmod(m.name));
			return [unload];
		} },
	], { op: "listKmod", what: "loaded modules", empty: "No modules loaded." });

	renderTable(document.getElementById("kmodconfig-table"), "KmodConfigEntry",
	    cache.kmodConfig, [
		"name",
		{ label: "Default options", text: (c) => (c.default_options &&
			Object.keys(c.default_options).length > 0
			? Object.entries(c.default_options).map(([k, v]) => k + "=" + v).join(" ")
			: FIELD_ABSENT) },
		"autoload",
		{ label: "", actions: (c) => {
			const rm = document.createElement("button");

			rm.textContent = "Remove";
			rm.className = "button-danger";
			gateAction(rm, "deleteKmodConfig");
			rm.addEventListener("click", () => removeKmodConfig(c.name));
			return [rm];
		} },
	], {
		op: "listKmodConfig",
		what: "persisted module options",
		empty: "No kmod config persisted.",
	});
}

/* The log browsing table/filter form moved to the always-visible
 * bottom log panel (ADR-0129) -- this page now only shows/edits the
 * server-side store's own size cap. */
async function refreshLogsConfig() {
	try {
		const cfg = await apiRequest("GET", CIX_API.getSystemLogsConfig());

		/* Guarded now (#592): this ran on every render of the logs page
		 * and overwrote a size being typed. The form also offers the
		 * minimum level, which the contract has always had and the
		 * hand-built form never showed. */
		syncForm(document.getElementById("logs-config-form"), "putSystemLogsConfig", {
			values: cfg, submit: "Save", saved: "Log settings saved",
			failed: "Failed to save the log settings", after: refreshLogsConfig,
		});
	} catch (e) {
		/* Best-effort -- the field just stays at whatever was last shown. */
	}
}

/*
 * Issue #77 surface parity: GET /v1/system/kmsg (the kernel ring buffer)
 * had no web surface at all. Rendered as plain text rather than a table --
 * kmsg is a linear stream of kernel messages, and an operator reading it is
 * scanning for a failure in sequence, exactly as with dmesg.
 */
const KMSG_PRIORITY_NAMES = ["emerg", "alert", "crit", "err", "warn", "notice", "info", "debug"];

/* The kernel log read the tail field asks for. */
function kmsgPath() {
	const tail = parseInt(document.getElementById("kmsg-tail").value, 10);

	return Number.isFinite(tail) && tail > 0 ? CIX_API.getSystemKmsg() + "?tail=" + tail
	                                         : CIX_API.getSystemKmsg();
}

/* One sample of the kernel log, pushed or polled (startLiveRead()). */
function renderKmsg(data) {
	const out = document.getElementById("kmsg-output");
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
}

/* Live while its page is open (ADR-0339 phase 2): the kernel log is a
 * stream, never announced, so arriving at it is not enough. */
function startKmsgLive() {
	if (!liveReads.kmsg)
		document.getElementById("kmsg-output").textContent = "(loading)";
	startLiveRead("kmsg", kmsgPath(), renderKmsg);
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
/*
 * Generated (#592), from `SoftwareItem` -- promoted out of
 * GET /software's inline items, which also put its required keys in
 * front of the contract-vs-daemon gate (#605).
 *
 * The Recipe column flags the one state that is a problem rather than
 * information, and only that one: no recipe means it cannot be rebuilt
 * from source control, an error badge with that sentence as its hover;
 * a recipe is the ordinary "yes", so its badge kind is null.
 */
function renderSoftwareReconcile(list) {
	renderTable(document.getElementById("sw-table"), "SoftwareItem", list, [
		"name",
		"kind",
		{ label: "Recipe", text: (i) => (i.declared ? "yes" : "none"),
		  badge: (t, i) => (i.declared ? null : "error"),
		  title: (i) => (i.declared ? "" : "Cannot be rebuilt from source control") },
		"installed",
	], { op: "listSoftware", what: "installed software", empty: "Nothing to reconcile" });
}

async function refreshSoftwareReconcile() {
	const summary = document.getElementById("sw-summary");
	let list;

	try {
		const data = await apiRequest("GET", CIX_API.listSoftware());

		list = data.software || [];
	} catch (e) {
		summary.textContent = "Could not read: " + e.message;
		/* And the table says why too, through listState(), where it used
		 * to keep whatever it last painted. */
		renderSoftwareReconcile([]);
		return;
	}
	renderSoftwareReconcile(list);

	const drift = list.filter((item) => !item.declared).length;

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
/*
 * The page forms that act rather than configure (#592): nothing on the
 * box to prefill them from, so they are bound once, at load, and a
 * successful submit clears them by rebuilding. Each one's authored
 * words, question and consequence are here.
 */
function bindStaticPageForms() {
	bindForm(document.getElementById("factory-reset-form"), "factoryReset", {
		submit: "Factory reset", saved: "Factory reset armed \u2014 the box is rebooting and " +
		                                 "will come back empty.",
		failed: "Factory reset refused", clear: true,
		/* The authored question replaces the contract's generic one:
		 * it says what is about to be destroyed. */
		before: (body) => confirm("Factory reset " + body.confirm + "?\n\nThis destroys every " +
		                          "container, image, network and VOLUME (and all data in them), " +
		                          "then reboots. It cannot be undone."),
	});
	bindForm(document.getElementById("sys-update-form"), "updateSystem", {
		submit: "Stage update", failed: "Failed to stage update", clear: true,
		after: (body, result) =>
			showStatus("Staged to slot " + result.slot + " (" + result.updated.join(", ") + ").",
			           false),
	});
	bindForm(document.getElementById("iso-form"), "postSystemIso", {
		submit: "Build ISO", saved: "Building installer media\u2026",
		failed: "Could not start the ISO build", after: refreshIso,
	});
	/* `config_symbols`, which the hand-built form sent as `symbols` --
	 * a key the daemon has never read, so every symbol typed there was
	 * dropped and the kernel built without it (#592). */
	bindForm(document.getElementById("kmod-build-form"), "postKmodBuild", {
		submit: "Build kernel", failed: "Failed to start kernel build",
		saved: "Build started -- see the log panel below for progress.", clear: true,
	});
	bindForm(document.getElementById("pki-ca-form"), "createPkiCa", {
		submit: "Bootstrap CA", saved: "CA created", failed: "Failed to bootstrap CA",
		clear: true, after: refreshPkiCa,
	});
	bindForm(document.getElementById("pki-intermediate-form"), "createPkiIntermediate", {
		submit: "Create intermediate", saved: "Intermediate CA created",
		failed: "Failed to bootstrap intermediate", clear: true, after: refreshPkiIntermediate,
	});
	bindForm(document.getElementById("pki-reset-form"), "resetPki", {
		submit: "Reset CA chain", failed: "Failed to reset CA chain", clear: true,
		before: () => confirm("Wipe and regenerate the entire CA chain? Every " +
		                      "previously-issued certificate stops verifying against the new " +
		                      "root the moment this completes."),
		after: async (body, result) => {
			const resultBox = document.getElementById("pki-reset-result");
			const reissued = Array.isArray(result.reissued) ? result.reissued : [];

			resultBox.hidden = false;
			resultBox.textContent = "Reset complete. Root: " +
				(result.root ? result.root.subject : "?") +
				(result.intermediate ? " / Intermediate: " + result.intermediate.subject : "") +
				(reissued.length > 0 ? " / Reissued: " + reissued.map((c) => c.name).join(", ")
				                     : " / No leaves to reissue");
			await refreshPkiCa();
			await refreshPkiIntermediate();
			await refreshPkiCerts();
		},
	});
	/* Cleared on success only: a rejected paste stays in the boxes, so a
	 * correctable mistake -- the wrong half of a pair, most likely -- is
	 * not pasted again from scratch. */
	bindForm(document.getElementById("signing-keys-form"), "putSystemSigningKeys", {
		submit: "Install", saved: "Signing key pair installed.", clear: true,
		failed: "Could not install the signing key pair", after: refreshSigningKeys,
	});
	bindForm(document.getElementById("release-key-form"), "putSystemReleaseKey", {
		submit: "Install", saved: "Release key installed.", clear: true,
		failed: "Could not install the release key", after: refreshReleaseKey,
	});
	bindForm(document.getElementById("boot-manager-form"), "setBootManager", {
		submit: "Install", failed: "Failed to install boot manager", clear: true,
		before: (body) => confirm("Install " + (body.path || body.url) + " as this host's boot " +
		                          "manager?\n\nThis is the one file both A/B slots share -- there " +
		                          "is no per-slot fallback if it is wrong. It does not reboot; you " +
		                          "choose when to."),
		after: async (body, r) => {
			showStatus("Boot manager installed (" + r.size + " bytes, sha256 " +
			           r.sha256.slice(0, 16) + "\u2026) -- reboot separately to exercise it", false);
			await refreshBootManager();
		},
	});
}

/* ---- Issue #96: volume content snapshots ---- */

async function refreshVolumeBackupConfig() {
	try {
		const cfg = await apiRequest("GET", CIX_API.getVolumeBackupConfig());

		/* Only disks carrying the backup role: offering any other would
		 * be offering a choice the daemon is going to refuse. Not
		 * rebuilt while the operator is choosing (syncForm()). This PUT
		 * replaces the whole config, and the form sends both fields. */
		syncForm(document.getElementById("vbc-form"), "setVolumeBackupConfig", {
			values: cfg,
			choices: { disk: (cache.storage || [])
				.filter((d) => { const r = storageRoleFor(d.name); return r !== null && r.role === "backup"; })
				.map((d) => ({ value: d.name,
				               label: d.name + (d.mounted ? " (" + d.mount_path + ")" : " — not mounted") })) },
			submit: "Save", saved: "Volume backup config saved", failed: "Failed to save",
			after: refreshVolumeBackupConfig,
		});
	} catch (e) {
		showStatus("Failed to read the volume backup config: " + e.message, true);
	}
}

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
	/* The policy, bound to this volume; not rebuilt while the operator
	 * is editing it (syncForm()). */
	syncForm(document.getElementById("vd-backup-form"), "setVolumeBackupPolicy", {
		args: [volumeName],
		values: { enabled: !!data.enabled, retain: data.retain,
		          while_running: data.while_running || "refuse" },
		submit: "Save policy", saved: "Backup policy saved",
		failed: "Failed to save the backup policy",
		after: () => renderVolumeBackups(volumeName, 1),
	});

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

	/*
	 * Generated (#592), from `VolumeSnapshot`, promoted out of
	 * VolumeBackups.snapshots' inline items for this table.
	 *
	 * Delete is red now. Restore was, and Delete was a plain button
	 * beside it, while both are irreversible -- the Primitives board's
	 * rule is that destructive actions are red, go last and confirm, and
	 * this was the one that had the confirm without the colour.
	 */
	renderTable(document.getElementById("vd-snapshots"), "VolumeSnapshot",
	    data.snapshots || [], [
		{ field: "stamp", label: "Snapshot" },
		{ label: "", actions: (snap) => {
			const restore = document.createElement("button");
			const del = document.createElement("button");

			restore.type = "button";
			restore.className = "button-danger button-small";
			restore.textContent = "Restore";
			gateAction(restore, "restoreVolume");
			restore.addEventListener("click", () => restoreVolumeSnapshot(volumeName, snap.stamp));

			del.type = "button";
			del.className = "button-danger button-small";
			del.textContent = "Delete";
			gateAction(del, "deleteVolumeSnapshot");
			del.addEventListener("click", async () => {
				if (!confirm("Delete snapshot " + snap.stamp + "?"))
					return;
				try {
					await apiRequest("DELETE",
					    CIX_API.deleteVolumeSnapshot(volumeName, snap.stamp));
					clearStatus();
					await renderVolumeBackups(volumeName, 1);
				} catch (e) {
					showStatus("Failed to delete snapshot: " + e.message, true);
				}
			});
			return [restore, del];
		} },
	], {
		op: "getVolumeBackups",
		key: "volume-detail-snapshots",
		extra: volumeName,
		what: "snapshots",
		empty: "No snapshots yet",
	});
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

	/*
	 * #432: keyed on the NAME as well as the payload, or navigating
	 * between two volumes with identical records would not redraw.
	 * `holder` and `users` are derived from other caches, so they are
	 * in the signature too rather than assumed to follow the volume.
	 * volumeDetailFresh below is untouched: it is about arriving at
	 * the page, which a skipped re-render does not change.
	 */
	if (unchangedAndRendered(fields, "volume-detail",
	                         { name: name, volume: v, holder: holder, users: users }))
		return;
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
		fieldBlock("Created", formatInstant(v.created_at))
	);
	fields.appendChild(
		fieldBlock("Owner",
		           v.owner_uid !== null && v.owner_uid !== undefined
		               ? v.owner_uid + ":" + v.owner_gid
		               : "root (0:0)")
	);

	/* Issue #102's rule -- typing here is never overwritten by the poll
	 * -- is syncForm()'s now (#592). Both ids empty send null, which is
	 * root: the "Hand back to root" button was a second way to send the
	 * same request. */
	syncForm(document.getElementById("vd-owner-form"), "setVolumeOwner", {
		args: [name], values: { uid: v.owner_uid, gid: v.owner_gid },
		submit: "Set owner", saved: "Owner changed", failed: "Could not change the owner",
		after: () => renderVolumeDetail(name),
	});
	document.getElementById("vd-owner-note").textContent =
		v.owner_uid !== null && v.owner_uid !== undefined
			? "Owned by " + v.owner_uid + ":" + v.owner_gid + "."
			: "Owned by root — a workload running as anyone else cannot write to it.";

	/* Only devices that could actually hold it: mounted, and not
	 * protected. Offering anything else would be offering a choice the
	 * daemon is going to refuse; the blank is the default OS-disk
	 * placement. */
	const targets = (cache.storage || [])
		.filter((d) => d.mounted && !d.protected)
		.map((d) => ({ value: d.name, label: d.name + " (" + d.mount_path + ")" }));

	syncForm(document.getElementById("vd-migrate-form"), "migrateVolume", {
		args: [name], values: { disk: v.disk || "" }, choices: { disk: targets },
		submit: "Move", saved: "Volume moved", failed: "Failed to migrate volume",
		before: (body) => confirm('Move volume "' + name + '" to ' +
		                          (body.disk || "the default OS-disk placement") + "?"),
		after: async () => {
			await refreshDisks();
			await renderVolumeDetail(name);
			renderTree();
		},
	});

	/* The size limit, typed with its unit (`x-cix-ui: bytes`) where
	 * the hand-built form took whole GiB and rounded -- re-saving a
	 * quota that was not a whole number of GiB changed it (#592). */
	syncForm(document.getElementById("vd-quota-form"), "setVolumeQuota", {
		args: [name], values: { quota_bytes: v.quota_bytes || 0 },
		submit: "Set limit", saved: "Limit saved", failed: "Failed to set the limit",
		after: () => renderVolumeDetail(name),
	});
	document.getElementById("vd-quota-note").textContent =
		v.quota_bytes > 0
			? "Limited to " + formatBytes(v.quota_bytes) + "."
			: "No limit — this volume can grow until its disk is full.";

	renderVolumeBackups(v.name, volumeDetailFresh);

	note.textContent =
		running.length > 0
			? "Cannot move it right now: " + running.map((c) => c.name).join(", ") + " is running and has it mounted. Stop it first."
			: targets.length === 0
			  ? "No other mounted disk is available to move it to. Give a disk or partition a role and format it first."
			  : "";
	const move = document.querySelector("#vd-migrate-form button[type=submit]");

	if (move !== null)
		move.disabled = running.length > 0 || !sessionMay("migrateVolume");
}

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
	let vols = [];
	let containers = [];
	/* Whether the second read answered. Without it, a refused
	 * listContainers after a good listVolumes would mark every volume
	 * "not currently mounted" -- a guess shown as a fact. */
	let containersRead = false;
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
		containersRead = true;
	} catch (e) {
		/* listState() has the failure; see refreshBuildLogs(). It
		 * used to be a toast, repeated on every sweep. */
	}
	const mountedBy = {};
	for (const c of containers) {
		for (const v of c.volumes || []) {
			if (!mountedBy[v.name])
				mountedBy[v.name] = [];
			mountedBy[v.name].push(c.name + " at " + v.path + (v.read_only ? " (ro)" : ""));
		}
	}
	renderVolumesList(vols, containersRead ? mountedBy : null);
}

/*
 * Generated (#592), from `Volume`. Three cells stay authored, each for
 * a stated reason: Size is measured per row after the row exists
 * (`fill`, since measuring is a tree walk the list does not carry),
 * Mounted by is the cross-reference above, and Disk's null is a
 * statement -- the default disk -- rather than an absence, so "—"
 * would be wrong.
 */
function renderVolumesList(vols, mountedBy) {
	renderTable(document.getElementById("volumes"), "Volume", vols, [
		{ field: "name", link: (v) => "#volumes/" + encodeURIComponent(v.name) },
		{ label: "Size", fill: (v, cell) => fillVolumeUsageCell(v.name, cell) },
		/* "unused" is deliberately not an error state -- a volume
		 * outliving every container that used it is the whole point,
		 * and is exactly when its data is most at risk of being deleted
		 * by someone who assumes it is dead weight. */
		/* null mountedBy: the containers were not read, so who mounts
		 * a volume is unknown -- "—", never "not currently mounted". */
		{ label: "Mounted by", text: (v) => mountedBy === null ? FIELD_ABSENT
		                                    : (mountedBy[v.name] || []).join(", ")
		                                      || "not currently mounted" },
		{ label: "Disk", text: (v) => v.disk || "(default)" },
		"host_path",
		{ field: "created_at", label: "Created" },
		{ label: "", actions: (v) => {
			const del = document.createElement("button");

			del.type = "button";
			del.className = "button-danger button-small";
			del.textContent = "Delete";
			gateAction(del, "deleteVolume");
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
			return [del];
		} },
	], {
		op: "listVolumes",
		key: "storage-volumes",
		/* Who mounts each volume comes from listContainers, not from
		 * these rows, so it is part of what is compared. */
		extra: JSON.stringify(mountedBy),
		what: "volumes",
		empty: "No volumes",
	});
}



async function refreshServerHealth() {
	let servers = [];
	let warnings = [];

	try {
		const data = await apiRequest("GET", CIX_API.getServerHealth());
		servers = data.servers || [];
		warnings = data.warnings || [];
	} catch (e) {
		/* listState() has the failure; see refreshBuildLogs(). It
		 * used to be a toast, repeated on every two-second sweep. */
	}

	/* Issue #83: a warning here means Cix is managing state with nowhere
	   to deliver it -- a silent, total failure of that subsystem, not a
	   degradation. Shown above the table because it is more urgent than
	   any individual server's state. */
	if (dataChanged("server-health-warnings", warnings)) {
		const warnBox = document.getElementById("server-health-warnings");

		warnBox.textContent = "";
		warnBox.hidden = warnings.length === 0;
		for (const wmsg of warnings) {
			const p = document.createElement("p");
			p.className = "warning";
			p.textContent = wmsg;
			warnBox.appendChild(p);
		}
	}
	renderServerHealth(servers);
}

/*
 * Generated (#592), from `ServerHealth`. Probe and failures keep their
 * own headers because the field names say how they are computed rather
 * than what a reader is looking at.
 */
function renderServerHealth(servers) {
	renderTable(document.getElementById("server-health"), "ServerHealth", servers, [
		"kind",
		"container",
		{ field: "state", badge: (st) => serverHealthStateKind(st) },
		"in_service",
		"probe",
		{ field: "consecutive_failures", label: "Failures" },
		"last_error",
		/* Drain/undrain -- the operator override, distinct from what
		 * the probe says. Labelled by the action it performs, not the
		 * state it is in, so the button never reads as a status. */
		{ label: "", actions: (s) => {
			const btn = document.createElement("button");

			btn.type = "button";
			btn.textContent = s.drained ? "Undrain" : "Drain";
			gateAction(btn, "setServerHealthDrain");
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
			return [btn];
		} },
	], {
		op: "getServerHealth",
		what: "server health",
		empty: "No registered servers are being health-tracked yet",
	});
}

document.getElementById("kmsg-form").addEventListener("submit", (event) => {
	event.preventDefault();
	/* A new tail is a new read: the old subscription ends and this one
	 * starts with a sample at once. */
	stopLiveRead("kmsg");
	startKmsgLive();
});

async function refreshDaemonConfig() {
	try {
		const dc = await apiRequest("GET", CIX_API.getDaemonConfig());

		cache.daemonConfig = dc;
		syncForm(document.getElementById("sys-daemon-config-form"), "putDaemonConfig", {
			values: dc, submit: "Save", saved: "Daemon config saved",
			failed: "Failed to save daemon config", after: refreshDaemonConfig,
			/* This dashboard's fetch() calls are relative to the page's
			 * own origin, so changing the plain-HTTP port disconnects
			 * this exact page the moment it takes effect -- confirmed,
			 * as reboot and shutdown are. Asked only when the port is
			 * really changing. */
			before: (body) => body.port === dc.port ||
				confirm("This changes the port this dashboard is served on -- this page " +
				        "will lose its connection once it takes effect. You'll need to " +
				        "reload at the new port. Continue?"),
		});
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
		} else if (ma.bind_unavailable) {
			status.textContent = "Configured " + ma.address + " (network " + ma.network +
				") but NOT BOUND -- " + ma.bind_unavailable + " is not on this host; 127.0.0.1 only.";
		} else {
			status.textContent = "Bound on " + ma.bound + " (network " + ma.network +
				"). 127.0.0.1 is always bound too.";
		}
		/* Guarded now, which it was not: the 30-second sweep used to
		 * overwrite an address being typed (#592). */
		syncForm(document.getElementById("sys-management-address-form"), "putManagementAddress", {
			values: ma.configured ? { address: ma.address } : {},
			submit: "Set", saved: "Management address set",
			failed: "Failed to set management address", after: refreshManagementAddress,
			/* Moving the off-box address disconnects this page if it is
			 * being viewed over that address -- guarded like reboot. */
			before: () => confirm("This sets the off-box address cixd answers on. If you " +
			                      "are viewing this dashboard over that address, this page " +
			                      "will lose its connection and you'll need to reload at the " +
			                      "new address (127.0.0.1 always works on the box). Continue?"),
		});
	} catch (e) {
		status.textContent = "Could not read the management address.";
	}
}

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

function renderZswap(z) {
	const kernelGrid = document.getElementById("zswap-kernel");
	const divergence = document.getElementById("zswap-divergence");

	syncForm(document.getElementById("zswap-form"), "setZswap", {
		values: z, submit: "Save", saved: "zswap saved.",
		failed: "Failed to save zswap", after: refreshZswap,
		/* Only what this kernel actually has. A compressor it was not
		 * built with is one it will refuse, and offering it would be
		 * offering a setting that cannot take. */
		choices: { compressor: (z.available_compressors || []).map((n) => ({ value: n })) },
	});

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


async function refreshSwap() {
	try {
		const s = await apiRequest("GET", CIX_API.getSystemSwap());

		cache.swap = s;
		document.getElementById("swap-status").textContent = s.enabled
			? "Swap: enabled (" + s.size_mb + " MB at " + s.path + ")"
			: "Swap: disabled";
		/*
		 * The disk, which the hand-built form never sent (#592) -- so
		 * every Enable moved swap back to its default path and dropped
		 * the placement the page said it kept. The form opens on the
		 * current disk and size; the choices are the disks given the
		 * swap role.
		 */
		syncForm(document.getElementById("swap-enable-form"), "postSystemSwap", {
			values: s.enabled ? { size_mb: s.size_mb, disk: s.disk } : { disk: s.disk },
			choices: { disk: (cache.storageRoles || []).filter((r) => r.role === "swap")
				.map((r) => ({ value: r.disk_name })) },
			submit: "Enable", saved: "Swap enabled", failed: "Failed to enable swap",
			after: refreshSwap,
		});
	} catch (e) {
		/* Best-effort -- the status line just stays at whatever was last shown. */
	}
}

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

/* The disks given the backup role, as choices (#592) -- used by both
 * backup forms, the host's and the volumes'. */
function backupDiskChoices() {
	return (cache.storage || [])
		.filter((d) => {
			const role = storageRoleFor(d.name);

			return !d.protected && role !== null && role.role === "backup";
		})
		.map((d) => ({ value: d.name, label: d.name + (d.model ? " (" + d.model + ")" : "") }));
}

function renderBackupConfig() {
	/*
	 * #432 was this form rebuilding its <select> on every tick and
	 * reverting a toggle made between ticks. syncForm() keeps both
	 * properties that fix kept: nothing is rebuilt while the operator
	 * is editing, and nothing at all when the inputs have not changed.
	 */
	syncForm(document.getElementById("bc-form"), "putBackupConfig", {
		values: cache.backupConfig || {}, choices: { disk: backupDiskChoices() },
		submit: "Save", saved: "Backup config saved",
		failed: "Failed to save backup config",
		after: async (body, result) => {
			cache.backupConfig = result;
			renderBackupConfig();
		},
	});

	const s = cache.backupStatus;
	const statusP = document.getElementById("bc-status");

	if (!s || s.state === "never") {
		statusP.textContent = "Snapshots: no attempt has run yet";
	} else {
		statusP.textContent =
			"Snapshots: " +
			s.state +
			(s.last_attempt_unixtime
				? " (last attempt " + formatInstant(s.last_attempt_unixtime) + ")"
				: "") +
			(s.state === "failed" && s.error ? " -- " + s.error : "");
	}
}


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

	wrap.className = "stage-track";
	stages.forEach((st, i) => {
		const c = document.createElement("div");

		c.className = "stage";
		if (!stageApplies(kind, st, row))
			c.classList.add("stage-na");
		else if (i < at)
			c.classList.add("stage-done");
		else if (i === at)
			c.classList.add("stage-here", "stage-" + (row.status || "ok"));
		else
			c.classList.add("stage-todo");
		c.title = st + (i === at ? " ← " + (row.status || "ok") : "");
		wrap.appendChild(c);
	});
	return wrap;
}

function dgRow(kind, stages, row, opts) {
	const el = document.createElement("tr");
	const sub = !!(opts && opts.sub);

	el.className = sub ? "pipeline-sub" : "pipeline-row";
	if (!sub)
		el.tabIndex = 0;

	const name = document.createElement("td");
	name.className = "pipeline-name";
	name.textContent = (opts && opts.label) || row.name;
	el.appendChild(name);

	const ver = document.createElement("td");
	ver.className = "pipeline-ver";
	ver.textContent = row.version || row.resolved_version || "";
	ver.title = ver.textContent;
	el.appendChild(ver);

	el.appendChild(document.createElement("td")).appendChild(dgCells(kind, stages, row));

	const why = document.createElement("td");
	why.className = "pipeline-why";
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

/*
 * A REAL TABLE (#598).
 *
 * Each section was a stack of `<div class="dg-row">`, every one its own
 * four-column CSS grid repeating the same template -- which is a table
 * emulated, and it lined up only because every row re-declared the
 * identical `grid-template-columns`. Nothing announced a column to a
 * screen reader, and seven private classes existed to say what `<tr>`,
 * `<td>` and `<th>` already mean.
 *
 * The stage track stays ONE cell rather than becoming one `<td>` per
 * stage: the marks are a composite mark read left to right, and table
 * cell padding between them would turn a progress bar into eight
 * boxes.
 *
 * HAND-WRITTEN ON PURPOSE (#592), the one table on this page that is.
 * Every other list here renders through renderTable; this one is a
 * visualisation rather than a list -- a stage track drawn across one
 * header cell, a position per stage in each row, and a package's
 * per-image positions folded beneath it as sub-rows, one table built per
 * kind. Expressing that would need renderer forms nothing else uses, and
 * a form with one caller is a private widget wearing the renderer's
 * name. Its rows' FIELDS are the contract's since apigen read `allOf`
 * (PipelinePackage and its siblings were empty before), and test_apishape
 * checks them.
 */
function dgSection(kind, label, stages, rows, frag) {
	const sec = document.createElement("div");
	const table = document.createElement("table");
	const thead = document.createElement("thead");
	const hdr = document.createElement("tr");
	const body = document.createElement("tbody");
	const hc = document.createElement("div");

	sec.className = "pipeline-section";

	const head = document.createElement("p");
	head.className = "section-label";
	head.textContent = label + " (" + rows.length + ")";
	sec.appendChild(head);

	table.className = "pipeline-table";
	hdr.appendChild(document.createElement("th")).textContent = "Name";
	hdr.appendChild(document.createElement("th")).textContent = "Version";
	hc.className = "stage-track";
	for (const st of stages) {
		const c = document.createElement("div");
		c.className = "stage-head";
		c.textContent = st;
		/* Ten stages in one row means the header text is truncated; the
		 * full name has to stay reachable somewhere. */
		c.title = st;
		hc.appendChild(c);
	}
	hdr.appendChild(document.createElement("th")).appendChild(hc);
	hdr.appendChild(document.createElement("th")).textContent = "Why";
	thead.appendChild(hdr);
	table.appendChild(thead);
	table.appendChild(body);
	sec.appendChild(table);

	for (const r of rows) {
		body.appendChild(dgRow(kind, stages, r, null));
		/*
		 * A package is not in one place -- it is installed into N
		 * images, each with its own position, and the row above is a
		 * fold of them. Showing the images it is really in is what
		 * makes the fold honest rather than a summary that quietly
		 * stands in for eleven different answers.
		 */
		if (kind === "package" && Array.isArray(r.images) && r.images.length > 1)
			for (const im of r.images)
				body.appendChild(
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

	const q = (document.getElementById("pipeline-search") || {}).value || "";
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
	const viewAll = !!(document.getElementById("pipeline-view-all") || {}).checked;
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

	const count = document.getElementById("pipeline-count");
	if (count) {
		const probs =
			(data.blocked || 0) + (data.failed || 0) + (data.cancelled || 0);
		count.textContent = probs
			? probs + " need attention"
			: "nothing blocked, failed or cancelled";
		count.className = "count" + (probs ? " count-bad" : "");
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
	const root = document.getElementById("pipeline-drift");

	if (!root)
		return;
	root.textContent = "";
	if (!Array.isArray(drift) || drift.length === 0) {
		root.textContent = "Every package is at one version everywhere.";
		return;
	}
	/*
	 * A TABLE, like the rest of this page (#598).
	 *
	 * This was a responsive grid of little bordered cards, four
	 * private classes deep, saying one thing per card: a package and
	 * the versions of it in use. That is one row per package with a
	 * cell listing where each version is -- which is what every other
	 * list in this dashboard is, and it loses nothing but the cards.
	 */
	const table = document.createElement("table");
	const thead = document.createElement("tr");
	const body = document.createElement("tbody");

	thead.appendChild(document.createElement("th")).textContent = "Package";
	thead.appendChild(document.createElement("th")).textContent = "Versions in use";
	table.appendChild(document.createElement("thead")).appendChild(thead);
	table.appendChild(body);

	for (const d of drift) {
		const tr = document.createElement("tr");
		const name = document.createElement("td");
		const where = document.createElement("td");
		const byVersion = new Map();

		name.textContent = d.name;
		name.title = d.versions + " versions";
		tr.appendChild(name);

		for (const i of d.installs || []) {
			if (!byVersion.has(i.version))
				byVersion.set(i.version, []);
			byVersion.get(i.version).push(i.image);
		}
		for (const [v, images] of byVersion) {
			const line = document.createElement("div");
			const chip = document.createElement("span");

			/* The badge is the widget for "a value the daemon
			 * reported", and a version is one. `dg-chip` was an
			 * outlined pill doing the same job. */
			chip.className = statusBadge("unknown");
			chip.textContent = v;
			line.appendChild(chip);
			line.appendChild(document.createTextNode(" " + images.join(", ")));
			where.appendChild(line);
		}
		tr.appendChild(where);
		body.appendChild(tr);
	}
	root.appendChild(table);
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
	const drawer = document.getElementById("detail-panel");
	const title = document.getElementById("detail-panel-title");
	const body = document.getElementById("detail-panel-body");

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
		h.className = "section-label";
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

		bar.className = "detail-panel-actions";
		cancel.type = "button";
		cancel.className = "button-small";
		cancel.textContent = "Cancel the in-flight build";
		gateAction(cancel, "pkgCancel");
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
	hh.className = "section-label";
	hh.textContent = "History";
	body.appendChild(hh);
	const runsBox = document.createElement("div");
	runsBox.className = "log-output";
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
				const when = formatInstant(run.ended_at);
				const secs = run.duration_seconds || 0;
				/* #424: where the time went, stage by stage. A run from
				 * before stage times were kept has stages: null and shows
				 * the total alone. */
				const split = Array.isArray(run.stages) && run.stages.length > 0
					? " (" + run.stages.map((s) => s.stage + " " + boDuration(s.seconds)).join(", ") + ")"
					: "";

				/*
				 * A LOG ENTRY, because that is what a list of
				 * timestamped outcomes is (#598). `dg-run` was
				 * `.log-entry` and `dg-runs` was `.log-output`; the
				 * one thing that did not transfer was `dg-run-bad`,
				 * which coloured the whole line, where
				 * `.log-entry-error` colours a `.log-entry-status`
				 * child -- so the status becomes its own span, which
				 * is better anyway: the red belongs on the word that
				 * failed, not on the timestamp beside it.
				 */
				const status = document.createElement("span");

				el.className = "log-entry" + (bad ? " log-entry-error" : "");
				status.className = "log-entry-status";
				status.textContent = bad ? run.stage + "/" + run.status : "ok";
				/* The image is named on every line: a run is a
				 * (package, image) pair, and a history that omits the
				 * image reads as one stream of contradictory outcomes. */
				el.appendChild(document.createTextNode(
					when + " · " + (run.image || "-") + " · "));
				el.appendChild(status);
				el.appendChild(document.createTextNode(
					" · " + secs + "s" + split + " · " + (run.trigger || "-") +
					(run.error ? " — " + run.error : "")));
				runsBox.appendChild(el);
			}
		}
	} catch (e) {
		runsBox.textContent = "Could not read the run history: " + (e && e.message ? e.message : e);
	}

	const h = document.createElement("div");
	h.className = "section-label";
	h.textContent = "Build log";
	body.appendChild(h);
	const pre = document.createElement("pre");
	pre.className = "log-output";
	pre.textContent = "loading…";
	body.appendChild(pre);

	try {
		const file = await latestBuildLogFile(row.name);

		if (file === null) {
			pre.textContent = "No build log for this package on this host.";
			return;
		}
		/* apiRequestRaw(), not apiRequest(): a build log is plain text,
		 * and apiRequest() parses every non-204 body as JSON. Its own
		 * fourth argument is a timeout in ms, not options. */
		const text = await apiRequestRaw("GET", CIX_API.getBuildLog(file));
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
	const search = document.getElementById("pipeline-search");
	const only = document.getElementById("pipeline-view-all");
	const close = document.getElementById("detail-panel-close");
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
			document.getElementById("detail-panel").hidden = true;
		});
	}
}

async function refreshPipeline() {
	let data;

	try {
		data = await apiRequest("GET", CIX_API.getPipeline());
	} catch (e) {
		/* The stage board keeps its last picture; the Errors list
		 * says the read failed, since that is its own refused state. */
		renderPipelineErrors([]);
		return;
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

	renderPipelineErrors(data.packages || []);
	document.getElementById("pipeline-summary").textContent =
	    data.total + " package(s): " + data.ok + " ok, " + data.blocked + " blocked, " +
	    data.failed + " failed, " + data.cancelled + " cancelled, " + data.not_implemented +
	    " performed by hand";
}

/*
 * The Errors tab. Generated (#592), from `PipelinePackage`: every
 * package whose pipeline is not clean. `not-implemented` is not a
 * problem -- this platform performs that stage by hand -- so it is
 * left out with `ok`.
 */
function renderPipelineErrors(packages) {
	renderTable(document.getElementById("pipeline-table"), "PipelinePackage",
	    packages.filter((p) => p.status !== "ok" && p.status !== "not-implemented"), [
		{ field: "name", label: "Package" },
		"stage",
		{ field: "status", badge: (st) => pipelineStatusKind(st) },
		{ field: "resolved_version", label: "Resolved" },
		{ field: "newest_recipe_version", label: "Newest recipe" },
		"reason",
	], {
		op: "getPipeline",
		what: "the pipeline",
		empty: "Nothing is blocked or failed.",
	});
}

/* ---------- Software > Build > In Flight (#407) ---------- */

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
 * #433: the build-log window's controls -- close, resume following, and
 * scroll-to-pause. Escape closes it as well as the button,
 * because a panel covering what you were reading should close the way
 * every other dismissable thing on the page does -- and because the
 * bug being fixed is that it could not be closed at all.
 */
document.addEventListener("DOMContentLoaded", () => {
	document.getElementById("build-log-close").addEventListener("click", closeBuildLog);
	document.getElementById("build-log-state").addEventListener("click", resumeBuildLogFollowing);
	document.getElementById("build-log-view").addEventListener("scroll", onBuildLogScroll);
});

document.addEventListener("keydown", (event) => {
	if (event.key === "Escape" && !document.getElementById("build-log-window").hidden)
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

/*
 * #431: a build card carries its build container's CPU and memory as two
 * small charts, so an operator can see what a build is doing to the box
 * while it does it.
 *
 * The samples are the container's stats read, subscribed on the event
 * stream (startLiveRead(), ADR-0339 phase 2) for as long as the card is
 * on screen -- one subscription per build in flight, polled instead
 * while the stream is down or once a stream holds its 8. There is no
 * history anywhere but here, so a chart starts empty and fills as you
 * watch, and the card says so rather than looking broken. It is kept
 * across the card being redrawn, which happens on every announcement
 * the overview reads.
 *
 * The arithmetic is the container Stats tab's: CPU is the difference of
 * the cumulative usage_usec over wall time, 1 core = 100%; memory.current
 * plots directly and memory.peak is a figure, never a series. A build
 * may use more than one core, so the CPU scale grows past 100% rather
 * than clipping at it.
 */
const BO_STATS_HISTORY_MAX = 90; /* three minutes at the 2 s sample period */
const boStatsHistory = new Map(); /* build container -> samples */
const boStatsLive = new Set();    /* build containers whose live read the page holds */

function boStatsId(container) {
	return "bstats-" + container.replace(/[^A-Za-z0-9_-]/g, "-").slice(0, 25);
}

function boIngestStats(container, stats) {
	let h = boStatsHistory.get(container);

	if (!stats || !stats.cpu || !stats.memory)
		return;
	if (h === undefined) {
		h = [];
		boStatsHistory.set(container, h);
	}
	h.push({
		t: Date.now(),
		cpuUsageUsec: stats.cpu.usage_usec,
		memCurrent: stats.memory.current,
		memPeak: stats.memory.peak,
	});
	if (h.length > BO_STATS_HISTORY_MAX)
		h.shift();
	boDrawStats(container);
}

function boDrawStats(container) {
	const fig = document.querySelector('.bo-stats[data-container="' + CSS.escape(container) + '"]');
	const h = boStatsHistory.get(container) || [];

	if (fig === null)
		return;
	const cpuLabel = fig.querySelector(".bo-cpu-label");
	const memLabel = fig.querySelector(".bo-mem-label");

	if (h.length < 2) {
		cpuLabel.textContent = "CPU -- filling in as you watch";
		memLabel.textContent = h.length === 1
			? "Memory " + formatBytes(h[0].memCurrent) + " (peak " + formatBytes(h[0].memPeak) + ")"
			: "Memory -- filling in as you watch";
		return;
	}
	const cpu = [];

	for (let i = 1; i < h.length; i++) {
		const dUsec = h[i].cpuUsageUsec - h[i - 1].cpuUsageUsec;
		const dMs = h[i].t - h[i - 1].t;

		cpu.push(dMs > 0 ? Math.max(0, (dUsec / 1000 / dMs) * 100) : 0);
	}
	drawChart(fig.querySelector(".bo-cpu"), [{ values: cpu, color: seriesColor(1) }], {
		times: h.slice(1).map((s) => s.t),
		maxY: Math.max(100, ...cpu),
		formatY: (v) => v.toFixed(0) + "%",
	});
	drawChart(fig.querySelector(".bo-mem"), [{ values: h.map((s) => s.memCurrent), color: seriesColor(2) }], {
		times: h.map((s) => s.t),
		formatY: formatBytes,
	});
	const last = h[h.length - 1];

	cpuLabel.textContent = "CPU " + cpu[cpu.length - 1].toFixed(0) + "% (1 core = 100%)";
	memLabel.textContent = "Memory " + formatBytes(last.memCurrent) + " (peak " + formatBytes(last.memPeak) + ")";
}

/* The two charts' place on a card; drawn from history at once, so a
 * redraw of the card keeps what it has watched. */
function boStatsFigure(container) {
	const fig = document.createElement("div");
	const add = (canvasClass, labelClass) => {
		const canvas = document.createElement("canvas");
		const label = document.createElement("p");

		canvas.className = canvasClass;
		canvas.width = 420;
		canvas.height = 130;
		label.className = "hint " + labelClass;
		fig.appendChild(canvas);
		fig.appendChild(label);
	};

	fig.className = "bo-stats";
	fig.dataset.container = container;
	add("bo-cpu", "bo-cpu-label");
	add("bo-mem", "bo-mem-label");
	return fig;
}

/* Holds a live read for exactly the build containers in `wanted`, and
 * forgets the history of any that left. */
function boSyncStats(wanted) {
	for (const c of boStatsLive) {
		if (!wanted.has(c)) {
			stopLiveRead(boStatsId(c));
			boStatsLive.delete(c);
			boStatsHistory.delete(c);
		}
	}
	for (const c of wanted) {
		if (!boStatsLive.has(c)) {
			boStatsLive.add(c);
			startLiveRead(boStatsId(c), CIX_API.getContainerStats(c), (s) => boIngestStats(c, s));
		}
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
		boSyncStats(new Set());
		return;
	}
	if (hint)
		hint.hidden = false;

	const statsWanted = new Set(); /* #431: the build containers this page charts */

	for (const key of names) {
		let statsContainer = null;
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
			 *
			 * A CLOCK, not formatInstant(): this panel is a live view
			 * of something running now, beside an elapsed counter, so
			 * "started 17:24:03" reads and "started 11/10/2026,
			 * 17:24:03" does not. A build that began yesterday is
			 * pathological rather than a case to render for.
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
			/* #424: the stage now in flight and how long it has been in
			 * it, ticking from the daemon's own stage_seconds exactly as
			 * "running" ticks from run_seconds. The daemon names the
			 * stage -- current_stage -- so nothing here re-derives it
			 * from timestamps. */
			if (e.current_stage && e.stage_seconds !== null && e.stage_seconds !== undefined) {
				const dd = document.createElement("dd");
				const dt = document.createElement("dt");

				dt.textContent = "in " + e.current_stage;
				dd.className = "bo-elapsed";
				dd.dataset.baseSeconds = String(e.stage_seconds);
				dd.dataset.baseAt = String(Date.now());
				dd.textContent = boDuration(e.stage_seconds);
				dl.appendChild(dt);
				dl.appendChild(dd);
			}
			/* A clock, for the reason "started" above is one. */
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
			/* #431: charted while it builds -- the container exists only
			 * then (build_container is null outside BUILDING). */
			if (e.state === "building" && e.build_container)
				statsContainer = e.build_container;
		} catch (err) {
			add("state", "could not read this job");
			add("why", err && err.message ? err.message : String(err));
		}
		card.appendChild(dl);
		card.addEventListener("click", () => followBuildLog(name, image));
		card.addEventListener("keydown", (ev) => {
			if (ev.key === "Enter" || ev.key === " ") {
				ev.preventDefault();
				followBuildLog(name, image);
			}
		});
		list.appendChild(card);
		if (statsContainer !== null) {
			const note = document.createElement("p");

			note.className = "hint";
			note.textContent = "Sampled every 2 s while this page is open; the history is this " +
			    "page's own and starts when you arrive.";
			card.appendChild(boStatsFigure(statsContainer));
			card.appendChild(note);
			statsWanted.add(statsContainer);
			boDrawStats(statsContainer);
		}
	}
	boSyncStats(statsWanted);
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
	"pkg-repo": [refreshPkgSources, refreshPkgSyncStatus],
	"pkg-cache": [refreshPkgCacheConfig, refreshPkgCacheStatus, refreshPkgRepositories],
	"build-overview": [refreshBuildOverview],
	"pkg-build-config": [refreshPkgBuildConfig, refreshBuildLogs],
	update: [refreshImages, refreshPkgList, refreshSoftwareReconcile],
	devices: [refreshDevices, refreshDeviceMaps, refreshKmod, refreshKmodConfig],
	"dns-records": [refreshDnsRecords, refreshDnsServers, refreshDnsForwarders],
	"dns-servers": [refreshDnsRecords, refreshDnsServers, refreshDnsForwarders],
	"dns-forwarders": [refreshDnsRecords, refreshDnsServers, refreshDnsForwarders],
	"ldap-servers": [refreshLdapServers, refreshLdapConfig],
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
	/* ADR-0339 release B: these were called from renderCurrentView(),
	 * which ran them on every change; as refreshers they run on arrival
	 * and when something they read moves. */
	"control-plane-reservation": [refreshControlPlaneReservation],
	"boot-console": [refreshBootConsole],
	esp: [refreshEsp],
	"signing-keys": [refreshSigningKeys, refreshReleaseKey],
	"kernel-policy": [refreshKernelPolicy],
	logs: [refreshLogsConfig],
	"server-health": [refreshServerHealth],
	"running-config": [refreshRunningConfig],
	"volume-backup-config": [refreshVolumeBackupConfig],
	"hostauth-permissions": [refreshHostauthPermissions],
	"hostauth-sessions": [refreshHostauthConfig, refreshHostauthSessions],
};

const ALL_REFRESHERS = [
	refreshContainers, refreshNetworks, refreshImages, refreshDevices, refreshDeviceMaps,
	refreshDisks, refreshDiskRoles, refreshDiskFormatStatuses, refreshVolumeCache,
	refreshBackupConfig, refreshBackupStatus, refreshDnsRecords, refreshDnsServers,
	refreshLdapServers, refreshLdapGroups, refreshLdapUsers, refreshLdapConfig, refreshNtpConfig,
	refreshNtpServers, refreshSyslogTargets, refreshNtpStatus, refreshNtpTime, refreshPkiCa,
	refreshPkiIntermediate, refreshPkiCerts, refreshPkgRecipes, refreshImageRecipesList,
	refreshContainerRecipesList, refreshPkgList, refreshPkgSources, refreshPkgSyncStatus,
	refreshPkgCacheConfig, refreshPkgCacheStatus, refreshPkgRepositories,
	refreshSiteConfig, refreshDaemonConfig, refreshRollingConfig,
	refreshPkgBuildConfig, refreshRoutes, refreshSysctl, refreshKmod, refreshKmodConfig,
	refreshSwap, refreshTlsThrottleConfig, refreshTlsThrottleStatus,
];

const SWEEP_INTERVAL_MS = 30000;
const VIEW_INTERVAL_MS = 5000;
let lastSweepAt = 0;
let lastViewRefreshAt = 0;

async function runRefreshers(list) {
	const wasSweeping = inBackgroundSweep;

	inBackgroundSweep = true;
	try {
		await runRefreshersInner(list);
	} finally {
		inBackgroundSweep = wasSweeping;
	}
}

/*
 * ADR-0339 phase 2, release B: what each refresher reads, learned from
 * its own requests rather than declared. apiRequest() adds every GET's
 * operation to `recordingReads` while a refresher runs, so the set is
 * the refresher's code itself and cannot drift from it the way a
 * hand-kept list of ~60 would. It only grows: a read made once is
 * remembered, so a change to it always reaches the refresher. A
 * request from elsewhere landing during a refresher's await is recorded
 * too, which costs a spare refresh and never a missed one.
 * (REFRESHER_READS and recordingReads are declared beside
 * opForRequest(), above apiRequest(), which writes them.)
 */
async function runRefreshersInner(list) {
	for (const fn of list) {
		let reads = REFRESHER_READS.get(fn);

		if (reads === undefined) {
			reads = new Set();
			REFRESHER_READS.set(fn, reads);
		}
		recordingReads = reads;
		try {
			await fn();
		} catch (e) {
			/* One endpoint failing must not stop the rest: a single
			 * unhappy subsystem should cost its own panel, not the
			 * whole dashboard's freshness. */
		} finally {
			recordingReads = null;
		}
	}
}

/* The refreshers in `list` that read something in `ops` -- and any whose
 * reads are not known yet, because it has not run. */
function refreshersReading(list, ops) {
	return list.filter((fn) => {
		const reads = REFRESHER_READS.get(fn);

		if (reads === undefined || reads.size === 0)
			return true;
		for (const op of reads)
			if (ops.has(op))
				return true;
		return false;
	});
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
	/*
	 * ADR-0339: while the event stream is live the box says what changed,
	 * so nothing here asks. Its heartbeat is the liveness signal (the LEDs
	 * blink on its frames), a change runs the refreshers that need it
	 * (runEventRefresh()), and navigation reads the page it lands on. The
	 * log panel and the stats pages are live reads with timers of their
	 * own (startLiveRead(), phase 2). What stays here is the tree, drawn
	 * from the cache. NOT renderCurrentView(): several pages' renderers
	 * fetch (the kernel log, the log store's config, build logs, the
	 * ISO), so calling it every tick was polling those pages at 2 s while
	 * the page said it was live -- 60 requests a minute on Health,
	 * measured in Chromium on 2026-10-17. A page re-renders on arrival,
	 * on every announced change and from its live reads. When the stream
	 * is lost or there is no session, this is the polling it always was
	 * -- and the status bar says the page is not live
	 * (renderStreamState()).
	 */
	if (eventStream.isLive()) {
		if (document.hidden)
			return;
		renderTree();
		return;
	}
	await refreshHealth();
	/* The session shell's own two facts come from that health read and
	 * from nothing else, so they refresh here -- a locked dashboard
	 * left open overnight should still say truthfully whether the host
	 * is answering (#597). */
	if (!document.getElementById("lock-shell").hidden)
		refreshLockFacts();
	/* Health only while hidden: the LEDs and the reachability colour
	 * stay honest for anyone who looks back at the tab, and nothing
	 * else is being read by anybody. */
	if (document.hidden)
		return;
	/*
	 * #544: a gated host with no session refuses every read, so the
	 * sweep would be ~30 doomed requests every 30 s -- #490's wall of
	 * refusals, on the host. The login-required banner says why the
	 * panels are still; health above keeps running, so logging in (or
	 * the host being opened up) resumes everything on the next tick.
	 */
	if (hostGated && !sessionAuthenticated)
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
		/* #548: grants change elsewhere -- another admin, another tab --
		 * and apiRequest()'s pre-check must not act on stale ones for
		 * longer than a sweep. One public, unaudited request. */
		await refreshSessionPermissions();
		await runRefreshers(ALL_REFRESHERS);
		/* The current page's own reads too: several are refreshers only
		 * of their page (release B), and the first poll is a sweep. */
		await runRefreshers(refreshersForView(CATEGORY_VIEWS[route.category]));
	} else {
		await runRefreshers(CORE_REFRESHERS);
		await runRefreshers(refreshersForView(CATEGORY_VIEWS[route.category]));
	}
	renderTree();
	renderCurrentView();
}

/* ---------- ADR-0339: the event stream ----------
 *
 * The box names the reads that changed (web/events.js holds the rules).
 * Phase 1 of what a change means here: any change re-runs the core
 * refreshers and the current page's, debounced, so a burst is one
 * render. Phase 2 narrows that to the refreshers that read what moved.
 */
const EVENTS_DEBOUNCE_MS = 150;
let eventStreamToken;              /* the token the open stream was opened with; undefined = none */
let eventsChanged = new Set();     /* operationIds named since the last refresh */
let eventsTimer = null;
let eventsRefreshing = false;
let eventsStaleWhileHidden = false;
const streamStateEl = document.getElementById("status-stream");

const eventStream = createEventStream({
	makeSocket: () => {
		const proto = location.protocol === "https:" ? "wss:" : "ws:";
		/* #541's arrangement, as the console uses it: the session rides
		 * as a subprotocol, since a browser WebSocket sets no headers. */
		const wsProtocols = authToken ? ["cix", "cix.bearer." + authToken] : [];

		return new WebSocket(proto + "//" + location.host + CIX_API.getEvents(), wsProtocols);
	},
	now: () => Date.now(),
	setTimer: (fn, ms) => setTimeout(fn, ms),
	clearTimer: (id) => clearTimeout(id),
	onFrame: () => ledBlink(ledRx),
	onChange: (ops) => {
		for (const op of ops)
			eventsChanged.add(op);
		scheduleEventRefresh();
	},
	onLive: (live) => {
		if (live) {
			statusLeds.className = "status-leds led-state-ok";
			statusLeds.title = "Live — the box tells this page what changed";
		}
		renderStreamState();
	},
	/* Close code 4401: the session ended while the stream was open. The
	 * same consequence as a 401 (sessionLapsed()), and only for this
	 * code -- a daemon restart closes the stream too, and must never
	 * sign anyone out. */
	onSessionEnded: () => {
		eventStreamToken = undefined;
		sessionLapsed(true);
		renderStreamState();
	},
});

/* Whether this page should hold a stream: a session, or a host that
 * needs none. Opened for the token it was opened with, and reopened when
 * that changes, because what a stream may hear is decided per session. */
function syncEventStream() {
	const want = sessionChecked && (sessionAuthenticated || !hostGated);

	if (!want) {
		stopEventStream();
		return;
	}
	if (eventStreamToken !== (authToken || "")) {
		eventStream.stop();
		eventStreamToken = authToken || "";
		resubscribeLiveReads();
		eventStream.start();
	}
	renderStreamState();
}

function stopEventStream() {
	if (eventStreamToken === undefined)
		return;
	eventStream.stop();
	eventStreamToken = undefined;
	renderStreamState();
}

/*
 * The status bar says whether the page is live. Nothing when it is --
 * that is the normal state and the LEDs already show the traffic -- and
 * "Live updates lost — reconnecting" when a stream the page should hold
 * is not up, so a page that has stopped being current never looks
 * current. Signed out, the login banner already says why nothing moves.
 */
function renderStreamState() {
	if (streamStateEl === null)
		return;
	const lost = eventStreamToken !== undefined && !eventStream.isLive();

	streamStateEl.hidden = !lost;
	streamStateEl.textContent = lost ? "Live updates lost — reconnecting" : "";
}

/*
 * ADR-0339 phase 2: a read the page shows live -- the log panel's tail,
 * a stats page. Subscribed on the event stream, so the box sends each
 * sample that differs from the last; while the stream is not live
 * (reconnecting, signed out on an open host, before the first frame) the
 * same read is polled on the old cadence instead. Both paths feed the
 * page's own `onData`, so a pushed sample and a polled one cannot be
 * drawn differently. One id per page slot: starting it again with
 * another path (another container) replaces it.
 */
const liveReads = {};

function subscribeLiveRead(id, entry) {
	eventStream.subscribe(id, entry.path, (body) => {
		if (liveReads[id] === entry)
			entry.onData(body);
	}, () => {
		/* Refused -- not subscribable, or not this session's to read:
		 * the timer below takes over, and meets the same answer a GET
		 * would. */
		entry.refused = true;
	});
}

function startLiveRead(id, path, onData) {
	const prev = liveReads[id];

	if (prev && prev.path === path)
		return;
	stopLiveRead(id);
	const entry = { path: path, onData: onData, timer: null, refused: false };

	liveReads[id] = entry;
	subscribeLiveRead(id, entry);
	const tick = async () => {
		if (eventStream.isLive() && !entry.refused)
			return;
		/* The same two quiet states poll() keeps: a hidden tab reads
		 * nothing but health, and a gated host with no session refuses
		 * every read (#544). */
		if (document.hidden || (hostGated && !sessionAuthenticated))
			return;
		let body;

		try {
			body = await apiRequest("GET", path);
		} catch (e) {
			return; /* best-effort: the next tick, or the stream, resolves it */
		}
		if (liveReads[id] === entry)
			onData(body);
	};
	tick();
	entry.timer = setInterval(tick, POLL_INTERVAL_MS);
}

function stopLiveRead(id) {
	const entry = liveReads[id];

	if (!entry)
		return;
	clearInterval(entry.timer);
	delete liveReads[id];
	eventStream.unsubscribe(id);
}

/* A session that ended (4401) took every subscription with it; a new
 * stream subscribes again to whatever the page still shows. */
function resubscribeLiveReads() {
	for (const id of Object.keys(liveReads)) {
		if (!eventStream.subscribed(id)) {
			liveReads[id].refused = false;
			subscribeLiveRead(id, liveReads[id]);
		}
	}
}

function scheduleEventRefresh() {
	if (document.hidden) {
		eventsStaleWhileHidden = true;
		return;
	}
	if (eventsTimer === null)
		eventsTimer = setTimeout(runEventRefresh, EVENTS_DEBOUNCE_MS);
}

async function runEventRefresh() {
	eventsTimer = null;
	if (eventsRefreshing) {
		/* One at a time: what arrived meanwhile goes in the next. */
		scheduleEventRefresh();
		return;
	}
	const ops = eventsChanged;

	eventsChanged = new Set();
	eventsRefreshing = true;
	try {
		if (ops.has("getWhoami"))
			await refreshSessionPermissions();
		if (ops.has("getHealth"))
			await refreshHealth();
		if (ops.has("getSystemBoot"))
			refreshStatusBar();
		/* Release B: only the refreshers that read what moved. */
		await runRefreshers(refreshersReading(CORE_REFRESHERS, ops));
		await runRefreshers(refreshersReading(refreshersForView(CATEGORY_VIEWS[parseHash().category]),
		                                      ops));
		renderTree();
		renderCurrentView();
	} finally {
		eventsRefreshing = false;
	}
	if (eventsChanged.size > 0)
		scheduleEventRefresh();
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
	/* #548: an item names the operation it performs (`op`), and the
	 * session decides whether it is offered -- disabled with the reason,
	 * never hidden, and never re-enabling an item disabled above. */
	if (item.op)
		gateAction(button, item.op);
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
			{ label: "Open console", danger: false, op: "consoleContainer", action: () => { location.hash = "#containers/" + encodeURIComponent(name); } },
		];

		const e = containerActionEligibility(status);

		if (e.start) items.push({ label: "Start", danger: false, op: "startContainer", action: () => startContainer(name) });
		if (e.pause) items.push({ label: "Pause", danger: false, op: "pauseContainer", action: () => pauseContainer(name) });
		if (e.unpause) items.push({ label: "Unpause", danger: false, op: "unpauseContainer", action: () => unpauseContainer(name) });
		if (e.stop) items.push({ label: "Stop", danger: false, op: "stopContainer", action: () => stopContainer(name) });
		items.push({ label: "Remove", danger: true, op: "deleteContainer", action: () => removeContainer(name) });
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
			items.push({ label: "Add a partition\u2026", danger: false, op: "addStoragePartition",
			             action: () => { location.hash = "storage/" + encodeURIComponent(name); } });
		if (e.assignRole)
			items.push({ label: "Assign a role\u2026", danger: false, op: "createStorageRole", action: () => openAssignRole(name) });
		if (e.removeRole)
			items.push({ label: "Remove role", danger: false, op: "deleteStorageRole", action: () => removeDiskRole(name) });
		if (e.format) {
			items.push({ label: "Format as btrfs", danger: true, op: "formatStorage", action: () => formatDisk(name, "btrfs") });
			items.push({ label: "Format as ext4", danger: true, op: "formatStorage", action: () => formatDisk(name, "ext4") });
		}
		if (e.unmount)
			items.push({ label: "Unmount", danger: false, op: "unmountStorage", action: () => unmountDisk(name) });
		if (e.grow)
			items.push({ label: "Grow\u2026", danger: false, op: "resizeStoragePartition", action: () => growPartition(d) });
		if (e.del)
			items.push({ label: "Delete partition", danger: true, op: "deleteStoragePartition", disabled: e.delDisabled,
			             title: e.delDisabled ? "Unmount it first." : "",
			             action: () => deletePartition(d.parent_disk, name) });
		return items.length ? items : null;
	}
	if (category === "volumes")
		return [{ label: "Delete volume", danger: true, op: "deleteVolume", action: () => deleteVolumeByName(name) }];
	if (category === "networks")
		return [{ label: "Remove", danger: true, op: "deleteNetwork", action: () => removeNetwork(name) }];
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

bindStaticPageForms();
restoreLastViewIfNoHash();
/* The log panel is on every page, so its live read lasts as long as the
 * page does (ADR-0339 phase 2). */
startLiveRead("log-panel", CIX_API.getSystemLogs() + "?since=0&tail=" + LOG_TAIL, ingestServerLogs);
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
		/* ADR-0339: a hidden tab only noted that something changed; it
		 * refreshes once now. Its timers were throttled while hidden, so
		 * the stream's silence is checked here rather than trusted to
		 * the watchdog. */
		eventStream.checkSilence();
		if (eventsStaleWhileHidden) {
			eventsStaleWhileHidden = false;
			scheduleEventRefresh();
		}
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
refreshStatusBar();
setInterval(renderStatusMeta, 1000);
setInterval(() => {
	if (!document.hidden)
		refreshStatusBar();
}, 60000);
