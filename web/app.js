/*
 * thinC dashboard: a pure client of docs/api/openapi.yaml, same as
 * thincctl (cli/src/main.c) -- no capability here that isn't already
 * one of the REST endpoints. Vanilla JS, no framework, no build step
 * (docs/adr/0010). Left resource tree + per-resource detail views,
 * routed via location.hash (#category or #category/name) -- no router
 * library, matching this project's own hand-rolled-over-dependency
 * posture everywhere else.
 */
"use strict";

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
	disks: [],
	diskRoles: [],
	diskFormatStatus: {},
	stateStorage: { disk: null },
	stateStorageMigrate: { state: "none" },
	logStorage: { disk: null },
	logStorageMigrate: { state: "none" },
	rebuildableStorage: { disk: null },
	rebuildableStorageMigrate: { state: "none" },
	backupConfig: { disk: null, enabled: false, interval_hours: 0 },
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

const healthBadge = document.getElementById("health");
const statusBox = document.getElementById("status");
const treeEl = document.getElementById("tree");
const logOutput = document.getElementById("log-output");
const logPanel = document.getElementById("log-panel");
const logPanelHeader = document.getElementById("log-panel-header");
const logPanelArrow = document.getElementById("log-panel-arrow");
const logPanelSource = document.getElementById("log-panel-source");

const LOG_COLLAPSE_KEY = "thinc-log-collapsed";
const LOG_HEIGHT_KEY = "thinc-log-height";

/* Collapsing/expanding must also manage #log-panel's own inline
 * flex-basis -- makeResizable() below (log-panel-resize-handle) sets
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
		logPanel.style.flexBasis = "";
	} else {
		try {
			const saved = localStorage.getItem(LOG_HEIGHT_KEY);

			if (saved !== null) logPanel.style.flexBasis = saved + "px";
		} catch (e) {
			/* localStorage unavailable -- default flex-basis (#log-panel's
			 * own base 200px rule) stands. */
		}
	}
	try {
		localStorage.setItem(LOG_COLLAPSE_KEY, collapsed ? "1" : "0");
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
	setLogCollapsed(localStorage.getItem(LOG_COLLAPSE_KEY) === "1");
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
 * localStorage (thinc-tree-width/thinc-log-height) and restores on
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
			localStorage.setItem(storageKey, String(lastSize));
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
		const saved = localStorage.getItem(storageKey);

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
	storageKey: "thinc-tree-width",
	min: 160,
	max: 480,
	getSize: () => document.querySelector(".tree-panel").getBoundingClientRect().width,
	apply: (size) => {
		document.querySelector(".layout").style.gridTemplateColumns = size + "px 1fr";
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
		logPanel.style.flexBasis = size + "px";
	},
	skipInitialApplyIf: () => logPanel.classList.contains("collapsed"),
});

/* ---------- theme toggle (light / dark / auto) ---------- */

const THEME_KEY = "thinc-theme";
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

/* Same inline-SVG style as TREE_ICONS (viewBox 16x16, currentColor
 * stroke, 1.3 stroke-width) -- sun / crescent-moon / monitor, so the
 * toggle reads as an icon button instead of an emoji+text label. */
const THEME_ICONS = {
	light: '<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><circle cx="8" cy="8" r="3"/><line x1="8" y1="1" x2="8" y2="2.5"/><line x1="8" y1="13.5" x2="8" y2="15"/><line x1="1" y1="8" x2="2.5" y2="8"/><line x1="13.5" y1="8" x2="15" y2="8"/><line x1="3.05" y1="3.05" x2="4.1" y2="4.1"/><line x1="11.9" y1="11.9" x2="12.95" y2="12.95"/><line x1="3.05" y1="12.95" x2="4.1" y2="11.9"/><line x1="11.9" y1="4.1" x2="12.95" y2="3.05"/></g></svg>',
	dark: '<svg viewBox="0 0 16 16" width="14" height="14"><path d="M13 8.5 A5.5 5.5 0 1 1 7.5 3 A4.2 4.2 0 0 0 13 8.5 Z" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"/></svg>',
	auto: '<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="3" width="12" height="8" rx="1"/><line x1="5.5" y1="14" x2="10.5" y2="14"/><line x1="8" y1="11" x2="8" y2="14"/></g></svg>',
};

function applyTheme(theme) {
	if (theme === "light" || theme === "dark")
		document.documentElement.setAttribute("data-theme", theme);
	else
		document.documentElement.removeAttribute("data-theme");
	themeToggle.innerHTML = THEME_ICONS[theme];
	themeToggle.title = "Theme: " + (theme === "dark" ? "Dark" : theme === "light" ? "Light" : "Auto") + " (click to cycle)";
	try {
		localStorage.setItem(THEME_KEY, theme);
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
function closeAllMenus() {
	for (const menu of document.querySelectorAll(".menu-dropdown-menu"))
		menu.hidden = true;
}

for (const dropdown of document.querySelectorAll(".menu-dropdown")) {
	const toggle = dropdown.querySelector(".menu-dropdown-toggle");
	const menu = dropdown.querySelector(".menu-dropdown-menu");

	toggle.addEventListener("click", (event) => {
		event.stopPropagation();
		const shouldOpen = menu.hidden;
		closeAllMenus();
		menu.hidden = !shouldOpen;
	});
	menu.addEventListener("click", (event) => {
		if (event.target.closest("a, button"))
			menu.hidden = true;
	});
}
document.addEventListener("click", closeAllMenus);
document.addEventListener("keydown", (event) => {
	if (event.key === "Escape")
		closeAllMenus();
});

/* Create-action items (data-modal, shared across every topical menu
 * above) all open the same modal shell the old single +Create
 * dropdown already used -- only where they live changed. */
for (const item of document.querySelectorAll(".menu-dropdown-menu button[data-modal]")) {
	item.addEventListener("click", () => openModal(item.dataset.modal, item.dataset.title));
}

/* About thinC: read-only, fetched fresh on every open rather than
 * cached -- build/slot/kernel can change under an operator's feet
 * (an update staged to the inactive slot, a reboot) and this is the
 * one place meant to answer "what is this box actually running right
 * now," so a stale answer would defeat its own purpose. */
document.getElementById("menu-about").addEventListener("click", async () => {
	const instanceEl = document.getElementById("about-instance");
	const buildEl = document.getElementById("about-build");
	const slotEl = document.getElementById("about-slot");
	const kernelEl = document.getElementById("about-kernel");

	instanceEl.textContent = buildEl.textContent = slotEl.textContent = kernelEl.textContent = "…";
	try {
		const [boot, site] = await Promise.all([
			apiRequest("GET", "/v1/system/boot"),
			apiRequest("GET", "/v1/system/site"),
		]);
		const fqdn = site.site_name
			? site.instance_name + "." + site.site_name + "." + site.domain_suffix
			: site.instance_name + "." + site.domain_suffix;

		instanceEl.textContent = fqdn;
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
 * analog of thincctl's ~/.thincctl_token dotfile, so a page reload
 * doesn't force a fresh login. GET requests never need it (the
 * daemon's write-gating leaves every GET open, always, regardless of
 * auth state); apiRequest()/apiRequestRaw() below attach it to every
 * request automatically once set -- one place, not every one of this
 * file's many call sites, the same design kx_client_set_token() gives
 * thincctl (client/include/httpclient.h).
 */
let authToken = localStorage.getItem("thinc-auth-token") || null;
let authUsername = localStorage.getItem("thinc-auth-username") || null;

const authStatusEl = document.getElementById("auth-status");
const authActionBtn = document.getElementById("menu-auth-action");

function updateAuthUi() {
	if (authToken) {
		authStatusEl.hidden = false;
		authStatusEl.className = "badge badge-ok";
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
		localStorage.setItem("thinc-auth-token", token);
		localStorage.setItem("thinc-auth-username", username || "");
	} else {
		localStorage.removeItem("thinc-auth-token");
		localStorage.removeItem("thinc-auth-username");
	}
	updateAuthUi();
}

/* Opens the login modal on a 401 from any request, so "why did my
 * action just fail" has an immediate, actionable answer instead of
 * only a status-bar error -- the same reasoning thincctl's own
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
			await apiRequest("POST", "/v1/logout");
		} catch (e) {
			/* best-effort -- matches thincctl's own idempotent-logout
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
		const result = await apiRequest("POST", "/v1/login", { username: username, password: password });

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
 * log store (kernel/thincd/audit/container, GET /v1/system/logs,
 * ADR-0070/ADR-0126) merged in via periodic polling (pollServerLogs(),
 * called from the main poll() loop). logBuffer is the one source of
 * truth both the live-append path and a full filter-change re-render
 * read from -- capped at MAX_LOG_ENTRIES total, across every source
 * combined (a dedicated Logs page still exists, under System > Server,
 * for browsing/configuring the server's own full history/size cap;
 * this panel is a live tail, not a history browser).
 */
const MAX_LOG_ENTRIES = 300;
let logBuffer = [];
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
 * entry (see addLogEntriesBatch() below -- a real, confirmed-live
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

/* Trims the rendered DOM (not logBuffer, which callers already trim
 * themselves) down to MAX_LOG_ENTRIES and scrolls to the bottom --
 * pulled out so both the single-entry and batch paths share it, each
 * calling it exactly once per operation regardless of how many
 * entries that operation added. */
function trimAndScrollLogOutput() {
	while (logOutput.children.length > MAX_LOG_ENTRIES)
		logOutput.removeChild(logOutput.firstChild);
	logOutput.scrollTop = logOutput.scrollHeight;
}

/* Single real-time entry (a toast, or one web-ui action) -- rare
 * enough that one reflow per call is fine. */
function addLogEntry(ts, source, level, text, kind) {
	const entry = { ts, source, level, text, kind };

	logBuffer.push(entry);
	while (logBuffer.length > MAX_LOG_ENTRIES)
		logBuffer.shift();
	if (logSourceFilter === "" || logSourceFilter === source) {
		logOutput.appendChild(buildLogEntryDom(entry));
		trimAndScrollLogOutput();
	}
}

/* The polled-server-logs path (pollServerLogs() below can hand this
 * dozens of entries in one call) -- one DocumentFragment append and
 * one trim/scroll for the whole batch, not one each per entry. */
function addLogEntriesBatch(entries) {
	const fragment = document.createDocumentFragment();
	let rendered = false;

	for (const entry of entries) {
		logBuffer.push(entry);
		if (logSourceFilter === "" || logSourceFilter === entry.source) {
			fragment.appendChild(buildLogEntryDom(entry));
			rendered = true;
		}
	}
	while (logBuffer.length > MAX_LOG_ENTRIES)
		logBuffer.shift();
	if (rendered) {
		logOutput.appendChild(fragment);
		trimAndScrollLogOutput();
	}
}

function rerenderLogPanel() {
	const fragment = document.createDocumentFragment();

	for (const entry of logBuffer) {
		if (logSourceFilter === "" || logSourceFilter === entry.source)
			fragment.appendChild(buildLogEntryDom(entry));
	}
	logOutput.textContent = "";
	logOutput.appendChild(fragment);
	logOutput.scrollTop = logOutput.scrollHeight;
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

/* Merges newly-polled server-side entries into the same buffer/panel,
 * source-tagged by their own real source (kernel/thincd/audit/
 * container) rather than "web-ui". Deliberately starts from "now"
 * (logsSinceTs set at declaration time below) rather than 0 -- this
 * panel is a live tail from page-load onward, not a full-history
 * backfill (GET /system/logs with no since= would return the entire
 * store on first poll, flooding the panel). since= is an inclusive
 * floor (logstore_tail_ex()'s own "ts < since" exclusion) and this
 * store's own ts resolution is whole seconds, so a naive since=lastTs
 * poll would re-return (and re-render) every entry still exactly at
 * that boundary second -- logsSeenAtSinceTs dedupes those specifically,
 * not the whole history, since only the current boundary second can
 * ever repeat across polls. */
let logsSinceTs = Math.floor(Date.now() / 1000);
let logsSeenAtSinceTs = new Set();
const LOG_ERROR_LEVELS = new Set(["emerg", "alert", "crit", "err", "error", "warning", "warn"]);

function serverLogEntryKey(e) {
	return e.source + "|" + e.container + "|" + e.level + "|" + e.msg;
}

async function pollServerLogs() {
	let entries;

	try {
		entries = await apiRequest("GET", "/v1/system/logs?since=" + logsSinceTs + "&tail=500");
	} catch (e) {
		return; /* best-effort, matches every other poll()'s own error tolerance */
	}
	if (!entries) return;

	let maxTs = logsSinceTs;
	let newAtMax = new Set();
	const toAdd = [];

	for (const e of entries) {
		if (e.ts < logsSinceTs) continue; /* defensive -- since= should already exclude these */
		if (e.ts === logsSinceTs && logsSeenAtSinceTs.has(serverLogEntryKey(e))) continue;

		const text = e.container ? "(" + e.container + ") " + e.msg : e.msg;
		const kind = LOG_ERROR_LEVELS.has(e.level) ? "error" : "ok";

		toAdd.push({ ts: e.ts, source: e.source, level: e.level, text, kind });

		if (e.ts > maxTs) {
			maxTs = e.ts;
			newAtMax = new Set([serverLogEntryKey(e)]);
		} else if (e.ts === maxTs) {
			newAtMax.add(serverLogEntryKey(e));
		}
	}
	if (toAdd.length > 0)
		addLogEntriesBatch(toAdd);
	if (maxTs > logsSinceTs) {
		logsSinceTs = maxTs;
		logsSeenAtSinceTs = newAtMax;
	} else {
		for (const k of newAtMax)
			logsSeenAtSinceTs.add(k);
	}
}

/* Toast (ADR-0129): a fixed-position popup with a disappear timer,
 * replacing the old always-content-flow status bar -- same element/
 * id/classes (only the CSS positioning changed), so every existing
 * showStatus()/clearStatus() call site needed no change. Every toast
 * also lands in the merged log panel above (source "web-ui") -- the
 * user's own request: "it should be shown in the logs right?". */
const STATUS_AUTO_DISMISS_MS = 5000;
let statusTimeoutId = null;

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

async function apiRequest(method, path, body) {
	const opts = { method: method, headers: {} };
	if (authToken)
		opts.headers["Authorization"] = "Bearer " + authToken;
	if (body !== undefined) {
		opts.headers["Content-Type"] = "application/json";
		opts.body = JSON.stringify(body);
	}
	const res = await fetch(path, opts);
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

		if (method !== "GET")
			logLine(method, path, "-> " + res.status + " " + message, "error");
		if (res.status === 401 && path !== "/v1/login")
			promptReauth();
		throw new Error(message);
	}
	if (method !== "GET")
		logLine(method, path, "-> " + res.status, "ok");
	return json;
}

/* Raw variant for /v1/system/backup|restore -- the response/request
 * body IS the bundle, written/read byte-for-byte, the same "exact
 * round trip, not re-serialized" guarantee thincctl's own backup/
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

async function refreshHealth() {
	const start = performance.now();

	try {
		await apiRequest("GET", "/v1/health");
		const ms = Math.round(performance.now() - start);

		consecutiveHealthFailures = 0;
		healthBadge.className = "health-dot health-dot-ok";
		healthBadge.title = "Daemon reachable — " + ms + "ms";
	} catch (e) {
		consecutiveHealthFailures++;
		if (consecutiveHealthFailures >= 2) {
			healthBadge.className = "health-dot health-dot-error";
			healthBadge.title = "Daemon unreachable (" + consecutiveHealthFailures + " consecutive failed checks)";
		} else {
			healthBadge.className = "health-dot health-dot-degraded";
			healthBadge.title = "Daemon check failed once — retrying";
		}
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
const LAST_VIEW_KEY = "thinc-last-view";

function saveLastView() {
	try {
		localStorage.setItem(LAST_VIEW_KEY, location.hash);
	} catch (e) {
		/* localStorage unavailable -- last view just won't survive a
		 * fresh tab/reload; starts on the default view instead. */
	}
}

function restoreLastViewIfNoHash() {
	if (location.hash !== "")
		return;
	try {
		const saved = localStorage.getItem(LAST_VIEW_KEY);

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
	routes: "view-routes",
	images: "view-images",
	devices: "view-devices",
	kmod: "view-kmod",
	sysctl: "view-sysctl",
	disks: "view-disks",
	"dns-records": "view-dns-records",
	"dns-servers": "view-dns-servers",
	"ldap-servers": "view-ldap-servers",
	"ldap-groups": "view-ldap-groups",
	"ldap-users": "view-ldap-users",
	"ldap-config": "view-ldap-config",
	"ntp-config": "view-ntp-config",
	"ntp-servers": "view-ntp-servers",
	"ntp-time": "view-ntp-time",
	"pki-ca": "view-pki-ca",
	"pki-intermediate": "view-pki-intermediate",
	"pki-certs": "view-pki-certs",
	packages: "view-packages",
	recipes: "view-recipes",
	"pkg-repo": "view-pkg-repo",
	"pkg-cache": "view-pkg-cache",
	site: "view-site",
	"daemon-config": "view-daemon-config",
	"host-swap": "view-host-swap",
	"rolling-restart": "view-rolling-restart",
	"pkg-build-config": "view-pkg-build-config",
	"hostauth-sessions": "view-hostauth-sessions",
	"host-stats": "view-host-stats",
	processes: "view-processes",
	"syslog-targets": "view-syslog-targets",
	"tls-throttle": "view-tls-throttle",
	logs: "view-logs",
	backup: "view-backup",
	update: "view-update",
};

const DETAIL_VIEWS = {
	containers: "view-container-detail",
	networks: "view-network-detail",
	images: "view-image-detail",
	packages: "view-package-detail",
};

function renderCurrentView() {
	const route = parseHash();
	const allViews = document.querySelectorAll(".view");

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
		else if (route.category === "routes")
			renderRoutesList();
		else if (route.category === "sysctl")
			renderSysctlList();
		else if (route.category === "kmod")
			renderKmodList();
		else if (route.category === "host-stats")
			startHostStatsPolling();
		else if (route.category === "processes")
			renderProcessesList();
		else if (route.category === "syslog-targets")
			renderSyslogTargetsList();
		else if (route.category === "tls-throttle")
			refreshTlsThrottleStatus();
		else if (route.category === "logs")
			renderLogsList();
		else if (route.category === "images" && route.name !== null)
			renderImageDetail(route.name);
		else if (route.category === "devices")
			renderDevices();
		else if (route.category === "disks") {
			renderDisks();
			renderAllStoragePlacements();
		}
		else if (route.category === "packages")
			renderPackagesView(route.name);
		else if (route.category === "recipes") {
			renderRecipesList();
			renderImageRecipesTable();
			renderContainerRecipesTable();
		}
		else if (route.category === "backup")
			renderBackupConfig();
		else if (route.category === "hostauth-sessions")
			refreshHostauthSessions();
	}

	renderTreeActive();
}

window.addEventListener("hashchange", () => {
	renderCurrentView();
	ensureActiveCategoryExpanded();
});

/* ---------- tree ---------- */

const TREE_COLLAPSE_KEY = "thinc-tree-collapsed";

function loadCollapsedCategories() {
	try {
		const raw = localStorage.getItem(TREE_COLLAPSE_KEY);

		return raw ? new Set(JSON.parse(raw)) : new Set();
	} catch (e) {
		return new Set();
	}
}

function saveCollapsedCategories() {
	try {
		localStorage.setItem(TREE_COLLAPSE_KEY, JSON.stringify(Array.from(collapsedCategories)));
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
 * Small hand-rolled inline SVG icons, Proxmox-style -- no external
 * icon font/library/CDN request (this project's own no-external-
 * dependencies rule, same posture as the console's hand-rolled
 * terminal renderer). stroke="currentColor" so each one inherits
 * whatever color its own link text already has (theme-aware for
 * free, no separate light/dark icon set needed). Visually verified by
 * rendering this exact path data via rsvg-convert before wiring it
 * in, not trusted blind.
 */
const TREE_ICONS = {
	containers:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="3" width="12" height="10" rx="1"/><line x1="2" y1="8" x2="14" y2="8"/><circle cx="5" cy="5.5" r="0.6" fill="currentColor" stroke="none"/><circle cx="5" cy="10.5" r="0.6" fill="currentColor" stroke="none"/></g></svg>',
	networks:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><circle cx="8" cy="3" r="1.5"/><circle cx="3" cy="12" r="1.5"/><circle cx="13" cy="12" r="1.5"/><line x1="8" y1="4.5" x2="3" y2="10.5"/><line x1="8" y1="4.5" x2="13" y2="10.5"/></g></svg>',
	images:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><polyline points="8,2 14,5 8,8 2,5 8,2"/><polyline points="2,8 8,11 14,8"/><polyline points="2,11 8,14 14,11"/></g></svg>',
	packages:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><polyline points="8,2 14,5 14,11 8,14 2,11 2,5 8,2"/><line x1="8" y1="2" x2="8" y2="8"/><line x1="2" y1="5" x2="8" y2="8"/><line x1="14" y1="5" x2="8" y2="8"/></g></svg>',
	recipes:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="1.5" width="10" height="13" rx="1"/><line x1="5.5" y1="5" x2="10.5" y2="5"/><line x1="5.5" y1="8" x2="10.5" y2="8"/><line x1="5.5" y1="11" x2="9" y2="11"/></g></svg>',
	system:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><circle cx="8" cy="8" r="3"/><line x1="8" y1="1" x2="8" y2="3"/><line x1="8" y1="13" x2="8" y2="15"/><line x1="1" y1="8" x2="3" y2="8"/><line x1="13" y1="8" x2="15" y2="8"/><line x1="3.5" y1="3.5" x2="5" y2="5"/><line x1="11" y1="11" x2="12.5" y2="12.5"/><line x1="3.5" y1="12.5" x2="5" y2="11"/><line x1="11" y1="5" x2="12.5" y2="3.5"/></g></svg>',
	pki:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><rect x="3.5" y="7" width="9" height="7" rx="1"/><path d="M5.5 7 V4.5 a2.5 2.5 0 0 1 5 0 V7"/></g></svg>',
	dns:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><circle cx="8" cy="8" r="6.5"/><line x1="1.5" y1="8" x2="14.5" y2="8"/><path d="M8 1.5 C5 4.5 5 11.5 8 14.5 C11 11.5 11 4.5 8 1.5"/></g></svg>',
	backup:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="2" width="12" height="12" rx="1"/><line x1="2" y1="5.5" x2="14" y2="5.5"/><line x1="6" y1="9" x2="10" y2="9"/></g></svg>',
	devices:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><rect x="4.5" y="4.5" width="7" height="7" rx="0.5"/><line x1="6" y1="1.5" x2="6" y2="4.5"/><line x1="10" y1="1.5" x2="10" y2="4.5"/><line x1="6" y1="11.5" x2="6" y2="14.5"/><line x1="10" y1="11.5" x2="10" y2="14.5"/><line x1="1.5" y1="6" x2="4.5" y2="6"/><line x1="1.5" y1="10" x2="4.5" y2="10"/><line x1="11.5" y1="6" x2="14.5" y2="6"/><line x1="11.5" y1="10" x2="14.5" y2="10"/></g></svg>',
	update:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><path d="M13 8 A5 5 0 1 1 11 4"/><polyline points="13,2 13,5.5 9.5,5.5"/></g></svg>',
	stats:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><line x1="1.5" y1="14.5" x2="14.5" y2="14.5"/><rect x="3" y="9" width="2.5" height="5.5"/><rect x="6.75" y="5" width="2.5" height="9.5"/><rect x="10.5" y="7.5" width="2.5" height="7"/></g></svg>',
	disks:
		'<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><ellipse cx="8" cy="4" rx="6" ry="2.2"/><path d="M2 4 V12 A6 2.2 0 0 0 14 12 V4"/><path d="M2 8 A6 2.2 0 0 0 14 8"/></g></svg>',
};

/* colorClass tints the icon itself (via CSS "color", which the icon's
 * own stroke="currentColor" inherits) instead of a separate status dot
 * next to it -- one visual signal, not two. */
function treeIcon(name, colorClass) {
	const span = document.createElement("span");

	span.className = "tree-icon" + (colorClass ? " " + colorClass : "");
	span.innerHTML = TREE_ICONS[name] || "";
	return span;
}

function treeLink(href, text, className, icon, iconColorClass) {
	const a = document.createElement("a");

	a.href = href;
	a.className = className;
	if (icon)
		a.appendChild(treeIcon(icon, iconColorClass));
	a.appendChild(document.createTextNode(text));
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
	a.appendChild(document.createTextNode(label));
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

		toggle.addEventListener("click", (event) => {
			event.preventDefault();
			const nowCollapsed = !ul.hidden;

			ul.hidden = nowCollapsed;
			toggle.textContent = nowCollapsed ? "▸" : "▾";
			if (nowCollapsed)
				collapsedCategories.add(path);
			else
				collapsedCategories.delete(path);
			saveCollapsedCategories();
		});
	}
	parentUl.appendChild(li);
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

	const topLevel = [
		{
			label: "Containers",
			hash: "containers",
			icon: "containers",
			children: cache.containers.map((c) => ({
				label: c.name,
				hash: "containers/" + encodeURIComponent(c.name),
				status: c.status,
				icon: "containers",
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
				/* No real "status" concept for a network the way a container
				 * has one -- the closest real, available signal is whether
				 * anything is actually attached to it right now (green) vs.
				 * created but currently unused (grey), same "tint the icon,
				 * not a separate dot" treatment containers get. */
				iconColor: n.interfaces && n.interfaces.length > 0 ? "tree-icon-ok" : "tree-icon-idle",
			})),
		},
		{
			/* Aliases to its own first child's hash ("recipes", same as
			 * Catalog's own address, same as Catalog's own first child
			 * "Recipes"'s address) -- same convention every group here
			 * uses, see System's own comment below for why this matters.
			 *
			 * Two groups, split along a real axis (issue #41): Catalog is
			 * the declared source of truth and how it's distributed
			 * (Recipes -- already has Package/Image/Container tabs with
			 * inline apply/install actions per row -- plus how that
			 * catalog gets synced and where its build cache lives);
			 * Build & Deploy is live state -- what's actually installed/
			 * running right now, and where new installs/builds are
			 * triggered and tracked. Recipes used to sit alongside
			 * Images/Packages under one "Catalog" label despite being a
			 * fundamentally different kind of thing (declared vs. live),
			 * and the old "Build Pipeline" label was a misnomer -- it was
			 * always sync/cache, never building. */
			label: "Software",
			hash: "recipes",
			icon: "recipes",
			children: [
				{
					label: "Catalog",
					hash: "recipes",
					icon: "recipes",
					children: [
						{ label: "Recipes", hash: "recipes", icon: "recipes" },
						{ label: "Repo & Sync", hash: "pkg-repo", icon: "recipes" },
						{ label: "Cache & Artifacts", hash: "pkg-cache", icon: "recipes" },
					],
				},
				{
					label: "Build & Deploy",
					hash: "images",
					icon: "images",
					children: [
						{ label: "Images", hash: "images", icon: "images" },
						{ label: "Packages", hash: "packages", icon: "packages" },
					],
				},
			],
		},
		{
			/* Aliases to its own FIRST child's hash ("pki-ca", same as
			 * PKI's own address, same as PKI's own first child "Root CA"'s
			 * address) -- matching the exact convention every other group
			 * here already uses (Software->images, DNS->dns-records,
			 * Backup->backup). Aliasing to anything other than the first
			 * child is a real bug, not just a style choice: whatever hash
			 * System's own address shares gets highlighted/expanded
			 * alongside it (renderTreeActive()/ensureActiveCategoryExpanded()
			 * both walk shared hashes as one identity, by design, since
			 * they really do route to the identical page) -- aliasing to
			 * "update" (an unrelated, distant sibling) instead of the
			 * first child made an unrelated leaf light up any time System
			 * itself was merely an ancestor of whatever page was actually
			 * showing, which looked exactly as random as it was. */
			label: "System",
			hash: "pki-ca",
			icon: "system",
			children: [
				{
					label: "PKI",
					hash: "pki-ca",
					icon: "pki",
					children: [
						{ label: "Root CA", hash: "pki-ca", icon: "pki" },
						{ label: "Intermediate CA", hash: "pki-intermediate", icon: "pki" },
						{ label: "Certificates", hash: "pki-certs", icon: "pki" },
					],
				},
				{
					label: "DNS",
					hash: "dns-records",
					icon: "dns",
					children: [
						{ label: "Records", hash: "dns-records", icon: "dns" },
						{ label: "Servers", hash: "dns-servers", icon: "dns" },
					],
				},
				{
					label: "LDAP",
					hash: "ldap-servers",
					icon: "dns",
					children: [
						{ label: "Servers", hash: "ldap-servers", icon: "dns" },
						{ label: "Groups", hash: "ldap-groups", icon: "dns" },
						{ label: "Users", hash: "ldap-users", icon: "dns" },
						{ label: "Config", hash: "ldap-config", icon: "dns" },
					],
				},
				{
					label: "NTP",
					hash: "ntp-config",
					icon: "dns",
					children: [
						{ label: "Config", hash: "ntp-config", icon: "dns" },
						{ label: "Servers", hash: "ntp-servers", icon: "dns" },
						{ label: "Time & Sync", hash: "ntp-time", icon: "dns" },
					],
				},
				{
					/* Core host identity/hardware/network config -- this
					 * install's own instance identity (Site), listener
					 * config, physical device inventory, and the kernel
					 * routing table, plus the two smaller settings
					 * (Host Swap, Rolling Restart) that used to be bundled
					 * onto the Daemon page with no navigation of their own. */
					label: "Host",
					hash: "daemon-config",
					icon: "system",
					children: [
						{ label: "Daemon", hash: "daemon-config", icon: "system" },
						{ label: "Site", hash: "site", icon: "dns" },
						{ label: "Devices", hash: "devices", icon: "devices" },
						{ label: "Disks", hash: "disks", icon: "disks" },
						{ label: "Routes", hash: "routes", icon: "networks" },
						{ label: "Host Swap", hash: "host-swap", icon: "system" },
						{ label: "Rolling Restart", hash: "rolling-restart", icon: "system" },
						{ label: "Package Builds", hash: "pkg-build-config", icon: "system" },
						{ label: "Sessions", hash: "hostauth-sessions", icon: "system" },
					],
				},
				{
					/* Everything about observing/recording what the box is
					 * doing -- live resource graphs, the process table, and
					 * the two ends of the log pipeline (local store config,
					 * forward-target registration) together. */
					label: "Monitoring",
					hash: "host-stats",
					icon: "stats",
					children: [
						{ label: "Host Stats", hash: "host-stats", icon: "stats" },
						{ label: "Processes", hash: "processes", icon: "stats" },
						{ label: "Log Store", hash: "logs", icon: "system" },
						{ label: "Syslog Targets", hash: "syslog-targets", icon: "system" },
					],
				},
				{
					/* Protecting and evolving the running system over time. */
					label: "Maintenance",
					hash: "tls-throttle",
					icon: "system",
					children: [
						{ label: "TLS Throttle", hash: "tls-throttle", icon: "system" },
						{ label: "Update", hash: "update", icon: "update" },
						{ label: "Backup", hash: "backup", icon: "backup" },
					],
				},
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
function renderTreeActive() {
	const owning = new Set();

	for (let id = findCurrentNodeId(); id !== undefined; id = nodeParent[id])
		owning.add(id);

	for (const [id, anchor] of Object.entries(nodeAnchor))
		anchor.classList.toggle("active", owning.has(Number(id)));
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
			"badge " + (c.status === "running" ? "badge-ok" : c.status === "paused" ? "badge-paused" : "badge-unknown");
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

async function refreshContainers() {
	const data = await apiRequest("GET", "/v1/containers");
	cache.containers = data.containers;
	renderContainers(cache.containers);
}

async function removeContainer(name) {
	try {
		await apiRequest("DELETE", "/v1/containers/" + encodeURIComponent(name));
		clearStatus();
		if (parseHash().category === "containers" && parseHash().name === name)
			location.hash = "#containers";
		await refreshContainers();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove " + name + ": " + e.message, true);
	}
}

async function stopContainer(name) {
	try {
		await apiRequest("POST", "/v1/containers/" + encodeURIComponent(name) + "/stop");
		clearStatus();
		await refreshContainers();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to stop " + name + ": " + e.message, true);
	}
}

async function startContainer(name) {
	try {
		await apiRequest("POST", "/v1/containers/" + encodeURIComponent(name) + "/start");
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
		await apiRequest("POST", "/v1/containers/" + encodeURIComponent(name) + "/pause");
		clearStatus();
		await refreshContainers();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to pause " + name + ": " + e.message, true);
	}
}

async function unpauseContainer(name) {
	try {
		await apiRequest("POST", "/v1/containers/" + encodeURIComponent(name) + "/unpause");
		clearStatus();
		await refreshContainers();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to unpause " + name + ": " + e.message, true);
	}
}

function fieldBlock(label, value) {
	const div = document.createElement("div");
	const labelSpan = document.createElement("span");

	labelSpan.className = "field-label";
	labelSpan.textContent = label;
	div.appendChild(labelSpan);
	const valueDiv = document.createElement("div");

	valueDiv.textContent = value;
	div.appendChild(valueDiv);
	return div;
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

/* ---------- Minimal terminal (no framework -- ADR-0010/ADR-0043) ----------
 *
 * A line-buffer terminal, not a full VT100 emulator: \r/\n/backspace/Tab
 * plus SGR color codes (CSI ... m) are interpreted; every other CSI
 * escape sequence (cursor addressing, clear-screen, etc.) is recognized
 * structurally (so it never leaks into the output as literal garbage
 * text) but otherwise silently discarded -- there is no 2D cursor-
 * addressable screen model here, so full-screen redraw programs (vim,
 * top, less) render wrong. A stated, accepted boundary (see ADR-0043),
 * not an oversight -- `thincctl console` has no such limitation.
 */
const TERM_MAX_LINES = 2000;

function createTerminal(outputEl) {
	let lines = [[]];
	let cursorRow = 0;
	let cursorCol = 0;
	let escState = null; /* null | "esc" | { csi: string } */
	let sgr = { bold: false, fg: null, bg: null };

	function currentStyleClass() {
		const parts = [];

		if (sgr.bold)
			parts.push("term-bold");
		if (sgr.fg !== null)
			parts.push("term-fg-" + sgr.fg);
		if (sgr.bg !== null)
			parts.push("term-bg-" + sgr.bg);
		return parts.join(" ");
	}

	function applySgr(paramStr) {
		const codes = paramStr.length > 0 ? paramStr.split(";").map((s) => parseInt(s, 10)) : [0];

		for (const code of codes) {
			if (isNaN(code) || code === 0)
				sgr = { bold: false, fg: null, bg: null };
			else if (code === 1)
				sgr.bold = true;
			else if (code === 22)
				sgr.bold = false;
			else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97))
				sgr.fg = code;
			else if (code === 39)
				sgr.fg = null;
			else if ((code >= 40 && code <= 47) || (code >= 100 && code <= 107))
				sgr.bg = code;
			else if (code === 49)
				sgr.bg = null;
		}
	}

	function putChar(ch) {
		while (lines.length <= cursorRow)
			lines.push([]);
		const line = lines[cursorRow];
		const cls = currentStyleClass();

		while (line.length <= cursorCol)
			line.push({ ch: " ", cls: "" });
		line[cursorCol] = { ch: ch, cls: cls };
		cursorCol++;
	}

	function render() {
		outputEl.textContent = "";
		for (const line of lines) {
			const lineDiv = document.createElement("div");
			let i = 0;

			while (i < line.length) {
				const cell = line[i];
				let text = cell.ch;
				let j = i + 1;

				while (j < line.length && line[j].cls === cell.cls) {
					text += line[j].ch;
					j++;
				}
				if (cell.cls === "") {
					lineDiv.appendChild(document.createTextNode(text));
				} else {
					const span = document.createElement("span");

					span.className = cell.cls;
					span.textContent = text;
					lineDiv.appendChild(span);
				}
				i = j;
			}
			outputEl.appendChild(lineDiv);
		}
		outputEl.scrollTop = outputEl.scrollHeight;
	}

	function feed(text) {
		for (let i = 0; i < text.length; i++) {
			const ch = text[i];
			const code = text.charCodeAt(i);

			if (escState === "esc") {
				escState = ch === "[" ? { csi: "" } : null;
				continue;
			}
			if (escState !== null) {
				if (code >= 0x40 && code <= 0x7e) {
					if (ch === "m")
						applySgr(escState.csi);
					escState = null;
				} else {
					escState.csi += ch;
				}
				continue;
			}

			if (ch === "\x1b") {
				escState = "esc";
			} else if (ch === "\r") {
				cursorCol = 0;
			} else if (ch === "\n") {
				cursorRow++;
				cursorCol = 0; /* most output pairs \r\n; treating a bare \n the
				                * same way avoids staircase text in the common case */
				if (cursorRow >= TERM_MAX_LINES) {
					lines.shift();
					cursorRow--;
				}
			} else if (ch === "\b" || ch === "\x7f") {
				if (cursorCol > 0)
					cursorCol--;
			} else if (ch === "\t") {
				cursorCol = Math.min(cursorCol + (8 - (cursorCol % 8)), 512);
			} else if (code >= 0x20) {
				putChar(ch);
			}
			/* other control bytes (bell, etc.) ignored */
		}
		render();
	}

	return { feed: feed };
}

let consoleWs = null;
let consoleTerminal = null;
let consoleContainerName = null;
let currentContainerDetailName = null;

function closeConsole() {
	if (consoleWs !== null) {
		consoleWs.close();
		consoleWs = null;
	}
	consoleTerminal = null;
	consoleContainerName = null;
}

function openConsole(name) {
	if (consoleContainerName === name && consoleWs !== null)
		return; /* already connected to this exact container -- a poll-driven
		         * re-render of the same detail view must never reopen this,
		         * or every keystroke/prompt would reset every 2s */

	closeConsole();
	consoleContainerName = name;

	const outputEl = document.getElementById("cd-console-output");
	const statusEl = document.getElementById("cd-console-status");

	outputEl.textContent = "";
	consoleTerminal = createTerminal(outputEl);
	statusEl.textContent = "connecting…";

	const proto = location.protocol === "https:" ? "wss:" : "ws:";
	const ws = new WebSocket(proto + "//" + location.host + "/v1/containers/" + encodeURIComponent(name) + "/console");

	ws.binaryType = "arraybuffer";
	const decoder = new TextDecoder();

	ws.onopen = () => {
		statusEl.textContent = "connected";
	};
	ws.onmessage = (event) => {
		const bytes = new Uint8Array(event.data);

		consoleTerminal.feed(decoder.decode(bytes, { stream: true }));
	};
	ws.onclose = () => {
		statusEl.textContent = "session ended";
	};
	ws.onerror = () => {
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
function drawChart(canvas, series, opts) {
	const ctx = canvas.getContext("2d");
	const w = canvas.width;
	const h = canvas.height;
	const style = getComputedStyle(document.body);
	const textColor = style.getPropertyValue("--muted").trim() || "#888";
	const gridColor = style.getPropertyValue("--border").trim() || "#ccc";
	const marginLeft = 46;
	const marginBottom = 16;
	const marginTop = 6;
	const marginRight = 6;
	const plotW = w - marginLeft - marginRight;
	const plotH = h - marginTop - marginBottom;

	ctx.clearRect(0, 0, w, h);

	let maxV = opts.maxY || 0;

	if (!maxV) {
		for (const s of series)
			for (const v of s.values)
				if (v > maxV) maxV = v;
	}
	if (maxV <= 0) maxV = 1;

	ctx.font = "10px -apple-system, BlinkMacSystemFont, sans-serif";
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
	drawChart(document.getElementById("cd-stats-cpu"), [{ values: cpuPercents, color: "#0a84ff" }], {
		times: rateTimes,
		maxY: 100,
		formatY: (v) => v.toFixed(0) + "%",
	});
	document.getElementById("cd-stats-cpu-label").textContent =
		cpuPercents.length > 0 ? cpuPercents[cpuPercents.length - 1].toFixed(1) + "% (1 core = 100%)" : "…";

	const memValues = h.map((s) => s.memCurrent);
	const memMax = h[h.length - 1].memMax;

	drawChart(document.getElementById("cd-stats-mem"), [{ values: memValues, color: "#30d158" }], {
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

	drawChart(document.getElementById("cd-stats-disk"), [{ values: diskValues, color: "#ff9f0a" }], {
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
			{ values: rxRates, color: "#0a84ff" },
			{ values: txRates, color: "#ff375f" },
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

	drawChart(document.getElementById("cd-stats-cpu-pressure"), [{ values: cpuPressureValues, color: "#ff9f0a" }], {
		times: gaugeTimes,
		maxY: 100,
		formatY: (v) => v.toFixed(0) + "%",
	});
	document.getElementById("cd-stats-cpu-pressure-label").textContent =
		cpuPressureValues[cpuPressureValues.length - 1].toFixed(1) + "% (some, avg10)";

	const memPressureValues = h.map((s) => s.memPressure);

	drawChart(document.getElementById("cd-stats-mem-pressure"), [{ values: memPressureValues, color: "#ff375f" }], {
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
		stats = await apiRequest("GET", "/v1/containers/" + encodeURIComponent(name) + "/stats");
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
	drawChart(document.getElementById("hs-stats-cpu"), [{ values: cpuPercents, color: "#0a84ff" }], {
		times: rateTimes,
		maxY: 100,
		formatY: (v) => v.toFixed(0) + "%",
	});
	document.getElementById("hs-stats-cpu-label").textContent =
		cpuPercents.length > 0 ? cpuPercents[cpuPercents.length - 1].toFixed(1) + "%" : "…";

	const memValues = h.map((s) => s.memUsed);
	const memTotal = h[h.length - 1].memTotal;

	drawChart(document.getElementById("hs-stats-mem"), [{ values: memValues, color: "#30d158" }], {
		times: gaugeTimes,
		maxY: memTotal || 0,
		formatY: formatBytes,
	});
	{
		const last = h[h.length - 1];

		document.getElementById("hs-stats-mem-label").textContent =
			formatBytes(last.memUsed) + " (total " + formatBytes(last.memTotal) + ")";
	}

	const diskValues = h.map((s) => s.diskUsed);
	const diskTotal = h[h.length - 1].diskTotal;

	drawChart(document.getElementById("hs-stats-disk"), [{ values: diskValues, color: "#ff9f0a" }], {
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
			{ values: rxRates, color: "#0a84ff" },
			{ values: txRates, color: "#ff375f" },
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

	drawChart(document.getElementById("hs-stats-cpu-pressure"), [{ values: cpuPressureValues, color: "#ff9f0a" }], {
		times: gaugeTimes,
		maxY: 100,
		formatY: (v) => v.toFixed(0) + "%",
	});
	document.getElementById("hs-stats-cpu-pressure-label").textContent =
		cpuPressureValues[cpuPressureValues.length - 1].toFixed(1) + "% (some, avg10)";

	const memPressureValues = h.map((s) => s.memPressure);

	drawChart(document.getElementById("hs-stats-mem-pressure"), [{ values: memPressureValues, color: "#ff375f" }], {
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
		stats = await apiRequest("GET", "/v1/system/stats");
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
async function refreshProcesses() {
	const tbody = document.getElementById("processes-body");

	tbody.innerHTML = '<tr><td colspan="7" class="empty">Loading&hellip;</td></tr>';
	try {
		const procs = await apiRequest("GET", "/v1/system/processes");

		renderProcessesTable(procs);
	} catch (e) {
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
		await apiRequest("DELETE", "/v1/system/processes/" + pid);
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

function consoleKeydown(event) {
	if (consoleWs === null || consoleWs.readyState !== WebSocket.OPEN)
		return;

	let data = null;

	if (event.ctrlKey && event.key.length === 1) {
		const code = event.key.toUpperCase().charCodeAt(0);

		if (code >= 65 && code <= 90)
			data = String.fromCharCode(code - 64);
	} else if (event.key.length === 1 && !event.metaKey && !event.altKey) {
		data = event.key;
	} else if (event.key === "Enter") {
		data = "\r";
	} else if (event.key === "Backspace") {
		data = "\x7f";
	} else if (event.key === "Tab") {
		data = "\t";
	} else if (event.key === "Escape") {
		data = "\x1b";
	} else if (event.key === "ArrowUp") {
		data = "\x1b[A";
	} else if (event.key === "ArrowDown") {
		data = "\x1b[B";
	} else if (event.key === "ArrowRight") {
		data = "\x1b[C";
	} else if (event.key === "ArrowLeft") {
		data = "\x1b[D";
	}

	if (data !== null) {
		event.preventDefault();
		consoleWs.send(new TextEncoder().encode(data));
	}
}

document.getElementById("cd-console-output").addEventListener("keydown", consoleKeydown);

for (const tabButton of document.querySelectorAll(".tab-bar .tab-button")) {
	tabButton.addEventListener("click", () => {
		const tabBar = tabButton.parentElement;
		const tabName = tabButton.dataset.tab;

		for (const btn of tabBar.querySelectorAll(".tab-button"))
			btn.classList.toggle("active", btn === tabButton);
		for (const panel of tabBar.parentElement.querySelectorAll(".tab-panel"))
			panel.hidden = panel.dataset.tab !== tabName;
		if (tabName === "console")
			document.getElementById("cd-console-output").focus();
		if (tabButton.id === "cd-tab-summary" && currentContainerDetailName !== null)
			startStatsPolling(currentContainerDetailName);
		else if (statsContainerName !== null)
			stopStatsPolling();
	});
}

function renderContainerDetail(name) {
	const c = cache.containers.find((x) => x.name === name);
	const title = document.getElementById("cd-title");
	const fields = document.getElementById("cd-fields");

	currentContainerDetailName = name;

	if (!c) {
		title.textContent = name + " (not found)";
		fields.textContent = "";
		closeConsole();
		stopStatsPolling();
		return;
	}

	openConsole(name);
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
	title.textContent = c.name;

	/* Summary -- identity/runtime status only. */
	fields.textContent = "";
	fields.appendChild(fieldBlock("Status", c.status));
	fields.appendChild(fieldBlock("Image", c.image));
	fields.appendChild(fieldBlock("PID", c.pid === null || c.pid === undefined ? "-" : String(c.pid)));
	fields.appendChild(
		fieldBlock("Exit status", c.exit_status === null || c.exit_status === undefined ? "-" : String(c.exit_status))
	);
	fields.appendChild(fieldBlock("Command", (c.cmd || []).join(" ") || "-"));

	/* Resource limits -- read live from the real cgroup by the daemon
	 * (ADR-0165), not just whatever was requested at creation. Until
	 * now these were write-only: settable via POST /containers but
	 * never shown back anywhere, so there was no way to tell what a
	 * running container (a pkgbuild sandbox included) actually had. */
	fields.appendChild(
		fieldBlock("Memory limit", c.memory_max === null || c.memory_max === undefined ? "unlimited" : formatBytes(c.memory_max))
	);
	fields.appendChild(fieldBlock("CPU limit", formatCpuMax(c.cpu_max) === "-" ? "unlimited" : formatCpuMax(c.cpu_max)));
	fields.appendChild(
		fieldBlock("Process limit", c.pids_max === null || c.pids_max === undefined ? "unlimited" : String(c.pids_max))
	);

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
	optionsFields.appendChild(
		fieldBlock(
			"Readiness",
			c.readiness ? "tcp:" + c.readiness.tcp_port + " (timeout " + c.readiness.timeout_seconds + "s)" : "-"
		)
	);
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
	const data = await apiRequest("GET", "/v1/networks");
	cache.networks = data.networks;
	renderNetworks(cache.networks);
}

async function removeNetwork(name) {
	try {
		await apiRequest("DELETE", "/v1/networks/" + encodeURIComponent(name));
		clearStatus();
		if (parseHash().category === "networks" && parseHash().name === name)
			location.hash = "#networks";
		await refreshNetworks();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove network " + name + ": " + e.message, true);
	}
}

function renderNetworkDetail(name) {
	const n = cache.networks.find((x) => x.name === name);
	const title = document.getElementById("nd-title");
	const fields = document.getElementById("nd-fields");

	if (!n) {
		title.textContent = name + " (not found)";
		fields.textContent = "";
		return;
	}

	title.textContent = n.name;
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
		await apiRequest("POST", "/v1/networks/" + encodeURIComponent(networkName) + "/interfaces", body);
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
		await apiRequest(
			"DELETE",
			"/v1/networks/" + encodeURIComponent(networkName) + "/interfaces/" + encodeURIComponent(ifname)
		);
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
	const data = await apiRequest("GET", "/v1/images");
	cache.images = data.images;
	renderImages(cache.images);
}

async function removeImage(name) {
	try {
		await apiRequest("DELETE", "/v1/images/" + encodeURIComponent(name));
		clearStatus();
		if (parseHash().category === "images" && parseHash().name === name)
			location.hash = "#images";
		await refreshImages();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove image " + name + ": " + e.message, true);
	}
}

function renderImageDetail(name) {
	document.getElementById("imgd-title").textContent = name;

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
	refreshImageRecipeApplyStatus();

	document.getElementById("imgd-remove").onclick = () => removeImage(name);
	document.getElementById("imgd-add-recipe").onclick = () => openModal("pkg-recipe-form", "Add or update a recipe");
}

/* ---- ADR-0123: image recipe tab (declarative package-list definition) ---- */

let imageRecipeContentCache = { name: null, content: null };

async function loadImageRecipeContent(name) {
	if (imageRecipeContentCache.name === name)
		return imageRecipeContentCache.content;
	const data = await apiRequest("GET", "/v1/images/recipes/" + encodeURIComponent(name));

	imageRecipeContentCache = { name: name, content: data.content };
	return imageRecipeContentCache.content;
}

function renderImageRecipeTab(name) {
	const missingEl = document.getElementById("imgd-recipe-missing");
	const presentEl = document.getElementById("imgd-recipe-present");
	const contentEl = document.getElementById("imgd-recipe-content");

	loadImageRecipeContent(name)
		.then((content) => {
			if (parseHash().category === "images" && parseHash().name === name) {
				missingEl.hidden = true;
				presentEl.hidden = false;
				contentEl.textContent = content;
			}
		})
		.catch(() => {
			if (parseHash().category === "images" && parseHash().name === name) {
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
			await apiRequest("DELETE", "/v1/images/recipes/" + encodeURIComponent(name));
			clearStatus();
			imageRecipeContentCache = { name: null, content: null };
			renderImageRecipeTab(name);
		} catch (e) {
			showStatus("Failed to remove image recipe for " + name + ": " + e.message, true);
		}
	};
	document.getElementById("imgd-apply-recipe").onclick = async () => {
		try {
			const r = await apiRequest("POST", "/v1/images/" + encodeURIComponent(name) + "/apply-recipe");

			clearStatus();
			showStatus(
				r && r.state === "running"
					? "Recipe apply started for " + name + " (async artifact fetch)"
					: "Recipe applied for " + name,
				false
			);
			await refreshImageRecipeApplyStatus();
			await refreshImageDetailVersioning(name);
		} catch (e) {
			showStatus("Failed to apply recipe for " + name + ": " + e.message, true);
		}
	};
}

async function refreshImageRecipeApplyStatus() {
	const box = document.getElementById("imgd-recipe-apply-status");
	const route = parseHash();

	if (route.category !== "images" || route.name === null)
		return;
	try {
		const status = await apiRequest("GET", "/v1/images/recipe-apply-status");

		box.textContent = "";
		const lines = [
			["State", status.state],
			["Image", status.image || "-"],
			["Last attempt", status.last_attempt || "(never)"],
			["Error", status.error || "-"],
		];
		for (const [label, value] of lines) {
			const p = document.createElement("p");

			p.textContent = label + ": " + value;
			box.appendChild(p);
		}
	} catch (e) {
		box.textContent = "Failed to load apply status: " + e.message;
	}
}

document.getElementById("image-recipe-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("irf-name").value.trim();
	const content = document.getElementById("irf-content").value;

	if (name === "" || content.trim() === "")
		return;

	try {
		await apiRequest("POST", "/v1/images/recipes", { name: name, content: content });
		clearStatus();
		document.getElementById("image-recipe-form").reset();
		closeModal();
		imageRecipeContentCache = { name: null, content: null };
		if (parseHash().category === "images" && parseHash().name === name)
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
		const data = await apiRequest("GET", "/v1/images/" + encodeURIComponent(name));

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
					await apiRequest(
						"DELETE",
						"/v1/images/" + encodeURIComponent(name) + "/manifest/" + encodeURIComponent(entry.package)
					);
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
			await apiRequest("POST", "/v1/images/" + encodeURIComponent(name) + "/manifest", {
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
		stateCell.textContent = pkg.state;
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
					await apiRequest("POST", "/v1/pkg/install", { name: r.name, image: name });
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
	const data = await apiRequest("GET", "/v1/devices");
	cache.devices = data.devices;
	if (parseHash().category === "devices")
		renderDevices();
	populateContainerFormDeviceLists();
}

async function refreshDeviceMaps() {
	const data = await apiRequest("GET", "/v1/devicemaps");
	cache.deviceMaps = data.devicemaps;
	if (parseHash().category === "devices")
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

	badge.className = "badge " + (d.assignable ? "badge-ok" : "badge-unknown");
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

		badge.className = "badge " + (m.present ? "badge-ok" : "badge-unknown");
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
		await apiRequest("DELETE", "/v1/devicemaps/" + encodeURIComponent(name));
		clearStatus();
		await refreshDeviceMaps();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove device mapping " + name + ": " + e.message, true);
	}
}

/* ---------- Disks (multi-disk management: ADR-0071/ADR-0102/ADR-0104) ---------- */

async function refreshDisks() {
	const data = await apiRequest("GET", "/v1/disks");

	cache.disks = data.disks;
	if (parseHash().category === "disks")
		renderDisks();
	/* Keeps the "+ Create > Disk Role" modal's own disk select current
	 * even when opened from the header dropdown rather than a specific
	 * disk row's own "Assign role…" shortcut (which sets a value
	 * afterward, same populate-then-select order populateContainerForm
	 * DeviceLists()/refreshDevices() already establishes). */
	populateDiskRoleSelect();
}

async function refreshDiskRoles() {
	const data = await apiRequest("GET", "/v1/diskroles");

	cache.diskRoles = data.diskroles;
	if (parseHash().category === "disks")
		renderDisks();
	populateDiskRoleSelect();
}

function diskRoleFor(diskName) {
	return cache.diskRoles.find((r) => r.disk_name === diskName) || null;
}

/*
 * Format status is per-disk (GET /disks/{name}/format), unlike every
 * other cached resource here which is one list call -- fetched only
 * while the Disks page is actually showing (same "only while this
 * page is open" guard refreshImageRecipeApplyStatus() already
 * established), and only for disks that could ever have a job at all
 * (role-assigned, non-OS disks) rather than every disk on the box, to
 * keep this bounded regardless of how many disks exist.
 */
async function refreshDiskFormatStatuses() {
	if (parseHash().category !== "disks")
		return;

	const candidates = cache.disks.filter((d) => !d.is_os_disk && diskRoleFor(d.name) !== null);

	for (const d of candidates) {
		try {
			cache.diskFormatStatus[d.name] = await apiRequest("GET", "/v1/disks/" + encodeURIComponent(d.name) + "/format");
		} catch (e) {
			/* Transient -- next poll tick tries again; the row just keeps
			 * showing whatever status it last had. */
		}
	}
	if (parseHash().category === "disks")
		renderDisks();
}

/* ---------- Storage placement: state (ADR-0141 Phase 2) + logs (Phase 3) ----------
 * Both kinds share an identical shape (one daemon-side storage_kind
 * each, its own independent migration job slot) -- a single
 * kind-parameterized set of functions here, matching cli/src/main.c's
 * own cmd_storage_kind() refactor, rather than duplicating this block
 * a second time for "logs". */

const STORAGE_KINDS = {
	state: { endpoint: "state-storage", cacheKey: "stateStorage", statusCacheKey: "stateStorageMigrate",
	         currentId: "ss-current", statusId: "ss-migrate-status", selectId: "ss-target-disk",
	         formId: "ss-migrate-form", label: "State storage", role: "state-storage" },
	logs: { endpoint: "log-storage", cacheKey: "logStorage", statusCacheKey: "logStorageMigrate",
	        currentId: "ls-current", statusId: "ls-migrate-status", selectId: "ls-target-disk",
	        formId: "ls-migrate-form", label: "Log storage", role: "log-storage" },
	rebuildable: { endpoint: "rebuildable-storage", cacheKey: "rebuildableStorage",
	               statusCacheKey: "rebuildableStorageMigrate", currentId: "rs-current",
	               statusId: "rs-migrate-status", selectId: "rs-target-disk", formId: "rs-migrate-form",
	               label: "Rebuildable storage", role: "rebuildable-storage" },
};

async function refreshStoragePlacement(kind) {
	const k = STORAGE_KINDS[kind];

	cache[k.cacheKey] = await apiRequest("GET", "/v1/system/" + k.endpoint);
	if (parseHash().category === "disks")
		renderStoragePlacement(kind);
}

async function refreshStoragePlacementMigrate(kind) {
	const k = STORAGE_KINDS[kind];

	if (parseHash().category !== "disks")
		return;
	try {
		cache[k.statusCacheKey] = await apiRequest("GET", "/v1/system/" + k.endpoint + "/migrate");
	} catch (e) {
		/* Transient -- next poll tick tries again. */
	}
	if (parseHash().category === "disks")
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
	for (const d of cache.disks) {
		const role = diskRoleFor(d.name);

		if (d.is_os_disk || role === null || role.role !== k.role)
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
	renderStoragePlacement("state");
	renderStoragePlacement("logs");
	renderStoragePlacement("rebuildable");
}

for (const kind of Object.keys(STORAGE_KINDS)) {
	const k = STORAGE_KINDS[kind];

	document.getElementById(k.formId).addEventListener("submit", async (event) => {
		event.preventDefault();

		const disk = document.getElementById(k.selectId).value;

		try {
			cache[k.statusCacheKey] = await apiRequest("POST", "/v1/system/" + k.endpoint + "/migrate", {
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
 * currently open rather than a fixed daemon-wide slot, since cache.disks/
 * diskRoleFor() are already kept fresh by the global poll loop
 * regardless of which view is active.
 */
async function refreshContainerStorageMigrate(name) {
	if (currentContainerDetailName !== name)
		return;
	try {
		cache.containerStorageMigrate = await apiRequest(
			"GET",
			"/v1/containers/" + encodeURIComponent(name) + "/migrate-storage"
		);
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
	for (const d of cache.disks) {
		const role = diskRoleFor(d.name);

		if (d.is_os_disk || role === null || role.role !== "container-storage")
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
		cache.containerStorageMigrate = await apiRequest(
			"POST",
			"/v1/containers/" + encodeURIComponent(name) + "/migrate-storage",
			{ disk: disk === "" ? null : disk }
		);
		clearStatus();
		renderContainerStorage(name);
	} catch (e) {
		showStatus("Failed to start storage migration for " + name + ": " + e.message, true);
	}
});

function renderDisks() {
	const body = document.getElementById("disks-body");

	body.textContent = "";
	if (cache.disks.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 10;
		cell.className = "empty";
		cell.textContent = "No disks found";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}
	for (const d of cache.disks)
		body.appendChild(diskRow(d));
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

		badge.className = "badge badge-unknown";
		badge.textContent = "OS disk";
		osCell.appendChild(badge);
	} else {
		osCell.textContent = "-";
	}
	row.appendChild(osCell);

	const mountedCell = document.createElement("td");
	const mountedBadge = document.createElement("span");

	mountedBadge.className = "badge " + (d.mounted ? "badge-ok" : "badge-unknown");
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

	const role = diskRoleFor(d.name);
	const roleCell = document.createElement("td");

	roleCell.textContent = role ? role.role : "-";
	row.appendChild(roleCell);

	const formatStatus = cache.diskFormatStatus[d.name];
	const formatCell = document.createElement("td");

	if (formatStatus && formatStatus.state !== "none") {
		const badge = document.createElement("span");

		badge.className =
			"badge " +
			(formatStatus.state === "ready" ? "badge-ok" : formatStatus.state === "failed" ? "badge-error" : "badge-paused");
		badge.textContent = formatStatus.state === "failed" ? "failed: " + formatStatus.error : formatStatus.state;
		formatCell.appendChild(badge);
	} else {
		formatCell.textContent = "-";
	}
	row.appendChild(formatCell);

	const actionCell = document.createElement("td");

	if (d.is_os_disk) {
		/* Never a role/format candidate -- nothing to offer. */
	} else if (!role) {
		const assignBtn = document.createElement("button");

		assignBtn.type = "button";
		assignBtn.textContent = "Assign role…";
		assignBtn.addEventListener("click", () => {
			populateDiskRoleSelect();
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

		const fsSelect = document.createElement("select");

		for (const fs of ["ext4", "btrfs"]) {
			const opt = document.createElement("option");

			opt.value = fs;
			opt.textContent = fs;
			fsSelect.appendChild(opt);
		}

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

function populateDiskRoleSelect() {
	const select = document.getElementById("drf-disk-name");

	select.textContent = "";
	for (const d of cache.disks) {
		if (d.is_os_disk || diskRoleFor(d.name) !== null)
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
		await apiRequest("POST", "/v1/diskroles", { disk_name: diskName, role: role });
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
		await apiRequest("DELETE", "/v1/diskroles/" + encodeURIComponent(diskName));
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
 * mirroring thincctl's own "the operator already specified which disk
 * by typing its name once" reasoning (cli/src/main.c's cmd_disks_
 * format()), not asked for a second time as a separate typed field. */
async function formatDisk(diskName, fsType) {
	if (!confirm("Format " + diskName + " as " + fsType + "? This destroys every byte of existing content on the disk. This cannot be undone."))
		return;
	try {
		cache.diskFormatStatus[diskName] = await apiRequest("POST", "/v1/disks/" + encodeURIComponent(diskName) + "/format", {
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
	dnsRecordEditName = rec.name;
	openModal("dns-record-form", "Edit DNS record");
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
	const data = await apiRequest("GET", "/v1/dns/records");
	cache.dnsRecords = data.records;
	renderDnsRecords(cache.dnsRecords);
}

async function removeDnsRecord(name) {
	try {
		await apiRequest("DELETE", "/v1/dns/records/" + encodeURIComponent(name));
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
	const data = await apiRequest("GET", "/v1/dns/servers");
	cache.dnsServers = data.servers;
	renderDnsServers(cache.dnsServers);
}

async function removeDnsServer(container) {
	try {
		await apiRequest("DELETE", "/v1/dns/servers/" + encodeURIComponent(container));
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
	const data = await apiRequest("GET", "/v1/ldap/servers");
	cache.ldapServers = data.servers;
	renderLdapServers(cache.ldapServers);
}

async function removeLdapServer(container) {
	try {
		await apiRequest("DELETE", "/v1/ldap/servers/" + encodeURIComponent(container));
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
	ldapGroupEditName = group.name;
	openModal("ldap-group-form", "Edit LDAP group");
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
	const data = await apiRequest("GET", "/v1/ldap/groups");
	cache.ldapGroups = data.groups;
	renderLdapGroups(cache.ldapGroups);
}

async function removeLdapGroup(name) {
	try {
		await apiRequest("DELETE", "/v1/ldap/groups/" + encodeURIComponent(name));
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
	ldapUserEditName = user.name;
	openModal("ldap-user-form", "Edit LDAP user");
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
	const data = await apiRequest("GET", "/v1/ldap/users");
	cache.ldapUsers = data.users;
	renderLdapUsers(cache.ldapUsers);
}

async function removeLdapUser(name) {
	try {
		await apiRequest("DELETE", "/v1/ldap/users/" + encodeURIComponent(name));
		clearStatus();
		await refreshLdapUsers();
	} catch (e) {
		showStatus("Failed to remove LDAP user " + name + ": " + e.message, true);
	}
}

/* ---------- LDAP Config: start_uid/start_gid auto-allocation floor (task #748) ---------- */

let ldapConfigDirty = false;

async function refreshLdapConfig() {
	try {
		const config = await apiRequest("GET", "/v1/ldap/config");

		cache.ldapConfig = config;
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
		await apiRequest("PUT", "/v1/ldap/config", body);
		clearStatus();
		showStatus("LDAP config saved", false);
		ldapConfigDirty = false;
		await refreshLdapConfig();
	} catch (e) {
		showStatus("Failed to save LDAP config: " + e.message, true);
	}
});

/* ---------- NTP (tasks #751-755) ---------- */

/* ---- NTP Config: upstream server address list ---- */

let ntpConfigDirty = false;

async function refreshNtpConfig() {
	try {
		const config = await apiRequest("GET", "/v1/system/ntp");

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
		await apiRequest("PUT", "/v1/system/ntp", { upstream: servers });
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
	const data = await apiRequest("GET", "/v1/ntp/servers");
	cache.ntpServers = data.servers;
	renderNtpServers(cache.ntpServers);
}

async function removeNtpServer(container) {
	try {
		await apiRequest("DELETE", "/v1/ntp/servers/" + encodeURIComponent(container));
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
		await apiRequest("POST", "/v1/ntp/servers", { container: container });
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
	const data = await apiRequest("GET", "/v1/syslog/targets");
	cache.syslogTargets = data.targets;
	renderSyslogTargets(cache.syslogTargets);
}

function renderSyslogTargetsList() {
	refreshSyslogTargets();
}

async function removeSyslogTarget(container) {
	try {
		await apiRequest("DELETE", "/v1/syslog/targets/" + encodeURIComponent(container));
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
		await apiRequest("POST", "/v1/syslog/targets", { container: container });
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
		const config = await apiRequest("GET", "/v1/system/tls-throttle");

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
		await apiRequest("PUT", "/v1/system/tls-throttle", {
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
	const data = await apiRequest("GET", "/v1/system/tls-throttle/status");

	cache.tlsThrottleStatus = data.entries;
	renderTlsThrottleStatus(cache.tlsThrottleStatus);
}

/* ---- NTP Status: most recent sync attempt outcome ---- */

async function refreshNtpStatus() {
	const box = document.getElementById("ntp-status-box");

	try {
		const status = await apiRequest("GET", "/v1/system/ntp/status");

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
		await apiRequest("POST", "/v1/system/ntp/sync");
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
		const t = await apiRequest("GET", "/v1/system/time");

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
		await apiRequest("PUT", "/v1/system/time", { unixtime: unixtime });
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
		const ca = await apiRequest("GET", "/v1/pki/ca");

		cache.pkiCa = ca;
		pkiCaStatus.className = "pki-ca-status bootstrapped";
		pkiCaStatus.textContent =
			"Bootstrapped: " + ca.subject + " (serial " + ca.serial + ", expires " + ca.not_after + ") ";
		pkiCaStatus.appendChild(buildCaDownloadButton("thinc-root-ca.crt", ca.cert_pem));
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
		const intermediate = await apiRequest("GET", "/v1/pki/intermediate");

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
		status.appendChild(buildCaDownloadButton("thinc-intermediate-ca.crt", intermediate.cert_pem));
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
	const data = await apiRequest("GET", "/v1/pki/certs");
	cache.pkiCerts = data.certs;
	renderPkiCerts(cache.pkiCerts);
}

async function removePkiCert(name) {
	try {
		await apiRequest("DELETE", "/v1/pki/certs/" + encodeURIComponent(name));
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
		stateCell.textContent = pkg.state;
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
 * a package with many published versions (thinc has 14) used to render
 * as that many separate, equally-weighted rows, and worse, the single
 * "Delete" button on any one of them called DELETE .../recipes/{name}
 * with no ?version= -- silently wiping *every* stored version, not just
 * the row clicked. Real data-loss footgun, not just a display nit.
 *
 * Collapsed to one row per name (the most recently published version,
 * by created_at -- simpler and equally correct here than reimplementing
 * pkg_version_compare()'s dpkg-style comparator in JS, since a real
 * recipe history is only ever appended to, never backdated) with a
 * ▸/▾ toggle when more than one version exists. Every row, collapsed
 * or expanded, now has its own real, version-scoped delete -- the
 * old implicit "delete removes everything" behavior is gone entirely,
 * not just hidden; deleting every version now means expanding and
 * deleting each one explicitly, which is the safe default.
 */
const expandedPkgRecipes = new Set();

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

	const addVersionRow = (r, isLatest, toggle) => {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		if (toggle) {
			toggle.className = "recipe-version-toggle";
			nameCell.appendChild(toggle);
		}
		nameCell.appendChild(treeLink("#packages/" + encodeURIComponent(r.name), r.name, ""));
		row.appendChild(nameCell);

		const versionCell = document.createElement("td");
		versionCell.textContent = r.version;
		if (!isLatest)
			row.className = "recipe-version-row";
		row.appendChild(versionCell);

		const dependsCell = document.createElement("td");
		dependsCell.textContent = r.depends || "-";
		row.appendChild(dependsCell);

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		rmButton.textContent = "Delete";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removePkgRecipe(r.name, r.version));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	};

	for (const name of names) {
		const versions = groups.get(name);
		const latest = versions[0];
		let toggle = null;

		if (versions.length > 1) {
			toggle = document.createElement("button");
			toggle.type = "button";
			toggle.textContent = (expandedPkgRecipes.has(name) ? "▾ " : "▸ ") + versions.length + " versions";
			toggle.addEventListener("click", () => {
				if (expandedPkgRecipes.has(name))
					expandedPkgRecipes.delete(name);
				else
					expandedPkgRecipes.add(name);
				renderRecipesList();
			});
		}
		addVersionRow(latest, true, toggle);
		if (versions.length > 1 && expandedPkgRecipes.has(name)) {
			for (const older of versions.slice(1))
				addVersionRow(older, false, null);
		}
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
let pkgRecipeContentCache = { name: null, content: null };

async function loadPkgRecipeContent(name) {
	if (pkgRecipeContentCache.name === name)
		return pkgRecipeContentCache.content;
	const data = await apiRequest("GET", "/v1/pkg/recipes/" + encodeURIComponent(name));

	pkgRecipeContentCache = { name: name, content: data.content };
	return pkgRecipeContentCache.content;
}

function renderPackageDetail(name) {
	document.getElementById("pkgd-title").textContent = name;

	/* issue #19: .find() alone returns whatever GET /pkg/recipes
	 * happened to list first for this name -- not necessarily the
	 * latest version (confirmed live: squashfs-tools' 5 stored
	 * versions come back in on-disk readdir() order, not sorted).
	 * Pick the most recently published one explicitly, same "newest
	 * created_at wins" rule the collapsed Recipes list now uses. */
	const allVersions = cache.pkgRecipes.filter((r) => r.name === name);
	const recipe =
		allVersions.length === 0
			? undefined
			: allVersions.reduce((a, b) => (b.created_at > a.created_at ? b : a));
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
				recipe.version + (allVersions.length > 1 ? " (latest of " + allVersions.length + " -- see Software > Recipes to browse/delete a specific older version)" : "")
			)
		);
		fields.appendChild(fieldBlock("Depends", recipe.depends || "-"));

		const contentEl = document.getElementById("pkgd-recipe-content");

		if (pkgRecipeContentCache.name === name) {
			contentEl.textContent = pkgRecipeContentCache.content;
		} else {
			contentEl.textContent = "Loading…";
			loadPkgRecipeContent(name)
				.then((content) => {
					if (parseHash().category === "packages" && parseHash().name === name)
						document.getElementById("pkgd-recipe-content").textContent = content;
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

	renderPackageDetailInstalled(name);
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
		stateCell.textContent = pkg.state;
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
	const data = await apiRequest("GET", "/v1/pkg/recipes");
	cache.pkgRecipes = data.recipes;
	if (parseHash().category === "packages")
		renderPackagesView(parseHash().name);
}

/* version is always required from every call site now (issue #19) --
 * the old version-less call silently deleted every stored version of
 * name, a real data-loss footgun once the recipe list started showing
 * one row per version. */
async function removePkgRecipe(name, version) {
	try {
		await apiRequest(
			"DELETE",
			"/v1/pkg/recipes/" + encodeURIComponent(name) + "?version=" + encodeURIComponent(version)
		);
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
	const data = await apiRequest("GET", "/v1/images/recipes");

	cache.imageRecipes = data.recipes;
	if (parseHash().category === "recipes")
		renderImageRecipesTable();
}

async function refreshContainerRecipesList() {
	const data = await apiRequest("GET", "/v1/containers/recipes");

	cache.containerRecipes = data.recipes;
	if (parseHash().category === "recipes")
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
				const result = await apiRequest("POST", "/v1/images/" + encodeURIComponent(r.name) + "/apply-recipe");

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
				await apiRequest("DELETE", "/v1/images/recipes/" + encodeURIComponent(r.name));
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
	const data = await apiRequest("GET", "/v1/containers/recipes/" + encodeURIComponent(name));

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
				await apiRequest("DELETE", "/v1/containers/recipes/" + encodeURIComponent(r.name));
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

		await apiRequest("POST", "/v1/containers/recipes", { name: name, content: content });
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
		await apiRequest("POST", "/v1/containers/recipes/" + encodeURIComponent(name) + "/apply", { secrets: secrets });
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

async function refreshPkgList() {
	const data = await apiRequest("GET", "/v1/pkg");
	cache.pkgList = data.packages;
	if (parseHash().category === "packages")
		renderPackagesView(parseHash().name);
}

async function removePkg(name, image) {
	const key = image && image !== "base" ? name + "@" + image : name;

	try {
		await apiRequest("DELETE", "/v1/pkg/" + encodeURIComponent(key));
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
		const config = await apiRequest("GET", "/v1/pkg/repo-config");

		cache.pkgRepoConfig = config;
		if (!pkgRepoConfigDirty) {
			document.getElementById("prc-url").value = config.repo_url || "";
			document.getElementById("prc-kind").value = config.repo_kind || "gitea";
			document.getElementById("prc-ref").value = config.ref || "";
			document.getElementById("prc-token").value = "";
			document.getElementById("prc-token").placeholder =
				config.auth_token_set ? "(unchanged, a token is set)" : "(unchanged, no token set)";
			document.getElementById("prc-interval").value = config.sync_interval_seconds;
		}
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

for (const id of ["prc-url", "prc-kind", "prc-ref", "prc-token", "prc-clear-token", "prc-interval"]) {
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
		sync_interval_seconds: parseInt(document.getElementById("prc-interval").value, 10),
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
		await apiRequest("PUT", "/v1/pkg/repo-config", body);
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
		const status = await apiRequest("GET", "/v1/pkg/sync");

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
		await apiRequest("POST", "/v1/pkg/sync");
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
		const config = await apiRequest("GET", "/v1/pkg/cache-config");

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
		await apiRequest("PUT", "/v1/pkg/cache-config", {
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
		const status = await apiRequest("GET", "/v1/pkg/cache");

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
		await apiRequest("DELETE", "/v1/pkg/cache");
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
		const data = await apiRequest("GET", "/v1/system/hostauth/sessions");

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
				await apiRequest("DELETE", "/v1/system/hostauth/sessions/" + encodeURIComponent(s.username));
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
		const config = await apiRequest("GET", "/v1/system/rolling-config");

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
		await apiRequest("PUT", "/v1/system/rolling-config", {
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
		const config = await apiRequest("GET", "/v1/system/pkg-build-config");

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

		await apiRequest("PUT", "/v1/system/pkg-build-config", {
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
		const config = await apiRequest("GET", "/v1/pkg/artifact-config");

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
		await apiRequest("PUT", "/v1/pkg/artifact-config", body);
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
	const cmdText = document.getElementById("f-cmd").value.trim();
	const memoryMaxText = document.getElementById("f-memory-max").value.trim();
	const pidsMaxText = document.getElementById("f-pids-max").value.trim();
	const cpuMaxText = document.getElementById("f-cpu-max").value.trim();
	const cpusetText = document.getElementById("f-cpuset").value.trim();
	const diskQuotaText = document.getElementById("f-disk-quota").value.trim();
	const network = document.getElementById("f-network").value.trim();
	const ipForward = document.getElementById("f-ip-forward").checked;
	const dnsRegister = document.getElementById("f-dns-register").checked;
	const routesText = document.getElementById("f-routes").value.trim();
	const dnsServersText = document.getElementById("f-dns-servers").value.trim();
	const devices = Array.from(document.getElementById("f-devices").selectedOptions).map((o) => o.value);
	const interfaces = Array.from(document.getElementById("f-interfaces").selectedOptions).map((o) => o.value.replace(/^net:/, ""));
	const restart = document.getElementById("f-restart").value;
	const restartDelayText = document.getElementById("f-restart-delay").value.trim();
	const followRolling = document.getElementById("f-follow-rolling").checked;
	const followRollingJitterText = document.getElementById("f-follow-rolling-jitter").value.trim();
	const dependsOnText = document.getElementById("f-depends-on").value.trim();
	const readinessPortText = document.getElementById("f-readiness-port").value.trim();
	const readinessTimeoutText = document.getElementById("f-readiness-timeout").value.trim();
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

	const body = {
		name: name,
		image: image,
		cmd: cmdText.split(/\s+/).filter((s) => s.length > 0),
	};
	if (memoryMaxText !== "")
		body.memory_max = parseInt(memoryMaxText, 10);
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
	if (readinessPortText !== "") {
		body.readiness = { tcp_port: parseInt(readinessPortText, 10) };
		if (readinessTimeoutText !== "")
			body.readiness.timeout_seconds = parseInt(readinessTimeoutText, 10);
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

		await apiRequest("POST", "/v1/containers", body);
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
		await apiRequest("POST", "/v1/networks", body);
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
		await apiRequest("POST", "/v1/devicemaps", body);
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
		await apiRequest("POST", "/v1/system/routes", body);
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
		await apiRequest("POST", "/v1/images", { name: name });
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
			await apiRequest("PUT", "/v1/dns/records/" + encodeURIComponent(dnsRecordEditName), { ip: ip });
		else
			await apiRequest("POST", "/v1/dns/records", { name: name, ip: ip });
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
		await apiRequest("POST", "/v1/dns/servers", { container: container, hosts_path: hostsPath });
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
		await apiRequest("POST", "/v1/ldap/servers", { container: container, config_path: configPath });
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
			await apiRequest("PUT", "/v1/ldap/groups/" + encodeURIComponent(ldapGroupEditName), {
				gidnumber: parseInt(gidnumberRaw, 10),
			});
		} else {
			const body = { name: name };
			if (gidnumberRaw !== "")
				body.gidnumber = parseInt(gidnumberRaw, 10);
			await apiRequest("POST", "/v1/ldap/groups", body);
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

	/* Comma-separated GID numbers, matching thincctl's own
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
			await apiRequest("PUT", "/v1/ldap/users/" + encodeURIComponent(ldapUserEditName), body);
		else
			await apiRequest("POST", "/v1/ldap/users", body);
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
		await apiRequest("POST", "/v1/pki/ca", body);
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
		await apiRequest("POST", "/v1/pki/intermediate", body);
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
		const result = await apiRequest("POST", "/v1/pki/reset", body);
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
		const issued = await apiRequest("POST", "/v1/pki/certs", body);
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

		await apiRequest("POST", "/v1/pkg/recipes", { name: name, content: content });
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
		await apiRequest("POST", "/v1/pkg/bootstrap", body);
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
		await apiRequest("POST", "/v1/pkg/install", body);
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
		const result = await apiRequest("POST", "/v1/pkg/update-all");
		clearStatus();
		showStatus(result && result.status ? result.status : "update started", false);
		await refreshPkgList();
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
		const site = await apiRequest("GET", "/v1/system/site");

		cache.siteConfig = site;
		if (!siteConfigDirty) {
			document.getElementById("sitef-instance-name").value = site.instance_name;
			document.getElementById("sitef-site-name").value = site.site_name;
			document.getElementById("sitef-domain-suffix").value = site.domain_suffix;
		}
		document.getElementById("df-name").placeholder = suggestedFqdn("db") || "db.internal";
		document.getElementById("pf-name").placeholder = suggestedFqdn("svc") || "svc.internal";

		const label = document.getElementById("tree-instance-label");

		label.textContent = site.instance_name;
		label.hidden = false;
		document.title = "thinC — " + site.instance_name;
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
		await apiRequest("PUT", "/v1/system/site", body);
		clearStatus();
		showStatus("Site config saved", false);
		siteConfigDirty = false;
		await refreshSiteConfig();
	} catch (e) {
		showStatus("Failed to save site config: " + e.message, true);
	}
});

let daemonConfigDirty = false;

/* Rebuilds the management-network <select> from cache.networks -- only
 * has_address networks are valid repoint targets (network_set_management()
 * refuses otherwise, see daemon/src/network.c), but the currently-active
 * one is always included even if that were somehow false, so the form
 * never silently shows a value that isn't actually selected. */
function populateManagementNetworkSelect(currentName) {
	const select = document.getElementById("dcf-management-network");
	const names = cache.networks
		.filter((n) => n.has_address || n.name === currentName)
		.map((n) => n.name);

	if (currentName && !names.includes(currentName))
		names.push(currentName);

	select.textContent = "";
	for (const name of names) {
		const option = document.createElement("option");

		option.value = name;
		option.textContent = name;
		select.appendChild(option);
	}
	select.value = currentName || "";
}

async function refreshRoutes() {
	try {
		const r = await apiRequest("GET", "/v1/system/routes");

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
		await apiRequest("DELETE", "/v1/system/routes", body);
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
		const r = await apiRequest("GET", "/v1/system/sysctl");

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
		await apiRequest("DELETE", "/v1/system/sysctl/" + encodeURIComponent(key));
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
 * thincctl's repeatable --option=KEY=VALUE. Empty input -> {} (no
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
		await apiRequest("PUT", "/v1/system/sysctl/" + encodeURIComponent(key), body);
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
		const r = await apiRequest("GET", "/v1/system/kmod");

		cache.kmodModules = r.modules;
	} catch (e) {
		/* Best-effort -- cache.kmodModules just stays at whatever was last shown. */
	}
}

async function refreshKmodConfig() {
	try {
		const r = await apiRequest("GET", "/v1/system/kmod-config");

		cache.kmodConfig = r.kmod_config;
	} catch (e) {
		/* Best-effort -- cache.kmodConfig just stays at whatever was last shown. */
	}
}

async function unloadKmod(name) {
	try {
		await apiRequest("DELETE", "/v1/system/kmod/" + encodeURIComponent(name));
		clearStatus();
		await refreshKmod();
		renderCurrentView();
	} catch (e) {
		showStatus("Failed to unload " + name + ": " + e.message, true);
	}
}

async function removeKmodConfig(name) {
	try {
		await apiRequest("DELETE", "/v1/system/kmod-config/" + encodeURIComponent(name));
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

		await apiRequest("POST", "/v1/system/kmod/" + encodeURIComponent(name), { options: options });
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

		await apiRequest("PUT", "/v1/system/kmod-config/" + encodeURIComponent(name), body);
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
		await apiRequest("POST", "/v1/system/kmod-build", body);
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
		const cfg = await apiRequest("GET", "/v1/system/logs/config");

		document.getElementById("lcf-max-bytes").value = cfg.max_bytes;
	} catch (e) {
		/* Best-effort -- the field just stays at whatever was last shown. */
	}
}

function renderLogsList() {
	refreshLogsConfig();
}

document.getElementById("logs-config-form").addEventListener("submit", async (event) => {
	event.preventDefault();
	try {
		await apiRequest("PUT", "/v1/system/logs/config", {
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
		const dc = await apiRequest("GET", "/v1/system/daemon-config");

		cache.daemonConfig = dc;
		populateManagementNetworkSelect(dc.management_network);
		if (!daemonConfigDirty) {
			document.getElementById("dcf-port").value = dc.port;
			document.getElementById("dcf-https-port").value = dc.https_port;
			document.getElementById("dcf-http-enabled").checked = dc.http_enabled;
			document.getElementById("dcf-https-enabled").checked = dc.https_enabled;
			document.getElementById("dcf-bind-ip").value = dc.bind_ip || "";
		}
		document.getElementById("dcf-bind-hint").textContent = dc.bind_ip
			? "Currently bound to " + dc.bind + " (dedicated bind_ip -- not the management network's own address)."
			: "Currently bound to " + dc.bind + " (the management network's own address).";
	} catch (e) {
		/* Best-effort -- the form just stays at whatever was last shown. */
	}
}

for (const id of ["dcf-port", "dcf-https-port", "dcf-http-enabled", "dcf-https-enabled",
                   "dcf-management-network", "dcf-bind-ip", "dcf-clear-bind-ip"]) {
	document.getElementById(id).addEventListener("input", () => {
		daemonConfigDirty = true;
	});
}

document.getElementById("sys-daemon-config-form").addEventListener("submit", async (event) => {
	event.preventDefault();

	const bindIpValue = document.getElementById("dcf-bind-ip").value.trim();
	const clearBindIp = document.getElementById("dcf-clear-bind-ip").checked;

	const body = {
		port: parseInt(document.getElementById("dcf-port").value, 10),
		https_port: parseInt(document.getElementById("dcf-https-port").value, 10),
		http_enabled: document.getElementById("dcf-http-enabled").checked,
		https_enabled: document.getElementById("dcf-https-enabled").checked,
		management_network: document.getElementById("dcf-management-network").value,
	};

	if (clearBindIp)
		body.bind_ip = null;
	else if (bindIpValue)
		body.bind_ip = bindIpValue;

	/* This dashboard's own fetch() calls are relative to the page's own
	 * origin (host:port it was loaded from) -- changing the plain-HTTP
	 * port, repointing the management network to a different address,
	 * or setting/clearing bind_ip (ADR-0068), disconnects this exact
	 * page the moment it takes effect. Confirmed explicitly here, same
	 * as the reboot/shutdown buttons' own confirm() guard, since
	 * there's no way back short of navigating to the new address by
	 * hand. */
	const cur = cache.daemonConfig;
	const reconnectNeeded = cur &&
		(body.port !== cur.port || body.management_network !== cur.management_network ||
		 "bind_ip" in body);

	if (reconnectNeeded &&
	    !confirm("This will change the address/port this dashboard is served on -- " +
	             "this page will lose its connection once it takes effect. You'll need to " +
	             "reload at the new address. Continue?"))
		return;

	try {
		await apiRequest("PUT", "/v1/system/daemon-config", body);
		clearStatus();
		showStatus("Daemon config saved", false);
		daemonConfigDirty = false;
		document.getElementById("dcf-clear-bind-ip").checked = false;
		await refreshDaemonConfig();
	} catch (e) {
		showStatus("Failed to save daemon config: " + e.message, true);
	}
});

async function refreshSwap() {
	try {
		const s = await apiRequest("GET", "/v1/system/swap");

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
		await apiRequest("POST", "/v1/system/swap", { size_mb: sizeMb });
		clearStatus();
		showStatus("Swap enabled", false);
		await refreshSwap();
	} catch (e) {
		showStatus("Failed to enable swap: " + e.message, true);
	}
});

document.getElementById("swap-disable").addEventListener("click", async () => {
	try {
		await apiRequest("DELETE", "/v1/system/swap");
		clearStatus();
		showStatus("Swap disabled", false);
		await refreshSwap();
	} catch (e) {
		showStatus("Failed to disable swap: " + e.message, true);
	}
});

document.getElementById("sys-backup").addEventListener("click", async () => {
	try {
		const text = await apiRequestRaw("GET", "/v1/system/backup");
		const blob = new Blob([text], { type: "application/json" });
		const url = URL.createObjectURL(blob);
		const a = document.createElement("a");

		a.href = url;
		a.download = "thinc-backup.json";
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

		await apiRequestRaw("POST", "/v1/system/restore", raw);
		clearStatus();
		showStatus("Restored. Reboot for it to take effect.", false);
		document.getElementById("sys-restore-form").reset();
	} catch (e) {
		showStatus("Failed to restore: " + e.message, true);
	}
});

/* ---------- Automatic backup snapshots (ADR-0141 Phase 5) ---------- */

async function refreshBackupConfig() {
	cache.backupConfig = await apiRequest("GET", "/v1/system/backup-config");
	if (parseHash().category === "backup")
		renderBackupConfig();
}

async function refreshBackupStatus() {
	if (parseHash().category !== "backup")
		return;
	try {
		cache.backupStatus = await apiRequest("GET", "/v1/system/backup-config/status");
	} catch (e) {
		/* Transient -- next poll tick tries again. */
	}
	if (parseHash().category === "backup")
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
	for (const d of cache.disks) {
		const role = diskRoleFor(d.name);

		if (d.is_os_disk || role === null || role.role !== "backup")
			continue;
		const opt = document.createElement("option");

		opt.value = d.name;
		opt.textContent = d.name + (d.model ? " (" + d.model + ")" : "");
		select.appendChild(opt);
	}
	select.value = cache.backupConfig.disk || prevValue;

	document.getElementById("bc-enabled").checked = !!cache.backupConfig.enabled;
	document.getElementById("bc-interval").value = cache.backupConfig.interval_hours || 0;

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
	const intervalHours = parseInt(document.getElementById("bc-interval").value, 10) || 0;

	try {
		cache.backupConfig = await apiRequest("PUT", "/v1/system/backup-config", {
			disk: disk === "" ? null : disk,
			enabled: enabled,
			interval_hours: intervalHours,
		});
		clearStatus();
		renderBackupConfig();
	} catch (e) {
		showStatus("Failed to save backup config: " + e.message, true);
	}
});

document.getElementById("bc-snapshot-now").addEventListener("click", async () => {
	try {
		cache.backupStatus = await apiRequest("POST", "/v1/system/backup-config/snapshot-now");
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
		const result = await apiRequest("POST", "/v1/system/update", body);
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
		await apiRequest("POST", "/v1/system/reboot");
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
		await apiRequest("POST", "/v1/system/shutdown");
		clearStatus();
		showStatus("Shutting down.", false);
	} catch (e) {
		showStatus("Failed to shut down: " + e.message, true);
	}
});

/* ---------- poll loop ---------- */

async function poll() {
	await refreshHealth();
	try {
		await refreshContainers();
		await refreshNetworks();
		await refreshImages();
		await refreshDevices();
		await refreshDeviceMaps();
		await refreshDisks();
		await refreshDiskRoles();
		await refreshDiskFormatStatuses();
		await refreshStoragePlacement("state");
		await refreshStoragePlacementMigrate("state");
		await refreshStoragePlacement("logs");
		await refreshStoragePlacementMigrate("logs");
		await refreshStoragePlacement("rebuildable");
		await refreshStoragePlacementMigrate("rebuildable");
		await refreshBackupConfig();
		await refreshBackupStatus();
		await refreshDnsRecords();
		await refreshDnsServers();
		await refreshLdapServers();
		await refreshLdapGroups();
		await refreshLdapUsers();
		await refreshLdapConfig();
		await refreshNtpConfig();
		await refreshNtpServers();
		await refreshSyslogTargets();
		await refreshNtpStatus();
		await refreshNtpTime();
		await refreshPkiCa();
		await refreshPkiIntermediate();
		await refreshPkiCerts();
		await refreshPkgRecipes();
		await refreshImageRecipesList();
		await refreshContainerRecipesList();
		await refreshPkgList();
		await refreshPkgRepoConfig();
		await refreshPkgSyncStatus();
		await refreshPkgCacheConfig();
		await refreshPkgCacheStatus();
		await refreshPkgArtifactConfig();
		await refreshImageRecipeApplyStatus();
		await refreshSiteConfig();
		await refreshDaemonConfig();
		await refreshRollingConfig();
		await refreshPkgBuildConfig();
		await refreshRoutes();
		await refreshSysctl();
		await refreshKmod();
		await refreshKmodConfig();
		await refreshSwap();
		await refreshTlsThrottleConfig();
		await refreshTlsThrottleStatus();
		await pollServerLogs();
	} catch (e) {
		showStatus("Poll failed: " + e.message, true);
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

function addContextMenuItem(label, danger, action) {
	const li = document.createElement("li");
	const button = document.createElement("button");

	button.type = "button";
	button.textContent = label;
	if (danger)
		button.className = "button-danger";
	button.addEventListener("click", () => {
		hideContextMenu();
		action();
	});
	li.appendChild(button);
	contextMenu.appendChild(li);
}

/* Maps a tree link's own href to the (category, name, label) a
 * right-click menu needs -- only containers/networks/images have
 * individual actionable leaves today; everything else (categories,
 * DNS/PKI/Packages/System/Devices) gets no custom menu, same as the
 * tree's own per-item-detail-view boundary already established. */
function contextMenuItemsFor(category, name) {
	if (category === "containers") {
		const c = cache.containers.find((x) => x.name === name);
		const status = c ? c.status : "running";
		const items = [
			{ label: "Open console", danger: false, action: () => { location.hash = "#containers/" + encodeURIComponent(name); } },
		];

		if (status === "stopped") items.push({ label: "Start", danger: false, action: () => startContainer(name) });
		if (status === "running") items.push({ label: "Pause", danger: false, action: () => pauseContainer(name) });
		if (status === "paused") items.push({ label: "Unpause", danger: false, action: () => unpauseContainer(name) });
		if (status !== "stopped") items.push({ label: "Stop", danger: false, action: () => stopContainer(name) });
		items.push({ label: "Remove", danger: true, action: () => removeContainer(name) });
		return items;
	}
	if (category === "networks")
		return [{ label: "Remove", danger: true, action: () => removeNetwork(name) }];
	if (category === "images")
		return [{ label: "Remove", danger: true, action: () => removeImage(name) }];
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
		addContextMenuItem(item.label, item.danger, item.action);

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
setInterval(poll, POLL_INTERVAL_MS);
