# Contributing to Cix

The Cix Project welcomes bug reports, documentation improvements, tests, and
code contributions. Cix is led and maintained by Omar Sakka.

## Copyright and licence

Cix source is released under the Apache License, Version 2.0. Contributors
retain copyright in their own contributions. By submitting a contribution for
inclusion, you agree that it may be distributed as part of Cix under Apache-2.0
and that your contribution is made under the terms of that licence.

The project uses the Developer Certificate of Origin (DCO), version 1.1. Every
commit must include a sign-off line confirming that you have the right to submit
the work under the project licence:

```text
Signed-off-by: Your Name <you@example.com>
```

Use `git commit -s` to add it automatically. A sign-off is a provenance
statement, not a transfer of copyright: contributors keep ownership of their
contributions.

## Before opening a change

- Read `CLAUDE.md` and the relevant guide or ADR.
- Keep the API-first boundary intact: update OpenAPI before clients.
- Add or update tests and documentation with the implementation.
- Do not include credentials, private keys, generated build output, or copied
  third-party code without its licence and provenance.

The project maintainer makes the final decision about accepting changes,
releases, and the official Cix distribution.

