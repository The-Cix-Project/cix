# 0392 — A deployment is re-applied in place, and keeps its secrets

## Status

Accepted, 2026-11-23, by the owner ("Accept"), for #724.

## Context

Changing one thing in a running container's deployment means removing the container and applying the deployment again, typing every `--secret` again.

The owner, 2026-11-22: *"why are we recreating the vpn servers? I don't understand?"* longwire 0.5.80 needed one argument, `--pair`, in vpn-1's and vpn-2's service command. The only route was `container rm` and `deployment apply` with `LONGWIRE_SWITCHBOARD` and `LONGWIRE_TOKEN` typed again. The day before, the same happened to ar-1 (its SSID), cr-1/cr-2 (three times) and br-2. Each retype needed the owner at the console, and each met #717's quoting bug. Removing first also risks the only copy of a secret: vpn-1 was down on 2026-11-02 for that reason.

Two facts shape the fix:

- `POST /v1/containers/recipes/{name}/apply` answers 409 for a name already in use. The stored container definition holds the **rendered** body, secrets substituted, and nothing records which value came from which `{{SECRET:KEY}}`.
- A path already exists that changes a running container's definition and restarts it: DHCP updates a server's definition and arms the rolling-restart timer (ADR-0386). That timer holds a roll group's members apart (ADR-0385) and is the one an image roll uses.

## Decision

1. **Applying a deployment whose container exists replaces it in place.** The deployment is rendered.
   - If the result equals the stored definition, nothing happens and the answer says so.
   - Otherwise the definition is updated and the container is restarted through the rolling-restart timer, so roll groups are respected exactly as for an image roll. The answer is 202 naming the restart.

   Volumes are untouched: a restart keeps them, as it always has.
2. **The secrets a deployment was applied with are kept, per deployment**, in one state file under `STATE_DIR`, mode 0600.
   - A later apply uses the secrets it is given, and takes every other `{{SECRET:KEY}}` the deployment needs from what is kept.
   - A given value replaces the kept one.
   - Nothing that answers a read returns a value. The API may say which keys are kept, never what they are.
3. **Kept secrets live as long as the deployment.** `container rm` does not remove them, so removing and re-applying (today's route) also stops needing them retyped. Removing the deployment itself removes them.
4. **This adds no new exposure.** The values are already at rest in the stored container definitions, so the new file is no more sensitive. It is handled the way those definitions are, in backup and restore.

## Alternatives considered

- **Recover each value by aligning the template with the stored rendered body.** Rejected: it is a reverse render that breaks as soon as a token sits next to text that changed, which is exactly when an operator re-applies.
- **Keep requiring `container rm` first.** Rejected: that is the problem, and it is what put the only copy of a secret at risk.
- **Keep secrets in git.** Never: the catalogue is public, and agent-1's deployment already takes its credential from `{{SECRET:...}}` for that reason.

## Consequences

- `cixctl deployment apply NAME` with no `--secret` is how a deployment change reaches a running container. vpn-1/vpn-2's `--pair` change would have needed no console visit.
- A re-apply restarts the container when its definition changed. For vpn-1 and vpn-2, stopping and starting is still the owner's call, so a re-apply there is asked first, as now.
- A deployment applied before this change has no kept secrets. Its first re-apply needs them given once.
- **Found while building it:**
  - **Fields read only by a create.** The create parses `restart`, `restart_delay_seconds`, `depends_on`, `follow_rolling` and `follow_rolling_jitter_seconds` once into the definition's cached index, and a restart replays the body without re-persisting it. A re-apply that changes one of them is therefore refused with 409, naming the field. The route is remove and apply again, which needs no secret typed since secrets are kept.
  - **No validation before the switch.** There is no create that does nothing, so the new definition is checked only for parsing and for naming the same container. A definition the create then refuses leaves the container down, retried with its reason (ADR-0346), until a corrected deployment is applied.
  - **Backups.** A system backup carries the file as `deployment_secrets`, beside `container_defs`, and a restore writes it back 0600.
- `test_container_recipe` checks the kept secret after `container rm`, the 200 unchanged re-apply, the 409 for a changed `restart`, and #696's refusal for a deployment never given its secret.
