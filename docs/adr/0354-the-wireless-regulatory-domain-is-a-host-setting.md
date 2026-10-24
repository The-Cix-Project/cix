# 0354 — The wireless regulatory domain is a host setting

## Status

Accepted, 2026-10-24, for #355. The owner chose a host setting over the kernel command line, asked whether it should instead be per wireless interface, and accepted the answer below.

## Context

`ar-1`'s hostapd is refused `country_code=GB` on every start (`Failed to set country code`), so the host stays on the world domain `00`, where every 5 GHz rule is PASSIVE-SCAN and an access point may not initiate radiation there. Two measurements settle why.

- **The kernel gates the request on the initial user namespace.** Read from kernel 7.2.12 by `probe-regperm@1-1`: `NL80211_CMD_REQ_SET_REG` carries `GENL_ADMIN_PERM` (`net/wireless/nl80211.c:19741`), which is `netlink_capable(skb, CAP_NET_ADMIN)` (`net/netlink/genetlink.c:1182`), which is `ns_capable(&init_user_ns, ...)` (`net/netlink/af_netlink.c:883-886`).
- **A userns container is refused, live.** On 192.168.15.95, 2026-10-24, `iw reg set GB` in a `wifi_router` container with `"userns":true` answered `command failed: Operation not permitted (-1)`.

So no capability inside a userns container can set the country, and userns is the default for every container.

## Decision

1. **The country is one host setting**, `GET`/`PUT /v1/system/wireless` (`cixctl wireless show|set`), persisted in the state directory. cixd sends it as a user hint (`NL80211_CMD_REQ_SET_REG`) from the initial namespace at daemon start and on every change.
2. **Never per interface or per radio.** `nl80211_req_set_reg()` (`nl80211.c:10017-10044`) takes an alpha2 and nothing else and calls `regulatory_hint_user()`, which sets the host's one domain. A per-interface setting would offer something the kernel cannot do: setting a second radio to another country would silently move both. Two hosts in two countries each have their own setting.
3. **It is applied before anything can use a radio.** At start, before the kmod sweep and boot autostart: a wiphy registered later inherits the domain, and hostapd computes its channels when it starts.
4. **Intent and effect are reported apart.** The kernel applies a hint asynchronously and drops one for a country its database lacks, so the read reports the configured country and the domain the kernel has, separately. It also lists each radio that runs under a domain of its own; every other radio runs under the host's. The list comes from one `NL80211_CMD_GET_REG` dump, which walks every radio whatever its network namespace (`nl80211_get_reg_dump()`, `nl80211.c:10670`, read by `probe-regperm@2-2`), while a request naming one radio is resolved only in the caller's namespace (`:10562`). So a radio a container holds is covered without the daemon entering that namespace. The read is a sample, because the kernel changes it with no API call, and it is not on the device record, whose read is state and would report each such change as a missing announcement (ADR-0339).
5. **A container never sets the domain.** A recipe for an access point carries no `country_code` (and so no `ieee80211d`, which hostapd refuses without one).

## Alternatives

- **`cfg80211.ieee80211_regdom=` on the kernel command line.** cfg80211 is built in, so this is the parameter (`net/wireless/reg.c:276-281`). It needs no code. It is also not API-driven, takes a reboot to change, and adds a second writer of the boot entry's options beside the console setting.
- **`userns: false` for the access point.** That gives a WiFi container CAP_NET_ADMIN over the whole host to set one global value.
- **Per-radio settings.** Not expressible, as decision 2 says. A radio whose driver keeps its own domain (rtw88 with a country programmed in its efuse sets `REGULATORY_STRICT_REG` and hints it, `drivers/net/wireless/realtek/rtw88/regd.c:350-383`) is reported, not configured.

## Consequences

- A host with no setting stays on the world domain, as before.
- A container that sets a country is refused by the kernel, as it always was; the setting is what replaces it.
