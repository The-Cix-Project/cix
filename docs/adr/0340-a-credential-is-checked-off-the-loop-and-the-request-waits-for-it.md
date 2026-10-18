# 0340 — A credential is checked off the event loop, and the request waits for it

## Status

Proposed, 2026-10-18. Issue [#609](https://git.home.arpa/itdlabs/cix/issues/609). It changes the request pipeline, which is hard to reverse, and adds one documented response to `POST /v1/login`, so it is the owner's decision. It applies [ADR-0247](0247-the-reactor-does-not-block-and-that-is-the-defence.md) (the loop's blocking budget) and [ADR-0278](0278-the-reactor-forks-work-it-cannot-afford-to-wait-for.md) (`helper_run()`, the one place to add an offload), and leaves [ADR-0144](0144-host-authentication-and-real-ldap.md) (bcrypt, sessions) and [ADR-0317](0317-permissions-are-declared-by-the-api-contract.md) section 8 (app passwords on the user) unchanged. Nothing below is implemented.

## Context

cixd is one event loop. Every request, container exit, console byte and event-stream frame waits while a handler runs. Checking a password is the most expensive thing a handler does, and it runs on that loop.

**Measured on 192.168.15.95.**

- **One bcrypt check costs about 350 ms.** On 2026-10-18, `GET /v1/whoami` signed with an app password on its first use took 352 ms end to end. The same request on its second use, answered from the SHA-256 memo in `ldap_app_password_check()`, took 0.6 ms, and a bearer-token request took 0.85 ms. The cost factor is 12 (`PWHASH_BCRYPT_COST`), and the comment's "comfortably under 200ms" was false on this host (corrected in `19b66910`).
- **A login holds the loop for at least that long.** On 2026-10-17 the stall instrument named `POST /v1/login (375 ms)` as a pass's slowest handler. The stall record holds earlier login passes of 908, 929, 941 and 1070 ms (`GET /v1/system/stalls`, read 2026-10-18). Bcrypt is about 94% of the 375 ms one. How the others split is not measured.
- **A login with LDAP enabled can hold it for up to 9 s.** `try_ldap_login()` tries each configured server in turn with `ldapclient_bind()`, bounded by `HOSTAUTH_LDAP_TIMEOUT_MS` (3000) per server, and `HOSTAUTH_LDAP_MAX_SERVERS` is 3. If every server is down, a login waits out all three and then runs the local bcrypt check, which is read from the code rather than measured.
- **A wrong app password pays bcrypt on every request.** The memo holds only verified pairs (`ldap.c`, `g_app_memo`), so a failing Basic credential costs about 350 ms of loop time each time it is presented, until #547's per-address throttle blocks it.
- **Nothing answers a request after `dispatch()` returns.** The read path (`main.c`, the `pr == 1` branch) calls `dispatch()` and then `client_conn_finish()` unconditionally. Every `helper_run()` done callback writes state for a later read, and none writes a response (`container_disk_measure_done()` is typical). A connection serves one request and closes. The only deadline on a client connection is the output sweep (`conn_out_sweep()`, `CONN_OUT_DEADLINE_SECONDS`); nothing times out a connection that is waiting for its response to be produced.

Today the loop serialises every check, which accidentally rate-limits the whole box to about three logins a second. Every operator's dashboard and every `cixctl` command freezes for the length of somebody else's login. That is a product defect, not a slow endpoint.

## Decision

### One primitive checks a credential, and it never runs on the loop

1. **Every password check goes through one function, which runs only in a forked helper.** `credcheck_work()` (new, `daemon/src/credcheck.c`) does the whole chain that today lives in `hostauth_login()` and `ldap_app_password_check()`:
   - for a login: each LDAP server in order, then the local bcrypt record when no server gave a usable answer, exactly as `hostauth_login()` decides today;
   - for an app password: the bcrypt comparison against each of the user's app passwords.

   It is started with `helper_run()`, so it is the ADR-0278 primitive and not a second one. **`pwhash_bcrypt_check()` and `ldapclient_bind()` are called from nowhere else.**

2. **The child changes nothing and logs nothing.** It reads the user records and configuration it inherited at the fork. It reports one fixed-size result over a pipe, the way `container_disk_measure` does:
   - the verdict;
   - which path answered (an LDAP server, the local record, or an app password, with that app password's name);
   - a code per LDAP server tried, so that the "no CA chain, not binding at all" refusal and each unreachable server can still be logged.

   An exit status is not enough, because the parent has to log what happened and an exit status has only one bit. The child does not call `logstore_write()`. `try_ldap_login()` does today, and in a forked child that would be a second writer to the pid-1 daemon's log store, the silent split `helper_run()`'s own comment warns about.

3. **The parent does everything that has an effect.** When the helper finishes, the parent:
   - logs what the child reported;
   - records the result with #547's throttle (`hostauth_note_auth_failure()` or `connthrottle_record_auth_success()`);
   - inserts a verified app password into the memo;
   - for a login, issues the session.

   `hostauth_login()` splits into verifying, which moves into `credcheck_work()`, and `hostauth_issue_session()`. The second keeps today's code: the slot, the token, `save_sessions()` and the `EV_listHostauthSessions` announcement.

### The request waits; the contract does not change

4. **A request that needs a check is parked, and resumes from the start when the check finishes.** There are two places a check is needed:
   - `resolve_request_identity()`, for a Basic credential the memo does not answer;
   - `handle_login()`, after its throttle check and body parse.

   Each asks for a verdict. If the connection does not already carry one for that credential, the request is parked, `dispatch()` returns without a response, and the read path skips `client_conn_finish()` for a parked connection. When the helper finishes, the verdict is attached to the connection, bound to the SHA-256 of the user name and secret, so it cannot answer for a different credential. The request is then served again from the top. On the second pass the same call finds the verdict and continues as it does today.

   One mechanism covers both places, and it needs no completion code per handler. `POST /v1/login` still answers 200 with the token in one round trip. A Basic-authenticated request answers as it always has, only later.

5. **The read path's per-request steps move into one function that both passes use.** This is `serve_request(cc, req)`. It:
   - sets `g_response_conn` and `g_req_peer_ip` from the connection;
   - runs the WebSocket upgrades and `dispatch()`;
   - clears the stall activity;
   - finishes the connection unless the request was parked.

   The resume path calls the same function. Three of today's side effects run before a park, and each is settled here:
   - **The audit line** for a non-GET in `dispatch()` is written once, when the request arrives. A resumed request skips it, so a login is not audited twice.
   - **The stall activity** is set by `dispatch()` on each pass and cleared by `serve_request()` on each pass, so a resumed request is named as it is today.
   - **`connthrottle_auth_blocked()` and the JSON parse** have no effects and simply run again.

6. **A parked connection reads only to notice that its peer has gone.** It stays in epoll with `EPOLLIN`. One request per connection is the protocol today, so any further bytes are read and discarded, exactly as they are lost now when the connection closes after its response. A read of 0 tears the connection down.

   The helper's context is a **park record**, never the connection. Teardown clears the record's pointer to the connection, because a connection's free is deferred and the helper finishes later. The done callback still does the parent's bookkeeping when the peer has gone: a failure still counts against the address, and a verified app password is still memoised. It just writes no response, and a login from a peer that left issues no session.

### Bounds

7. **At most K checks run at once, and at most Q more wait.**
   - **K** is half the online CPUs, and at least one (`sysconf(_SC_NPROCESSORS_ONLN)` at startup). That leaves the loop and the rest of the host the other half. No core count is asserted here; it is read at startup.
   - **Q is 32.** Requests that cannot start a check wait in arrival order.
   - **A request beyond Q is answered `503 Service Unavailable`** with `Retry-After: 1` and "credential checks are busy -- try again shortly".

   That is deliberately not 429, which #547 gives a meaning of its own: this address has failed too often. A 503 here means the box is busy, says nothing about the caller, and does not count as a failure. #547's per-address block still runs before a request is parked, so a blocked address never queues.

8. **The fork stays on the loop, and it is the one cost implementation measures.** Every `helper_run()` already pays for one. A parked request has no deadline of its own beyond its check's bound, about 350 ms for bcrypt and about 9 s for three dead LDAP servers, plus its wait in the queue. Its peer can give up at any time, and item 6 handles that.

### Hashing, in a second phase, by the same mechanism

9. **Making a hash moves off the loop too.** `pwhash_bcrypt_new()` runs inline at `ldap.c:329` (a user's password set) and `ldap.c:2219` (an app password created). It costs the same as a check. The same park takes it: the child writes the 60-byte hash over the pipe, and on resume the handler commits the write with that hash. It is a second phase only because it is a separate set of handlers. It is tracked as [#610](https://git.home.arpa/itdlabs/cix/issues/610).

### Gates

10. **`test_blocking_waits` gains a third counted set.** It counts the call sites of `pwhash_bcrypt_check(`, `ldapclient_bind(` and `pwhash_bcrypt_new(`. Each budget is zero outside `credcheck.c`, except `pwhash_bcrypt_new(` in `ldap.c`, budgeted at its two sites until phase 2 lowers it to zero. A new inline check is then a number someone has to raise in a diff.

    `test_hostauth` adds what a static scan cannot see:
    - `GET /v1/health` answers while a login's check is in flight;
    - a client that disconnects mid-check leaves the daemon serving, and the next login succeeds;
    - a login beyond Q is answered 503.

## Consequences

- **Contract:** `POST /v1/login` gains a documented `503`, and a Basic-authenticated request can now answer 503 for the same reason. `openapi.yaml` and `docs/api/README.md` change in the same commit. Nothing else changes, and no client changes.
- **Daemon:**
  - new: `credcheck.c`, the park record and the bounded queue;
  - `hostauth_login()` split into verifying and `hostauth_issue_session()`;
  - `serve_request()` taken out of the read path;
  - the parked-connection read rule.
- **What stays as it is:**
  - the stored hash, which glauth already reads unchanged from the same `passbcrypt` field;
  - the cost factor;
  - the app-password memo;
  - #547's throttle;
  - the login audit by name.
- **Cost:** phase 1 is two to three build cycles, depending on what the floor tests find. Phase 2 is one more. Verified live on .95 with the measurement above: the 352 ms first-use request is repeated, and the stall record must name no credential check as a pass's slowest handler.

## Alternatives considered

- **Login as a job: 202, then a status read for the token.** Rejected, for two reasons:
  - **Every client changes:** `cixctl`, the dashboard and any script.
  - **The token would arrive through a second request** whose only secret is a job id. That is a new public read under ADR-0317's enumerated list, and a second place a token can be read.

  It also does nothing for Basic authentication, which has no job to poll.
- **A lower bcrypt cost.** Rejected as the fix: it is a security setting, not a scheduling one, and it does nothing for LDAP's up to 9 s. If it is ever lowered, that is its own decision.
- **A thread for checks.** Rejected: cixd has no threads and its globals are not built for them. `helper_run()` already gives the isolation a fork provides, and it is the primitive ADR-0278 says the next offload uses.
- **A completion callback per handler instead of resuming from the start.** Rejected: there are two places now and four after phase 2, and each would need its own code to write the response its handler already writes. Resuming from the start reuses every handler as it is. The cost is the three side effects item 5 names, each of which is settled there.
