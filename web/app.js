/*
 * Kanxeo dashboard: a pure client of docs/api/openapi.yaml, same as
 * kanxeoctl (cli/src/main.c) -- no capability here that isn't already
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
	dnsRecords: [],
	dnsServers: [],
	ldapServers: [],
	ldapGroups: [],
	ldapUsers: [],
	ldapConfig: null,
	ntpConfig: null,
	ntpServers: [],
	ntpStatus: null,
	ntpTime: null,
	pkiCa: null,
	pkiIntermediate: null,
	pkiCerts: [],
	pkgRecipes: [],
	pkgList: [],
	siteConfig: null,
	daemonConfig: null,
	swap: null,
	logs: [],
};

const healthBadge = document.getElementById("health");
const statusBox = document.getElementById("status");
const treeEl = document.getElementById("tree");
const logOutput = document.getElementById("log-output");
const logPanel = document.getElementById("log-panel");
const logPanelHeader = document.getElementById("log-panel-header");
const logPanelArrow = document.getElementById("log-panel-arrow");

const LOG_COLLAPSE_KEY = "kanxeo-log-collapsed";

function setLogCollapsed(collapsed) {
	logPanel.classList.toggle("collapsed", collapsed);
	logPanelArrow.textContent = collapsed ? "▸" : "▾";
	try {
		localStorage.setItem(LOG_COLLAPSE_KEY, collapsed ? "1" : "0");
	} catch (e) {
		/* localStorage unavailable -- state just won't survive a reload. */
	}
}

logPanelHeader.addEventListener("click", () => {
	setLogCollapsed(!logPanel.classList.contains("collapsed"));
});

try {
	setLogCollapsed(localStorage.getItem(LOG_COLLAPSE_KEY) === "1");
} catch (e) {
	/* localStorage unavailable -- default expanded. */
}

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

const createDropdownToggle = document.getElementById("create-dropdown-toggle");
const createDropdownMenu = document.getElementById("create-dropdown-menu");

createDropdownToggle.addEventListener("click", (event) => {
	event.stopPropagation();
	createDropdownMenu.hidden = !createDropdownMenu.hidden;
});
document.addEventListener("click", () => {
	createDropdownMenu.hidden = true;
});
for (const item of createDropdownMenu.querySelectorAll("button[data-modal]")) {
	item.addEventListener("click", () => {
		createDropdownMenu.hidden = true;
		openModal(item.dataset.modal, item.dataset.title);
	});
}

/* Action log (bottom-right panel) -- every mutating request this
 * dashboard makes, newest at the bottom, like Proxmox's own task log.
 * Routine poll GETs are deliberately not logged here (every 2s x 10
 * endpoints would drown out anything a human actually did) -- see
 * apiRequest()'s own method check. */
const MAX_LOG_ENTRIES = 300;
let logCount = 0;

function logLine(method, path, statusText, kind) {
	const time = new Date().toTimeString().slice(0, 8);
	const entry = document.createElement("div");

	entry.className = "log-entry log-entry-" + kind;

	const timeSpan = document.createElement("span");

	timeSpan.className = "log-entry-time";
	timeSpan.textContent = time + "  ";
	entry.appendChild(timeSpan);
	entry.appendChild(document.createTextNode(method + " " + path + " "));

	const statusSpan = document.createElement("span");

	statusSpan.className = "log-entry-status";
	statusSpan.textContent = statusText;
	entry.appendChild(statusSpan);

	logOutput.appendChild(entry);
	logCount++;
	while (logCount > MAX_LOG_ENTRIES) {
		logOutput.removeChild(logOutput.firstChild);
		logCount--;
	}
	logOutput.scrollTop = logOutput.scrollHeight;
}

function showStatus(message, isError) {
	statusBox.textContent = message;
	statusBox.hidden = false;
	statusBox.className = "status " + (isError ? "status-error" : "status-ok");
}

function clearStatus() {
	statusBox.hidden = true;
}

async function apiRequest(method, path, body) {
	const opts = { method: method, headers: {} };
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
		throw new Error(message);
	}
	if (method !== "GET")
		logLine(method, path, "-> " + res.status, "ok");
	return json;
}

/* Raw variant for /v1/system/backup|restore -- the response/request
 * body IS the bundle, written/read byte-for-byte, the same "exact
 * round trip, not re-serialized" guarantee kanxeoctl's own backup/
 * restore commands already have (cli/src/main.c). */
async function apiRequestRaw(method, path, rawBody) {
	const opts = { method: method };
	if (rawBody !== undefined) {
		opts.headers = { "Content-Type": "application/json" };
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

async function refreshHealth() {
	try {
		await apiRequest("GET", "/v1/health");
		healthBadge.textContent = "daemon reachable";
		healthBadge.className = "badge badge-ok";
	} catch (e) {
		healthBadge.textContent = "daemon unreachable";
		healthBadge.className = "badge badge-error";
	}
}

/* ---------- routing ---------- */

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
	"dns-records": "view-dns-records",
	"dns-servers": "view-dns-servers",
	"ldap-servers": "view-ldap-servers",
	"ldap-groups": "view-ldap-groups",
	"ldap-users": "view-ldap-users",
	"ldap-config": "view-ldap-config",
	"ntp-config": "view-ntp-config",
	"ntp-servers": "view-ntp-servers",
	"ntp-status": "view-ntp-status",
	"ntp-time": "view-ntp-time",
	"pki-ca": "view-pki-ca",
	"pki-certs": "view-pki-certs",
	packages: "view-packages",
	recipes: "view-recipes",
	site: "view-site",
	"daemon-config": "view-daemon-config",
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
		if (route.category === "networks" && route.name !== null)
			renderNetworkDetail(route.name);
		else if (route.category === "routes")
			renderRoutesList();
		else if (route.category === "logs")
			renderLogsList();
		else if (route.category === "images" && route.name !== null)
			renderImageDetail(route.name);
		else if (route.category === "devices")
			renderDevices();
		else if (route.category === "packages")
			renderPackagesView(route.name);
		else if (route.category === "recipes")
			renderRecipesList();
	}

	renderTreeActive();
}

window.addEventListener("hashchange", () => {
	renderCurrentView();
	ensureActiveCategoryExpanded();
});

/* ---------- tree ---------- */

const TREE_COLLAPSE_KEY = "kanxeo-tree-collapsed";

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
			label: "Software",
			hash: "images",
			icon: "images",
			children: [
				{ label: "Images", hash: "images", icon: "images" },
				{ label: "Packages", hash: "packages", icon: "packages" },
				{ label: "Recipes", hash: "recipes", icon: "recipes" },
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
						{ label: "Site", hash: "site", icon: "dns" },
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
						{ label: "Status", hash: "ntp-status", icon: "dns" },
						{ label: "Time", hash: "ntp-time", icon: "dns" },
					],
				},
				{
					/* Aliases to its own first child's hash ("daemon-config"),
					 * same convention as System's own alias above. */
					label: "Server",
					hash: "daemon-config",
					icon: "system",
					children: [
						{ label: "Daemon", hash: "daemon-config", icon: "system" },
						{ label: "Devices", hash: "devices", icon: "devices" },
						{ label: "Routes", hash: "routes", icon: "networks" },
						{ label: "Logs", hash: "logs", icon: "system" },
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
 * not an oversight -- `kanxeoctl console` has no such limitation.
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
		if (tabButton.id === "cd-tab-stats" && currentContainerDetailName !== null)
			startStatsPolling(currentContainerDetailName);
		else if (statsContainerName !== null)
			stopStatsPolling();
	});
}

document.getElementById("cd-backup-goto-system").addEventListener("click", () => {
	location.hash = "#backup";
});

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
	 * while the Stats tab stays the active one left the poll loop
	 * silently stuck on the previously-viewed container's own name
	 * (startStatsPolling()/stopStatsPolling() otherwise only ever fire
	 * from an explicit tab click) -- the graphs looked "stuck" because
	 * they genuinely were still polling someone else's stats. */
	if (document.getElementById("cd-tab-stats").classList.contains("active"))
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
	optionsFields.appendChild(fieldBlock("Depends on", (c.depends_on || []).join(", ") || "-"));
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
		document.querySelector("#cd-files tbody"),
		(c.files || []).map((f) => [f]),
		1,
		"No files staged"
	);

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

	document.getElementById("imgd-remove").onclick = () => removeImage(name);
	document.getElementById("imgd-add-recipe").onclick = () => openModal("pkg-recipe-form", "Add or update a recipe");
}

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
		ownerCell.textContent = rec.owner || "-";
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

function renderLdapUsers(users) {
	const body = document.getElementById("ldap-users-body");

	body.textContent = "";
	if (users.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 7;
		cell.className = "empty";
		cell.textContent = "No LDAP users";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const u of users) {
		const row = document.createElement("tr");

		const cells = [u.name, u.uidnumber, u.primarygroup, u.mail,
		               u.has_password ? "set" : "unset", u.disabled ? "yes" : "no"];
		for (const v of cells) {
			const cell = document.createElement("td");
			cell.textContent = v;
			row.appendChild(cell);
		}

		const actionCell = document.createElement("td");
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

async function refreshPkiCa() {
	const pkiCaStatus = document.getElementById("pki-ca-status");
	const pkiCaForm = document.getElementById("pki-ca-form");

	try {
		const ca = await apiRequest("GET", "/v1/pki/ca");

		cache.pkiCa = ca;
		pkiCaStatus.className = "pki-ca-status bootstrapped";
		pkiCaStatus.textContent =
			"Bootstrapped: " + ca.subject + " (serial " + ca.serial + ", expires " + ca.not_after + ")";
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
			")";
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
		ownerCell.textContent = cert.owner || "-";
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

/* The buildable-definition catalog -- a recipe existing here says
 * nothing about whether it's installed anywhere (see
 * renderPackagesList() for that); the two lists deliberately show
 * different, independent sets (cache.pkgRecipes vs. cache.pkgList),
 * not the same data sliced two ways. */
function renderRecipesList() {
	const body = document.getElementById("recipes-body");

	body.textContent = "";
	if (cache.pkgRecipes.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 4;
		cell.className = "empty";
		cell.textContent = "No recipes";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const r of cache.pkgRecipes) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.appendChild(treeLink("#packages/" + encodeURIComponent(r.name), r.name, ""));
		row.appendChild(nameCell);

		const versionCell = document.createElement("td");
		versionCell.textContent = r.version;
		row.appendChild(versionCell);

		const dependsCell = document.createElement("td");
		dependsCell.textContent = r.depends || "-";
		row.appendChild(dependsCell);

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");

		rmButton.textContent = "Delete";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removePkgRecipe(r.name));
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

	const recipe = cache.pkgRecipes.find((r) => r.name === name);
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
		fields.appendChild(fieldBlock("Version", recipe.version));
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
	removeBtn.onclick = () => removePkgRecipe(name);

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

async function removePkgRecipe(name) {
	try {
		await apiRequest("DELETE", "/v1/pkg/recipes/" + encodeURIComponent(name));
		clearStatus();
		await refreshPkgRecipes();
		renderTree();
	} catch (e) {
		showStatus("Failed to remove recipe " + name + ": " + e.message, true);
	}
}

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
	const network = document.getElementById("f-network").value.trim();
	const ipForward = document.getElementById("f-ip-forward").checked;
	const dnsRegister = document.getElementById("f-dns-register").checked;
	const routesText = document.getElementById("f-routes").value.trim();
	const devices = Array.from(document.getElementById("f-devices").selectedOptions).map((o) => o.value);
	const interfaces = Array.from(document.getElementById("f-interfaces").selectedOptions).map((o) => o.value.replace(/^net:/, ""));
	const restart = document.getElementById("f-restart").value;
	const restartDelayText = document.getElementById("f-restart-delay").value.trim();
	const dependsOnText = document.getElementById("f-depends-on").value.trim();
	const readinessPortText = document.getElementById("f-readiness-port").value.trim();
	const readinessTimeoutText = document.getElementById("f-readiness-timeout").value.trim();
	const sysctlsText = document.getElementById("f-sysctls").value.trim();
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
	if (devices.length > 0)
		body.devices = devices;
	if (interfaces.length > 0)
		body.interfaces = interfaces;
	if (restart !== "no")
		body.restart = restart;
	if (restartDelayText !== "")
		body.restart_delay_seconds = parseInt(restartDelayText, 10);
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

	const body = {
		name: document.getElementById("luf-name").value.trim(),
		primarygroup: parseInt(document.getElementById("luf-primarygroup").value, 10),
		givenname: document.getElementById("luf-givenname").value.trim(),
		sn: document.getElementById("luf-sn").value.trim(),
		mail: document.getElementById("luf-mail").value.trim(),
		loginshell: document.getElementById("luf-loginshell").value.trim(),
		homedirectory: document.getElementById("luf-homedirectory").value.trim(),
		password: document.getElementById("luf-password").value,
		disabled: document.getElementById("luf-disabled").checked,
	};
	const uidnumberRaw = document.getElementById("luf-uidnumber").value;

	if (uidnumberRaw !== "")
		body.uidnumber = parseInt(uidnumberRaw, 10);

	try {
		await apiRequest("POST", "/v1/ldap/users", body);
		clearStatus();
		document.getElementById("ldap-user-form").reset();
		closeModal();
		await refreshLdapUsers();
	} catch (e) {
		showStatus("Failed to create LDAP user: " + e.message, true);
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

		const badge = document.getElementById("header-instance-name");

		badge.textContent = site.instance_name;
		badge.hidden = false;
		document.title = "Kanxeo — " + site.instance_name;
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

/* Fetch-on-demand, not folded into the global poll() loop the way
 * routes/daemon-config are -- this view has real filters (source/
 * level/tail) that change what's actually fetched, and a diagnostic
 * log view has no reason to keep re-polling the server every few
 * seconds while nobody's looking at it. */
async function refreshLogs() {
	const source = document.getElementById("lf-source").value;
	const level = document.getElementById("lf-level").value.trim();
	const tail = document.getElementById("lf-tail").value;
	let path = "/v1/system/logs?tail=" + encodeURIComponent(tail || "200");

	if (source)
		path += "&source=" + encodeURIComponent(source);
	if (level)
		path += "&level=" + encodeURIComponent(level);

	try {
		cache.logs = await apiRequest("GET", path);
	} catch (e) {
		cache.logs = [];
	}
	renderLogsTable();
}

function renderLogsTable() {
	const tbody = document.getElementById("logs-body");

	tbody.textContent = "";
	if (!cache.logs || cache.logs.length === 0) {
		tbody.innerHTML = '<tr><td colspan="4" class="empty">No log entries.</td></tr>';
		return;
	}
	for (const entry of cache.logs) {
		const tr = document.createElement("tr");
		const tsCell = document.createElement("td");
		const sourceCell = document.createElement("td");
		const levelCell = document.createElement("td");
		const msgCell = document.createElement("td");

		tsCell.textContent = new Date(entry.ts * 1000).toLocaleString();
		sourceCell.textContent = entry.source;
		levelCell.textContent = entry.level;
		msgCell.textContent = entry.msg;
		tr.append(tsCell, sourceCell, levelCell, msgCell);
		tbody.appendChild(tr);
	}
}

async function refreshLogsConfig() {
	try {
		const cfg = await apiRequest("GET", "/v1/system/logs/config");

		document.getElementById("lcf-max-bytes").value = cfg.max_bytes;
	} catch (e) {
		/* Best-effort -- the field just stays at whatever was last shown. */
	}
}

function renderLogsList() {
	refreshLogs();
	refreshLogsConfig();
}

document.getElementById("logs-filter-form").addEventListener("submit", (event) => {
	event.preventDefault();
	refreshLogs();
});

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
		a.download = "kanxeo-backup.json";
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

document.getElementById("sys-reboot").addEventListener("click", async () => {
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

document.getElementById("sys-shutdown").addEventListener("click", async () => {
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
		await refreshDnsRecords();
		await refreshDnsServers();
		await refreshLdapServers();
		await refreshLdapGroups();
		await refreshLdapUsers();
		await refreshLdapConfig();
		await refreshNtpConfig();
		await refreshNtpServers();
		await refreshNtpStatus();
		await refreshNtpTime();
		await refreshPkiCa();
		await refreshPkiIntermediate();
		await refreshPkiCerts();
		await refreshPkgRecipes();
		await refreshPkgList();
		await refreshSiteConfig();
		await refreshDaemonConfig();
		await refreshRoutes();
		await refreshSwap();
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

poll().then(ensureActiveCategoryExpanded);
setInterval(poll, POLL_INTERVAL_MS);
