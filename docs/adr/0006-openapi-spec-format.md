# 0006 — OpenAPI 3.0 YAML as the API spec format

## Status

Accepted

## Context

Given the API-First Mandate (ADR-0005), the spec itself needed a format. The two real options were a formal OpenAPI document versus a hand-written Markdown description of each endpoint. Nothing in this project auto-generates code or docs from the spec today, so the practical difference is mostly precision and future-proofing versus simplicity.

## Decision

OpenAPI 3.0, in YAML, at `docs/api/openapi.yaml`. It's the industry-standard shape for exactly this "spec anchors the implementation" workflow, is precise about request/response schemas and status codes in a way prose isn't, and costs nothing extra today while leaving the door open to generated docs or client stubs later without rewriting the contract.

## Consequences

- `docs/api/openapi.yaml` is the single source of truth for the API surface; the daemon is implemented to match it, not the other way around, and any future client (CLI, web) is built by reading it, not by reading the daemon's source.
- Nothing currently validates the daemon against the spec automatically (no codegen or contract-testing tooling) — keeping them in sync by hand is a discipline this project takes on, not a gap papered over. If that becomes error-prone as the surface grows, adding lightweight validation is future work, not a reason to abandon OpenAPI now.
