# Documentation Index

Every Markdown document in this repo, and where to look outside it.

## In this repo

| File | What it covers |
|---|---|
| `README.md` | User-facing overview: what works, current limitations, keyboard and Teletype/II usage, roadmap, build instructions, common pitfalls |
| `CONTEXT.md` | Developer context: current state, architecture decisions (audio bridge, RTLD_GLOBAL, card paths, keyboard capture, II link, module map), key file map, next steps |
| `documentation_index.md` | This file |

## Sibling repos

These are separate checkouts, expected alongside this one under `~/repos/`.

| Repo | What it is |
|---|---|
| `er-301-vcv-firmware` (`stolmine/er-301-vcv-firmware`) | The ER-301 firmware fork this plugin compiles in, on branch `vcv-compat` — currently release `v0.7.0-stolmine.9.7.0` plus three VCV-specific fixes (a `SequencerTask` heap crash, a `const` qualifier libc++'s `std::sort` requires, and a guard around the `emu` Lua module). The repo's `er-301` symlink points here. It also carries the firmware's own docs and `tools/symbolize_crash.py`, which can symbolize plugin crash reports thanks to `src/hal/modulemap.cpp`. |
| `monome-rack-stolmine` (`stolmine/monome-rack-stolmine`) | Fork of monome-rack (plugin slug `monome-stolmine`) whose Teletype can drive this module over II. It holds the other copy of the expander wire contract at `src/common/core/ER301IIExpander.h`, which must stay byte-identical to `src/ER301IIExpander.h` here. |

DSP packages are built from the firmware repo and from package repos such as `er-301-habitat`, `er-301-custom-units` and `Accents`; they must be built with clang/libc++ and installed to `~/.od-vcv/rear/v0.7/libs/`. See the Building section of `README.md`.
