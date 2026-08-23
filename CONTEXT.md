# ER-301 VCV Plugin — Development Context

This document captures the current state, key architectural decisions, and known constraints for developers continuing work on the plugin.

## Current State

The plugin is a working prototype. The full ER-301 engine (Lua UI, DSP graph, all 80+ core units) runs inside VCV Rack with interactive controls and dual display rendering. It replaces the ER-301's SDL-based emulator HAL with ~20 VCV-native HAL files in `src/hal/`.

Everything is in a single source file (`src/ER301Module.cpp`, ~1300 lines) containing the module, the widget classes (button, toggle, knob, display/panel, keyboard capture), and the engine initialization logic. This is intentional — the file is manageable at this size and avoids splitting tightly coupled VCV/HAL glue code across files.

The embedded firmware lives in its own repo, `stolmine/er-301-vcv-firmware` (branch `vcv-compat`), which the `er-301` symlink points at. That branch currently tracks release `v0.7.0-stolmine.9.7.0` and carries three VCV-specific commits on top of it: the `SequencerTask` Comparator ownership fix (heap crash), a `const` qualifier on `LeastWasteAllocator::operator<` (libc++'s `std::sort` requires it), and a guard around the `emu` Lua module so the control bridge tolerates it being absent.

### What's working
- Engine init with error handling, single instance guard, sample rate warning
- Audio bridge: 128-sample ring buffer bridging VCV's per-sample processing to ER-301's frame-based `Pump_callback()`
- Display: main (256x64, 4-bit grayscale) and sub (128x64, 1-bit mono), decoded and rendered via NanoVG with 4x nearest-neighbor upscale
- All controls: 19 GPIO-mapped buttons, encoder knob (drag + scroll), 2 three-position toggles
- All LEDs: GPIO-driven (output, link, fine/coarse, I/O, safe) and PWM-driven (12 bicolor CV LEDs)
- Core DSP packages: `libcore.so` and all 80+ units load via RTLD_GLOBAL symbol promotion
- Keyboard capture layer mirroring the SDL emulator's keymap (see below)
- Right-click context menu: keyboard capture action, keymap legend, II debug injectors
- I2C follower HAL, and a working II link to a VCV Teletype from the `monome-rack-stolmine` fork
- Crash reports are symbolizable: `od::enumerateModules()` is implemented for the plugin (`src/hal/modulemap.cpp`)

### What's not working yet
- **Deleting the module or quitting Rack can hang.** `~ER301Module` pushes `EVENT_QUIT` and then blocks on `luaThread.join()` with no timeout. If the Lua engine has already crashed the thread may never unwind, and Rack has to be force-quit.
- **TXo units will not instantiate.** The master half of the I2C API (`I2c_openMaster`, `I2c_sendMessage`, `I2c_drainMasterQueue`) is a fork addition to `hal/i2c.h` that the plugin does not define, so the `txo` package `.so` fails to `dlopen` on undefined symbols at unit-instantiate time.
- **II reads are impossible.** The ER-301 firmware has no follower-response path at all (no XRDY in the slave ISR mask), so Teletype II read ops can never work. Writes only.
- No state persistence — patch save/load stores nothing about the ER-301 (no `dataToJson`/`dataFromJson`). Use the firmware's own quicksave. Note that state is global to the card rather than per-patch, and the front card is shared with the SDL emulator, so quicksave slots are shared with it too.
- No MIDI mapping
- macOS/arm64 only, 48kHz only, single instance only

## Architecture Decisions

### Audio bridge (128-sample frames)
The ER-301 engine processes audio in fixed-size frames (default 128 samples). VCV Rack processes sample-by-sample. The bridge accumulates samples in `inFrame[]`, calls `Pump_callback()` when a full frame is ready, then reads output from `outFrame[]` one sample at a time. This adds ~2.67ms latency at 48kHz. There is no way around this without rewriting the ER-301 engine — it's fundamental to how the DSP graph works.

### RTLD_GLOBAL for DSP packages
The ER-301's Lua layer loads DSP packages (like `libcore.so`) via `dlopen`. These .so files reference symbols from the main ER-301 engine (e.g. `od::ZeroOutput`). In VCV Rack, those symbols live in `plugin.dylib`, which is loaded with `RTLD_LOCAL` by default. The fix: at init time, we use `dladdr` to find our own dylib path, then re-open it with `RTLD_NOW | RTLD_GLOBAL` to make symbols visible to subsequently loaded .so files.

### Single instance only
The ER-301 engine uses extensive global state (`globalConfig`, HAL singletons, Lua interpreter). Supporting multiple instances would require refactoring the entire ER-301 codebase. Instead, we use an atomic `instanceCount` and block the second instance with an error overlay.

### Sample rate locked at init
`globalConfig` (sample rate, frame length, derived values) is set once by `Config_init()` and used everywhere in the engine. The ER-301 was designed for fixed 48kHz operation. VCV Rack destroys and recreates modules on sample rate changes, but the engine's global state makes runtime rate changes unreliable. Current approach: check at init, show a warning if mismatched. Future approach: add a resampler at the audio bridge boundary to decouple the two rates entirely.

### Display rendering
The ER-301 writes to packed pixel buffers (4-bit grayscale for main, 1-bit for sub). We decode these into RGBA, writing each source pixel as a 4x4 block (UPSCALE=4) into a larger buffer. NanoVG renders the texture with `NVG_IMAGE_NEAREST` for sharp pixels at any zoom. The amber tint comes from setting R to full intensity and G to 85% (SCREEN_TINT=0.85).

### Config paths (rear isolated, front shared)
`Config_init()` stores raw `const char*` pointers to path strings. The module keeps `std::string` members (`xRootStr`, `rearRootStr`, `frontRootStr`, `firmwareCfgStr`) alive for the lifetime of the module to prevent dangling pointers.

The two cards point at different places on purpose. The REAR card is isolated at `~/.od-vcv/rear`: it holds the DSP packages, and those must be built with clang/libc++ to match the plugin's `std::string` ABI, so they cannot be shared with a GCC-built emulator install. The FRONT card is shared with the SDL emulator at `~/.od/front` — samples, recordings, quicksaves and browser history have no ABI-specific content, and that is where the user's sample library already lives. The consequence is that quicksave slots are shared between the plugin and the emulator.

### Keyboard capture
`ER301KeyboardCapture` is a transparent `OpaqueWidget` laid over the main display. Clicking it (or using the context-menu action) makes it Rack's selected widget, which routes `onSelectKey` to it. The keymap mirrors the SDL emulator by physical US-QWERTY position: `Q`–`Y` main softkeys, `A`–`H` dial and sub buttons, `V`/`B`/`N` enter/up/shift, `1`–`4` channel select, arrow keys as the encoder (left/right coarse = 5, up/down fine = 1), and `Z`/`X` held with the arrows to step the STORAGE/MODE toggles. Buttons press on key-down and release on key-up; `GLFW_REPEAT` is ignored because the engine runs its own auto-repeat. `onDeselect` releases every mappable button so a key held while focus is lost cannot leave a button stuck, and all other keys are consumed while focused so Rack's global shortcuts (space, delete, …) do not fire mid-edit. An amber ring is drawn while focused.

### II / i2c link to Teletype
The plugin implements the ER-301's I2C *follower* side only. `src/hal/i2cSlave.cpp` (previously a `.c` stub that always returned false) is a 64-deep lock-free SPSC ring mirroring `arch/am335x/hal/i2cSlave.c`: non-blocking in both directions, dropping the newest message when full, flushed on `I2c_openSlave`. Producers call `VCV_i2cPushMessage(address, data, length)`, which is also where address filtering happens — `I2cMessage` itself carries no address field.

Each frame is timestamped with `ticks()` individually as it arrives. This matters: `Dispatcher` converts `timestamp - mLastTimestamp` into a sample offset within the 128-sample frame, so batch-stamping a burst would collapse every event onto sample 0 and quantise II timing to 2.67 ms.

Transport between plugins is Rack's expander message buffers. A Teletype from the `monome-rack-stolmine` fork (plugin slug `monome-stolmine`) placed **directly adjacent** on either side can drive the ER-301 with `SC.*` ops (`SC.CV`, `SC.TR`, `SC.TR.PULSE`, …) at follower address 0x31. Adjacency is strict — immediate neighbour only, no chaining through other modules.

`src/ER301IIExpander.h` is the wire contract and is **duplicated byte-identically** in `monome-rack-stolmine/src/common/core/ER301IIExpander.h`. The two plugins are separate dylibs with no shared build, so nothing enforces this at compile time: anyone editing the struct must edit both copies and bump `ER301_II_VERSION`. Both sides validate magic/version/size, so drift degrades to silence rather than memory corruption across a plugin boundary.

### Module map for crash reports
Firmware 9.7.0 added `od/glue/CrashDiag.cpp`, which calls `od::enumerateModules()` with no weak default. It is implemented only in the am335x and linux arches, neither of which the plugin compiles, so the link failed on an undefined symbol. `src/hal/modulemap.cpp` provides it for macOS, which has no `dl_iterate_phdr`: it walks dyld's image list via `_dyld_get_image_header` plus the vmaddr slide, reading `__TEXT` and `__DATA`/`__DATA_CONST` extents out of the Mach-O load commands. It identifies our own dylib with `dladdr` and reports that first as "kernel", then reports only `.so` files (the DSP packages) — everything else in dyld's list is a system or Rack library and would just be noise. The result is that plugin crash reports can be fed to the firmware's own `tools/symbolize_crash.py`.

## Key Files

| File | Purpose |
|---|---|
| `src/ER301Module.cpp` | Module, widgets, engine init, display rendering, keyboard capture, context menu |
| `src/ER301IIExpander.h` | II/i2c expander wire contract — duplicated byte-identically in `monome-rack-stolmine` |
| `src/plugin.cpp` | VCV plugin registration |
| `src/hal/` | VCV HAL layer (~20 files replacing SDL HAL) |
| `src/hal/audio.c` | Audio bridge — exposes buffers to `Pump_callback()` |
| `src/hal/display.cpp` | Display buffer management |
| `src/hal/gpio.c` | Button/LED state via GPIO read/write |
| `src/hal/encoder.cpp` | Encoder delta accumulator |
| `src/hal/card.cpp` | Filesystem/SD card abstraction |
| `src/hal/i2cSlave.cpp` | I2C follower queue — lock-free SPSC ring, `VCV_i2cPushMessage()` entry point |
| `src/hal/modulemap.cpp` | `od::enumerateModules()` via dyld image walk, for symbolizable crash reports |
| `res/ER301.svg` | SVG panel (Inkscape, 30HP, component positions in hidden layer) |
| `res/components/` | SVG artwork: GreyButton, BlueButton, Rogan2SGray, NKK toggles |
| `sync_panel.sh` | Extracts component positions from SVG and prints C++ code |
| `Makefile` | Build config — ER-301 sources, FFTW, SWIG, direct-install target |

## Immediate Next Steps

These follow the roadmap in README.md, Phase 1 (Usability). Keyboard shortcuts and the right-click context menu are **done** — see the architecture notes above. What remains:

### 1. Toggle click from center
Currently toggles only respond to clicking the top or bottom third. Add center-third click to cycle through states (0→1→2→0). The click regions are in `ER301Toggle::onButton`.

### 2. Button/knob tooltips
Add `description` strings to custom widgets. VCV shows tooltips on hover for standard components, but our custom `SvgWidget`-based buttons and knob need manual tooltip support or wrapping in a `ParamWidget`.

### 3. Persistence
There is still no `dataToJson`/`dataFromJson`, so a saved patch remembers nothing about the ER-301 — not even the toggle positions. The cheap win is toggle state; the real one is triggering a firmware quicksave on patch save and a restore on load, with the caveat that quicksave slots live on the shared front card.

### 4. Context menu additions
`appendContextMenu` now exists (keyboard capture, keymap legend, II debug injectors). Still worth adding: log file path, xroot/rear/front paths, a link to the docs, and overriding or repurposing VCV's built-in "Randomize".

### 5. Mouse & keyboard interaction improvements
The keyboard capture layer covers the button/encoder keymap. The real ER-301 has a physical encoder and buttons — in VCV we can do better still:
- **Keyboard text input** — When the ER-301 UI is in a search/rename context, feed typed characters into the engine instead of requiring encoder scrolling through an alphabet. `ER301KeyboardCapture` already receives (and currently swallows) every key, so this is the natural place to hook it in. This is exploratory — it depends on how the Lua UI handles text input internally (may need to inject keypress events or call Lua functions directly).
- **Click-drag scrolling on displays** — Detect drag gestures over the main/sub display areas and translate vertical movement into encoder deltas, giving a touch-screen feel for scrolling lists and menus.
- **Mouse wheel on displays** — Forward scroll wheel events over display areas to the encoder, so you don't have to hover over the knob to scroll.
- The feasibility of text input depends on whether the ER-301 Lua UI exposes a text entry API or if it's purely encoder-driven. Needs investigation.

Beyond usability, the two things most worth fixing are the **shutdown hang** (bound the `luaThread.join()` in `~ER301Module` with a timeout so a crashed engine can't wedge Rack) and **persistence** (Phase 2).
