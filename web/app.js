/*
 * Kanxeo dashboard: a pure client of docs/api/openapi.yaml, same as
 * kanxeoctl (cli/src/main.c) -- no capability here that isn't already
 * one of the five REST endpoints. Vanilla JS, no framework, no build
 * step (docs/adr/0010).
 */
"use strict";

const POLL_INTERVAL_MS = 2000;

const healthBadge = document.getElementById("health");
const statusBox = document.getElementById("status");
const containersBody = document.getElementById("containers-body");
const runForm = document.getElementById("run-form");
const networksBody = document.getElementById("networks-body");
const networkForm = document.getElementById("network-form");
const dnsRecordsBody = document.getElementById("dns-records-body");
const dnsRecordForm = document.getElementById("dns-record-form");
const dnsServersBody = document.getElementById("dns-servers-body");
const dnsServerForm = document.getElementById("dns-server-form");

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
		throw new Error(message);
	}
	return json;
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

function renderContainers(containers) {
	containersBody.textContent = "";

	if (containers.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");
		cell.colSpan = 7;
		cell.className = "empty";
		cell.textContent = "No containers";
		row.appendChild(cell);
		containersBody.appendChild(row);
		return;
	}

	for (const c of containers) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.textContent = c.name;
		row.appendChild(nameCell);

		const statusCell = document.createElement("td");
		const statusSpan = document.createElement("span");
		statusSpan.textContent = c.status;
		statusSpan.className = "badge " + (c.status === "running" ? "badge-ok" : "badge-unknown");
		statusCell.appendChild(statusSpan);
		row.appendChild(statusCell);

		const pidCell = document.createElement("td");
		pidCell.textContent = c.pid;
		row.appendChild(pidCell);

		const exitCell = document.createElement("td");
		exitCell.textContent = c.exit_status === null || c.exit_status === undefined ? "-" : c.exit_status;
		row.appendChild(exitCell);

		const ipCell = document.createElement("td");
		ipCell.textContent =
			c.networks && c.networks.length > 0
				? c.networks.map((n) => n.name + ":" + n.ip).join(", ")
				: "-";
		row.appendChild(ipCell);

		const fwdCell = document.createElement("td");
		fwdCell.textContent = c.ip_forward ? "yes" : "no";
		row.appendChild(fwdCell);

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");
		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeContainer(c.name));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		containersBody.appendChild(row);
	}
}

async function refreshContainers() {
	try {
		const data = await apiRequest("GET", "/v1/containers");
		renderContainers(data.containers);
	} catch (e) {
		containersBody.textContent = "";
		const row = document.createElement("tr");
		const cell = document.createElement("td");
		cell.colSpan = 7;
		cell.className = "empty";
		cell.textContent = "Could not load containers: " + e.message;
		row.appendChild(cell);
		containersBody.appendChild(row);
	}
}

async function removeContainer(name) {
	try {
		await apiRequest("DELETE", "/v1/containers/" + encodeURIComponent(name));
		clearStatus();
		await refreshContainers();
	} catch (e) {
		showStatus("Failed to remove " + name + ": " + e.message, true);
	}
}

function renderNetworks(networks) {
	networksBody.textContent = "";

	if (networks.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");
		cell.colSpan = 5;
		cell.className = "empty";
		cell.textContent = "No networks";
		row.appendChild(cell);
		networksBody.appendChild(row);
		return;
	}

	for (const n of networks) {
		const row = document.createElement("tr");

		const nameCell = document.createElement("td");
		nameCell.textContent = n.name;
		row.appendChild(nameCell);

		const subnetCell = document.createElement("td");
		subnetCell.textContent = n.subnet;
		row.appendChild(subnetCell);

		const prefixCell = document.createElement("td");
		prefixCell.textContent = n.prefix_len;
		row.appendChild(prefixCell);

		const gatewayCell = document.createElement("td");
		gatewayCell.textContent = n.gateway;
		row.appendChild(gatewayCell);

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");
		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeNetwork(n.name));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		networksBody.appendChild(row);
	}
}

async function refreshNetworks() {
	try {
		const data = await apiRequest("GET", "/v1/networks");
		renderNetworks(data.networks);
	} catch (e) {
		networksBody.textContent = "";
		const row = document.createElement("tr");
		const cell = document.createElement("td");
		cell.colSpan = 5;
		cell.className = "empty";
		cell.textContent = "Could not load networks: " + e.message;
		row.appendChild(cell);
		networksBody.appendChild(row);
	}
}

async function removeNetwork(name) {
	try {
		await apiRequest("DELETE", "/v1/networks/" + encodeURIComponent(name));
		clearStatus();
		await refreshNetworks();
	} catch (e) {
		showStatus("Failed to remove network " + name + ": " + e.message, true);
	}
}

function renderDnsRecords(records) {
	dnsRecordsBody.textContent = "";

	if (records.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");
		cell.colSpan = 3;
		cell.className = "empty";
		cell.textContent = "No DNS records";
		row.appendChild(cell);
		dnsRecordsBody.appendChild(row);
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

		const actionCell = document.createElement("td");
		const rmButton = document.createElement("button");
		rmButton.textContent = "Remove";
		rmButton.className = "button-danger";
		rmButton.addEventListener("click", () => removeDnsRecord(rec.name));
		actionCell.appendChild(rmButton);
		row.appendChild(actionCell);

		dnsRecordsBody.appendChild(row);
	}
}

async function refreshDnsRecords() {
	try {
		const data = await apiRequest("GET", "/v1/dns/records");
		renderDnsRecords(data.records);
	} catch (e) {
		dnsRecordsBody.textContent = "";
		const row = document.createElement("tr");
		const cell = document.createElement("td");
		cell.colSpan = 3;
		cell.className = "empty";
		cell.textContent = "Could not load DNS records: " + e.message;
		row.appendChild(cell);
		dnsRecordsBody.appendChild(row);
	}
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

function renderDnsServers(servers) {
	dnsServersBody.textContent = "";

	if (servers.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");
		cell.colSpan = 3;
		cell.className = "empty";
		cell.textContent = "No DNS server bindings";
		row.appendChild(cell);
		dnsServersBody.appendChild(row);
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

		dnsServersBody.appendChild(row);
	}
}

async function refreshDnsServers() {
	try {
		const data = await apiRequest("GET", "/v1/dns/servers");
		renderDnsServers(data.servers);
	} catch (e) {
		dnsServersBody.textContent = "";
		const row = document.createElement("tr");
		const cell = document.createElement("td");
		cell.colSpan = 3;
		cell.className = "empty";
		cell.textContent = "Could not load DNS server bindings: " + e.message;
		row.appendChild(cell);
		dnsServersBody.appendChild(row);
	}
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

runForm.addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("f-name").value.trim();
	const image = document.getElementById("f-image").value.trim();
	const cmdText = document.getElementById("f-cmd").value.trim();
	const memoryMaxText = document.getElementById("f-memory-max").value.trim();
	const pidsMaxText = document.getElementById("f-pids-max").value.trim();
	const network = document.getElementById("f-network").value.trim();
	const ipForward = document.getElementById("f-ip-forward").checked;
	const routesText = document.getElementById("f-routes").value.trim();

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
			.filter((s) => s.length > 0);
	}
	if (ipForward)
		body.ip_forward = true;
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

	try {
		await apiRequest("POST", "/v1/containers", body);
		clearStatus();
		runForm.reset();
		await refreshContainers();
	} catch (e) {
		showStatus("Failed to create container: " + e.message, true);
	}
});

networkForm.addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("nf-name").value.trim();
	const subnet = document.getElementById("nf-subnet").value.trim();
	const prefixText = document.getElementById("nf-prefix").value.trim();

	const body = {
		name: name,
		subnet: subnet,
		prefix_len: parseInt(prefixText, 10),
	};

	try {
		await apiRequest("POST", "/v1/networks", body);
		clearStatus();
		networkForm.reset();
		await refreshNetworks();
	} catch (e) {
		showStatus("Failed to create network: " + e.message, true);
	}
});

dnsRecordForm.addEventListener("submit", async (event) => {
	event.preventDefault();

	const name = document.getElementById("df-name").value.trim();
	const ip = document.getElementById("df-ip").value.trim();

	try {
		await apiRequest("POST", "/v1/dns/records", { name: name, ip: ip });
		clearStatus();
		dnsRecordForm.reset();
		await refreshDnsRecords();
	} catch (e) {
		showStatus("Failed to create DNS record: " + e.message, true);
	}
});

dnsServerForm.addEventListener("submit", async (event) => {
	event.preventDefault();

	const container = document.getElementById("sf-container").value.trim();
	const hostsPath = document.getElementById("sf-hosts-path").value.trim();

	try {
		await apiRequest("POST", "/v1/dns/servers", { container: container, hosts_path: hostsPath });
		clearStatus();
		dnsServerForm.reset();
		await refreshDnsServers();
	} catch (e) {
		showStatus("Failed to register DNS server: " + e.message, true);
	}
});

async function poll() {
	await refreshHealth();
	await refreshContainers();
	await refreshNetworks();
	await refreshDnsRecords();
	await refreshDnsServers();
}

poll();
setInterval(poll, POLL_INTERVAL_MS);
