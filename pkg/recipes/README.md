# pkg/recipes/

52 `.recipe` files, one per package — real, from-source build recipes for Kanxeo's own `pkg install`. Each is a plain shell script following a strict, documented format: see [`docs/guides/writing-recipes.md`](../../docs/guides/writing-recipes.md) for the complete authoring guide, with a real worked example.

Naming: `<pkg_name>.recipe`, matching the recipe's own `pkg_name=` field exactly. Uploaded to a running daemon via `POST /pkg/recipes` or `kanxeoctl pkg recipe add --name=NAME --file=PATH` — these files are the *source* for a fresh deployment's initial catalog, never read directly off disk by `kanxeod` itself or baked into the installer ISO (see ADR-0040).
