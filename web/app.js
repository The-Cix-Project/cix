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
	images: [],
	devices: [],
	dnsRecords: [],
	dnsServers: [],
	pkiCa: null,
	pkiCerts: [],
	pkgRecipes: [],
	pkgList: [],
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
	images: "view-images",
	devices: "view-devices",
	"dns-records": "view-dns-records",
	"dns-servers": "view-dns-servers",
	"pki-ca": "view-pki-ca",
	"pki-certs": "view-pki-certs",
	"pkg-recipes": "view-pkg-recipes",
	"pkg-installed": "view-pkg-installed",
	system: "view-system",
};

const DETAIL_VIEWS = {
	containers: "view-container-detail",
	networks: "view-network-detail",
	images: "view-image-detail",
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
		if (route.category === "networks" && route.name !== null)
			renderNetworkDetail(route.name);
		else if (route.category === "images" && route.name !== null)
			renderImageDetail(route.name);
		else if (route.category === "devices")
			renderDevices();
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

/* Maps every reachable route hash (a category's own hash, and each of
 * its fixed/dynamic children's hashes) back to the top-level category
 * hash that owns it in the tree -- rebuilt fresh by renderTree() every
 * poll, read by ensureActiveCategoryExpanded() on navigation. */
let hashToCategory = {};

function treeLink(href, text, className) {
	const a = document.createElement("a");

	a.href = href;
	a.textContent = text;
	a.className = className;
	return a;
}

/* Containers only -- the only resource with a real running/paused/
 * stopped/exited state (Container.status per the API, ADR-0045).
 * green = running, amber = paused (cgroup-frozen), grey = stopped
 * (manually stopped, still defined) or exited (process died, no
 * restart policy revived it) -- stopped and exited share one color
 * since both mean "not live" from a glance; the detail view's own
 * Status field spells out which. */
function treeItemLinkWithStatus(href, label, status) {
	const a = document.createElement("a");
	const dot = document.createElement("span");
	const dotClass =
		status === "running" ? "tree-status-running" : status === "paused" ? "tree-status-paused" : "tree-status-stopped";

	a.href = href;
	a.className = "tree-item";
	dot.className = "tree-status-dot " + dotClass;
	a.appendChild(dot);
	a.appendChild(document.createTextNode(label));
	return a;
}

function renderTree() {
	treeEl.textContent = "";
	hashToCategory = {};
	const root = document.createElement("ul");

	const addCategory = (label, hash, children) => {
		hashToCategory[hash] = hash;

		const li = document.createElement("li");
		const row = document.createElement("div");

		row.className = "tree-cat-row";

		const hasChildren = children && children.length > 0;
		const toggle = document.createElement("button");

		toggle.type = "button";
		if (hasChildren) {
			const collapsed = collapsedCategories.has(hash);

			toggle.className = "tree-toggle";
			toggle.dataset.hash = hash;
			toggle.textContent = collapsed ? "▸" : "▾";
			toggle.setAttribute("aria-label", "Toggle " + label);
		} else {
			toggle.className = "tree-toggle no-children";
			toggle.tabIndex = -1;
		}
		row.appendChild(toggle);
		row.appendChild(treeLink("#" + hash, label, "tree-category"));
		li.appendChild(row);

		if (hasChildren) {
			const ul = document.createElement("ul");

			ul.dataset.hash = hash;
			ul.hidden = collapsedCategories.has(hash);
			for (const child of children) {
				hashToCategory[child.hash] = hash;

				const childLi = document.createElement("li");

				childLi.appendChild(
					child.status !== undefined
						? treeItemLinkWithStatus("#" + child.hash, child.label, child.status)
						: treeLink("#" + child.hash, child.label, "tree-item")
				);
				ul.appendChild(childLi);
			}
			li.appendChild(ul);

			toggle.addEventListener("click", (event) => {
				event.preventDefault();
				const nowCollapsed = !ul.hidden;

				ul.hidden = nowCollapsed;
				toggle.textContent = nowCollapsed ? "▸" : "▾";
				if (nowCollapsed)
					collapsedCategories.add(hash);
				else
					collapsedCategories.delete(hash);
				saveCollapsedCategories();
			});
		}
		root.appendChild(li);
	};

	addCategory(
		"Containers",
		"containers",
		cache.containers.map((c) => ({ label: c.name, hash: "containers/" + encodeURIComponent(c.name), status: c.status }))
	);
	addCategory(
		"Networks",
		"networks",
		cache.networks.map((n) => ({ label: n.name, hash: "networks/" + encodeURIComponent(n.name) }))
	);
	addCategory(
		"Images",
		"images",
		cache.images.map((i) => ({ label: i.name, hash: "images/" + encodeURIComponent(i.name) }))
	);
	addCategory("Devices", "devices", null);
	addCategory("DNS", "dns-records", [
		{ label: "Records", hash: "dns-records" },
		{ label: "Servers", hash: "dns-servers" },
	]);
	addCategory("PKI", "pki-ca", [
		{ label: "Root CA", hash: "pki-ca" },
		{ label: "Certificates", hash: "pki-certs" },
	]);
	addCategory("Packages", "pkg-recipes", [
		{ label: "Recipes", hash: "pkg-recipes" },
		{ label: "Installed", hash: "pkg-installed" },
	]);
	addCategory("System", "system", null);

	treeEl.appendChild(root);
	renderTreeActive();
}

function renderTreeActive() {
	const current = location.hash.replace(/^#/, "") || "containers";

	for (const a of treeEl.querySelectorAll("a")) {
		const target = a.getAttribute("href").replace(/^#/, "");

		a.classList.toggle("active", target === current);
	}
}

/* Only called on real navigation (hashchange + the very first render),
 * never from the periodic poll -- otherwise a category the user
 * deliberately collapsed while staying on one of its own pages would
 * silently snap back open every 2s. */
function ensureActiveCategoryExpanded() {
	const current = location.hash.replace(/^#/, "") || "containers";
	const topSegment = current.split("/")[0];
	const categoryHash = hashToCategory[topSegment] || hashToCategory[current] || topSegment;
	const toggle = treeEl.querySelector('.tree-toggle[data-hash="' + categoryHash + '"]');
	const ul = treeEl.querySelector('ul[data-hash="' + categoryHash + '"]');

	if (toggle && ul && ul.hidden) {
		ul.hidden = false;
		toggle.textContent = "▾";
		collapsedCategories.delete(categoryHash);
		saveCollapsedCategories();
	}
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
	});
}

document.getElementById("cd-backup-goto-system").addEventListener("click", () => {
	location.hash = "#system";
});

function renderContainerDetail(name) {
	const c = cache.containers.find((x) => x.name === name);
	const title = document.getElementById("cd-title");
	const fields = document.getElementById("cd-fields");

	if (!c) {
		title.textContent = name + " (not found)";
		fields.textContent = "";
		closeConsole();
		return;
	}

	openConsole(name);
	title.textContent = c.name;

	/* Summary -- identity/runtime status only. */
	fields.textContent = "";
	fields.appendChild(fieldBlock("Status", c.status));
	fields.appendChild(fieldBlock("Image", c.image));
	fields.appendChild(fieldBlock("PID", c.pid === null || c.pid === undefined ? "-" : String(c.pid)));
	fields.appendChild(
		fieldBlock("Exit status", c.exit_status === null || c.exit_status === undefined ? "-" : String(c.exit_status))
	);

	/* Hardware -- devices/interfaces/network attachments granted at creation. */
	simpleTableRows(
		document.querySelector("#cd-devices tbody"),
		(c.devices || []).map((d) => [d.id, d.dev_path]),
		2,
		"No devices granted"
	);
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

		const gatewayCell = document.createElement("td");
		gatewayCell.textContent = n.has_gateway ? n.gateway : "(none)";
		row.appendChild(gatewayCell);

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
	fields.appendChild(fieldBlock("Gateway", n.has_gateway ? n.gateway : "(none -- pure L2)"));

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

	simpleTableRows(
		document.querySelector("#imgd-containers tbody"),
		usingContainers.map((c) => [c.name, c.status]),
		2,
		"No containers use this image"
	);

	renderImageDetailPackages(name);
	renderImageDetailRecipes(name);

	document.getElementById("imgd-remove").onclick = () => removeImage(name);
	document.getElementById("imgd-add-recipe").onclick = () => openModal("pkg-recipe-form", "Add or update a recipe");
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
		nameCell.textContent = pkg.name;
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

function renderDevices() {
	const container = document.getElementById("devices-groups");

	container.textContent = "";

	const busLabels = { pci: "PCI", usb: "USB", net: "Network interfaces", gpu: "GPU" };
	const buses = ["gpu", "pci", "usb", "net"];

	for (const bus of buses) {
		let entries = cache.devices.filter((d) => d.bus === bus);

		if (entries.length === 0)
			continue;

		const group = document.createElement("div");

		group.className = "device-group";
		const heading = document.createElement("h3");

		heading.textContent = busLabels[bus];
		group.appendChild(heading);

		const table = document.createElement("table");
		const thead = document.createElement("thead");
		const headRow = document.createElement("tr");

		for (const h of ["ID", "Description", "Driver", "Assignable"]) {
			const th = document.createElement("th");

			th.textContent = h;
			headRow.appendChild(th);
		}
		thead.appendChild(headRow);
		table.appendChild(thead);
		const tbody = document.createElement("tbody");

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

				groupCell.colSpan = 4;
				groupCell.textContent = groupId + " (grant this whole id to a container)";
				groupCell.style.fontWeight = "600";
				groupRow.appendChild(groupCell);
				tbody.appendChild(groupRow);
				for (const d of groups[groupId])
					tbody.appendChild(deviceRow(d));
			}
		} else {
			for (const d of entries)
				tbody.appendChild(deviceRow(d));
		}
		table.appendChild(tbody);
		group.appendChild(table);
		container.appendChild(group);
	}

	if (container.children.length === 0) {
		const empty = document.createElement("p");

		empty.className = "hint";
		empty.textContent = "No devices discovered.";
		container.appendChild(empty);
	}
}

function deviceRow(d) {
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
	return row;
}

/* ---------- DNS Records ---------- */

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

/* ---------- Packages: Recipes ---------- */

function renderPkgRecipes(recipes) {
	const body = document.getElementById("pkg-recipes-body");

	body.textContent = "";
	if (recipes.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 4;
		cell.className = "empty";
		cell.textContent = "No recipes found";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const r of recipes) {
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
		const rmButton = document.createElement("button");

		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removePkgRecipe(r.name));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshPkgRecipes() {
	const data = await apiRequest("GET", "/v1/pkg/recipes");
	cache.pkgRecipes = data.recipes;
	renderPkgRecipes(cache.pkgRecipes);
}

async function removePkgRecipe(name) {
	try {
		await apiRequest("DELETE", "/v1/pkg/recipes/" + encodeURIComponent(name));
		clearStatus();
		await refreshPkgRecipes();
	} catch (e) {
		showStatus("Failed to remove recipe " + name + ": " + e.message, true);
	}
}

/* ---------- Packages: Installed ---------- */

function renderPkgList(packages) {
	const body = document.getElementById("pkg-list-body");

	body.textContent = "";
	if (packages.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 8;
		cell.className = "empty";
		cell.textContent = "No packages";
		row.appendChild(cell);
		body.appendChild(row);
		return;
	}

	for (const pkg of packages) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.textContent = pkg.name;
		row.appendChild(nameCell);

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
			rmButton.addEventListener("click", () => removePkg(pkg.name, pkg.image));
			actionCell.appendChild(rmButton);
		}
		row.appendChild(actionCell);

		body.appendChild(row);
	}
}

async function refreshPkgList() {
	const data = await apiRequest("GET", "/v1/pkg");
	cache.pkgList = data.packages;
	renderPkgList(cache.pkgList);
}

async function removePkg(name, image) {
	const key = image && image !== "base" ? name + "@" + image : name;

	try {
		await apiRequest("DELETE", "/v1/pkg/" + encodeURIComponent(key));
		clearStatus();
		await refreshPkgList();
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
	const gateway = document.getElementById("nf-gateway").value.trim();

	const body = {
		name: name,
		subnet: subnet,
		prefix_len: parseInt(prefixText, 10),
	};
	if (gateway)
		body.gateway = gateway;

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
		await apiRequest("POST", "/v1/dns/records", { name: name, ip: ip });
		clearStatus();
		document.getElementById("dns-record-form").reset();
		closeModal();
		await refreshDnsRecords();
	} catch (e) {
		showStatus("Failed to create DNS record: " + e.message, true);
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

	if (fileInput.files.length === 0)
		return;

	try {
		const content = await readFileAsText(fileInput.files[0]);

		await apiRequest("POST", "/v1/pkg/recipes", { name: name, content: content });
		clearStatus();
		document.getElementById("pkg-recipe-form").reset();
		closeModal();
		await refreshPkgRecipes();
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
		await refreshDnsRecords();
		await refreshDnsServers();
		await refreshPkiCa();
		await refreshPkiCerts();
		await refreshPkgRecipes();
		await refreshPkgList();
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
