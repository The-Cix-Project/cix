# 0372 — A build tool brings its whole build

## Status

Accepted by the owner on 2026-11-03, for #350, as the composition rule the owner chose for [ADR-0371](0371-the-control-plane-root-is-one-image.md): split the libraries, and have a `tool` bring its package's outputs. It amends [ADR-0199](0199-recipes-declare-their-build-tools.md) and [ADR-0367](0367-a-recipes-outputs-are-packages.md) and reverses nothing in them.

## Context

ADR-0371 makes the control-plane root one image of whole packages, and the owner chose that the root must not grow. So curl, openssl and ncurses get `-dev` outputs for their build-time files (`usr/include`, pkg-config files, `*-config` scripts), and the main package keeps the libraries, the CLIs and terminfo.

Those headers are what builds compose today. A recipe's `requires { build { tool "openssl" } }` resolves through `buildenv_add_tool()` (`daemon/src/pkg.c:7851`). That function finds the newest installed copy of the named package in any image, copies that package's recorded file list into the environment, and recurses on its recorded runtime dependencies. It knows nothing of outputs. After a split, `tool "openssl"` would compose libraries with no headers. The newest recipe of every package in cix-recipes, read on 2026-11-03, shows the reach: 25 declare `tool "openssl"`, 10 `tool "curl"` and 12 `tool "ncurses"`.

Two other routes were put to the owner and not chosen: split only the kernel and take the libraries whole, or revise the 47 recipes to name the dev output.

An output carries no role. cbs's explain JSON gives `{name, main, files, requires}` per output (cbs user manual 11.7), so cixd cannot tell a development set from a client like `cixctl` except by its name, and keying a rule on a `-dev` suffix is a convention dressed as a mechanism.

## Decision

1. **A declared build tool composes its package's main and every output of the same recipe, at the same version.** `tool "openssl"` means the whole build product of the openssl recipe, which is what it meant before the split. The outputs are listed from the stored recipe at the chosen version, through `recipe_outputs()`, not from the newest recipe. Each output is added as `name@version` through the same `buildenv_add_tool()` path, so its runtime dependencies follow as they do for any tool.
2. **The version chosen is the newest at which the main and every output are installed somewhere.** This applies to every package the walk resolves, not only declared tools. Otherwise a tool first pulled in as another tool's runtime dependency could settle on a version its outputs have not reached. Then what the environment holds would depend on the order the tools were written, and a declared tool that is already present still brings its outputs. Composition copies installed file lists and never the cache (ADR-0199). A main that rolled into one image before its outputs reached another therefore does not refuse a build. It resolves to the newest complete set. A version-pinned tool (`tool "openssl@3.0.20-8"`) with an output missing at that version is refused, naming the output.
3. **The rule applies to tools, not runtime dependencies.** A runtime `requires` names exactly the package it means, so `package "openssl"` at runtime stays the libraries alone. An output named directly as a tool (`tool "cixctl"`) is that output alone. "Whole build" is a property of naming a recipe's main package.
4. **The dev outputs are installed in `cix-builder`**, which follows its recipe, so they roll with their main packages. One image is enough, because composition takes a package from any image.
5. **Order.** This ships in a cixd release, gated by a `test_pkg` composition case (a tool whose recipe has an output, whose files must be in the environment). Only after that release is live do the split recipes reach cix-recipes, each published by `pkg recipe commit`. The `cix-builder` recipe that installs their dev outputs follows them, because an image entry naming a package that does not exist yet cannot converge. A split published first would break the 47 builds at their next rebuild.

## Consequences

- **No consumer recipe changes.** The environment's identity is the hash of its resolved `name@version` set (`buildenv_image_for()`, `pkg.c:8346`), so a tool that gains outputs composes a new environment rather than reusing one without the headers.
- **An environment can be larger than before by the outputs it gains.** `PKG_BUILDENV_MAX_TOOLS` is 64 (`pkg.c:418`). The three splits add at most one package per tool.
- **The rule has no counterexample today.** The only outputs that exist or are planned are `cixctl`, `cixd` and kernel modules, and no recipe declares `tool "cix"` or `tool "kernel"` (measured 2026-11-03). If an output ever must stay out of its recipe's builds, that is the time to ask cbs for a role field, not to add a name rule here.
