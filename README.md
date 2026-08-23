# ER-301 Sound Computer — VCV Rack Module

A VCV Rack plugin that embeds the full [ER-301 Sound Computer](https://www.orthogonaldevices.com/er-301) engine, including its Lua-based UI, DSP graph, and display. This is not a simplified emulation — it runs the actual ER-301 firmware with a HAL (Hardware Abstraction Layer) adapted for VCV Rack.

## Status

Working prototype. The full ER-301 engine runs inside VCV Rack with all 80+ core DSP units (oscillators, filters, delays, sample players, etc.), interactive controls, and dual display rendering.

**What works:**
- Full engine with Lua UI booting on a dedicated thread
- 20 inputs (4 audio, 12 CV, 4 gate) and 4 outputs
- 19 buttons (grey tactile + blue function) with press feedback
- Rotary encoder with drag/scroll interaction
- 2 toggle switches (STORAGE, MODE) — 3-position NKK style
- Main display (256x64, 4-bit grayscale) and sub display (128x64, 1-bit mono), amber tint, 4x upscaled with nearest-neighbor for crisp pixels
- All LEDs: output levels, link indicators, fine/coarse, I/O, safe, bicolor green/red CV LEDs via PWM readback
- Keyboard control of every button, the encoder and both toggles, using the SDL emulator's keymap
- Right-click context menu: keyboard capture, keymap legend, II debug injectors
- II / i2c follower — a directly adjacent VCV Teletype can drive the ER-301 with `SC.*` ops (see below)
- Error handling with overlay on both displays if engine fails
- Single instance guard (ER-301 uses global state)
- Sample rate mismatch warning at boot

**Current limitations:**
- macOS/ARM64 only
- Requires 48kHz sample rate in VCV Rack (ER-301 engine locks rate at init)
- Single instance only (global state in ER-301 firmware)
- ~2.67ms audio latency (128-sample frame buffering)
- **No patch state persistence.** Saving a VCV patch stores nothing about the ER-301 — use the firmware's own quicksave. State is global to the card, not per-patch, and the front card is shared with the SDL emulator, so quicksave slots are shared with it.
- **Deleting the module or quitting Rack can hang.** The destructor blocks on the Lua thread with no timeout; if the engine has already crashed, Rack may need to be force-quit.
- **TXo units will not instantiate.** The plugin implements only the I2C follower side, so the `txo` package fails to load on undefined master-side symbols.
- **II reads can never work.** The ER-301 firmware has no follower-response path, so Teletype II read ops are unsupported. Writes only.

## Usage

### Keyboard

Click the main display (or use *Capture keyboard* in the right-click menu) to give the module keyboard focus — an amber ring shows it has it. Click elsewhere to release it. While focused, every key is consumed so Rack's own shortcuts don't fire. The keymap mirrors the ER-301 SDL emulator, by physical key position:

| Keys | Function |
|---|---|
| `Q` `W` `E` `R` `T` `Y` | Main display softkeys 1–6 |
| `A` `S` `D` | Dial buttons 1–3 |
| `F` `G` `H` | Sub display buttons 1–3 |
| `V` `B` `N` | ENTER / UP / SHIFT |
| `1` `2` `3` `4` | Channel select 1–4 |
| `←` `→` | Encoder, coarse |
| `↑` `↓` | Encoder, fine |
| hold `Z` + `↑`/`↓` | STORAGE toggle |
| hold `X` + `↑`/`↓` | MODE toggle |

Buttons press on key-down and release on key-up, so held combos (SHIFT+something, SELECT+SELECT for channel linking) work as they do on hardware. Everything is released if focus is lost, so a held key can't leave a button stuck.

### Teletype / II

The plugin implements the ER-301's I2C **follower** side, so a Teletype can drive it with the `SC.*` ops — `SC.CV`, `SC.TR`, `SC.TR.PULSE` and friends — at follower address 0x31, exactly as over a real II bus.

Requirements:

1. A Teletype from the [`monome-rack-stolmine`](https://github.com/stolmine/monome-rack-stolmine) fork (plugin slug `monome-stolmine`). Stock monome-rack does not carry the expander transport.
2. Place the Teletype **directly adjacent** to the ER-301, on either side. Adjacency is strict — immediate neighbour only, nothing in between.
3. Load and enable the `teletype` package on the ER-301, which is what opens the follower on 0x31.

Transport is Rack's expander message buffers rather than a real bus, so it costs one sample of latency (immaterial against the 128-sample engine frame) but keeps II event timing sample-accurate within the frame.

**Reads are not possible.** The ER-301 firmware has no follower-response path at all, so Teletype II *read* ops can never work against it — writes only. This is a firmware limitation, not a plugin one.

The right-click menu has three debug injectors (`SC.CV 1 5V`, `SC.CV 1 0V`, `SC.TR.PULSE 1`) that push frames straight into the follower queue, so you can test the receive path with no Teletype patched at all.

> The wire contract lives in `src/ER301IIExpander.h` and is duplicated byte-identically in `monome-rack-stolmine/src/common/core/ER301IIExpander.h`. Edit one, edit the other, and bump `ER301_II_VERSION`.

## Roadmap

### Phase 1 — Usability
- [x] Keyboard shortcuts — Full emulator keymap over the display, including held-modifier combos
- [x] Right-click context menu — Keyboard capture action, keymap legend, II debug injectors
- [ ] Toggle click from center — Cycle state by clicking the middle zone (in addition to top/bottom regions)
- [ ] Button/knob tooltips — Hover tooltips on all custom controls
- [ ] More context menu entries — Log/data paths, link to docs. Override VCV "Randomize" to insert random ER-301 units
- [ ] Mouse & keyboard interaction — Keyboard text input for searching/naming (instead of encoder-only), click-drag scrolling on display for touch-style list navigation, mouse wheel on display areas for faster browsing. Goal: make the plugin comfortable to use without a physical controller

### Phase 2 — Persistence
- [ ] Toggle persistence — Save/restore toggle positions via `dataToJson`/`dataFromJson`
- [ ] SD card / filesystem testing — Validate quicksaves, preset save/load, WAV sample loading
- [ ] Module state persistence — Trigger quicksave on VCV patch save, restore on load

### Phase 3 — Integration
- [x] II / i2c follower — Receive `SC.*` ops from an adjacent Teletype over Rack's expander channel
- [ ] Bounded shutdown — Time out the Lua thread join so a crashed engine can't hang Rack on delete/quit
- [ ] I2C master side — Implement `I2c_openMaster`/`I2c_sendMessage`/`I2c_drainMasterQueue` so the `txo` package loads
- [ ] MIDI mapping — Map MIDI CC/notes to buttons, encoder, and toggles
- [ ] Performance profiling — Timing around `Pump_callback()`, frame processing stats

### Phase 4 — Deep Work
- [ ] Sample rate resampler — Decouple VCV and ER-301 sample rates by resampling at the audio bridge boundary, removing the 48kHz requirement
- [ ] Full engine state save/restore — Serialize complete Lua + DSP state beyond quicksaves
- [ ] Multi-instance support — Requires refactoring all ER-301 global state (probably not worth it)

### Phase 5 — VCV Library Publication
- [ ] Brand permission — Get approval from Orthogonal Devices (Brian Clarkson) to use ER-301 name/design, or rebrand
- [ ] Static FFTW — Replace Homebrew dynamic link with static build for cross-compilation
- [ ] Pre-generate SWIG — Commit `app_swig.cpp` so SWIG isn't needed at build time
- [ ] Cross-platform build — Linux + Windows via VCV rack-plugin-toolchain
- [ ] Relocate data files — Move `~/.od/` into Rack's standard plugin data folder
- [ ] Bundle assets — Include Lua scripts and xroot in `DISTRIBUTABLES`

## Architecture

The plugin replaces the ER-301's SDL-based emulator HAL with a VCV-native HAL layer (~20 files in `src/hal/`):

| Component | ER-301 Emulator | VCV Plugin |
|---|---|---|
| Audio I/O | SDL audio callback | `Module::process()` with ring buffer |
| Display | SDL textures | NanoVG `nvgCreateImageRGBA` on SVG panel |
| Buttons/GPIO | SDL keyboard/mouse | `SvgWidget` with click handlers, plus a keyboard capture widget |
| Encoder | SDL mouse wheel | Draggable knob with visual rotation |
| Toggles | SDL keyboard | NKK 3-position `SvgWidget` |
| I2C follower | am335x ISR | Lock-free SPSC ring fed from Rack expander messages |
| Concurrency | SDL threads/mutexes | `std::thread` / `std::mutex` |
| Timing | SDL ticks | `std::chrono` |
| Module map | `dl_iterate_phdr` | dyld image walk (`_dyld_get_image_header`) |
| Logging | stdout | `~/.od/er301-vcv.log` |

The audio bridge accumulates VCV's sample-by-sample calls into 128-sample frames, then calls `Pump_callback()` synchronously. Core DSP packages (like `libcore.so`) resolve ER-301 symbols via `RTLD_GLOBAL` promotion of the plugin dylib.

The two ER-301 cards live in different places. The **rear** card is isolated at `~/.od-vcv/rear`, because its DSP packages must be built with clang/libc++ to match the plugin's `std::string` ABI. The **front** card is shared with the SDL emulator at `~/.od/front` — samples, recordings and quicksaves have no ABI-specific content, and that is where your sample library already is. The trade-off is that quicksave slots are shared between the plugin and the emulator.

The embedded firmware is the [`er-301-vcv-firmware`](https://github.com/stolmine/er-301-vcv-firmware) fork, branch `vcv-compat`, currently tracking release `v0.7.0-stolmine.9.7.0` with three VCV-specific fixes on top (a `SequencerTask` heap crash, a `const` qualifier libc++'s `std::sort` requires, and a guard around the `emu` Lua module).

## Building

> **Tested only on macOS (Apple Silicon).** The Makefile detects `arm64` vs `x86_64` automatically, but only the ARM64 path has been verified. Linux/Windows are not supported yet — see Phase 5 of the roadmap.

### Prerequisites

- macOS with Xcode command-line tools
- [VCV Rack 2 SDK](https://vcvrack.com/manual/Building) — extract somewhere and remember the path
- ER-301 firmware source — the [`er-301-vcv-firmware`](https://github.com/stolmine/er-301-vcv-firmware) fork, on branch `vcv-compat`. The upstream [odevices/er-301](https://github.com/odevices/er-301) tree will not build here: the fork carries the compat fixes the plugin needs.
- Homebrew packages: `brew install fftw swig sdl2`
  (SDL2 is needed because the firmware's Doom screensaver pulls in doomgeneric, which includes `SDL.h` directly.)

### Step 1 — Build the ER-301 DSP packages

The plugin loads the ER-301's DSP packages (`libcore.so` and friends) at runtime from `~/.od-vcv/rear/v0.7/libs/`, a card kept separate from any SDL emulator install.

**Packages must be built with clang/libc++.** The Rack SDK builds the plugin with clang, whose `std::string` is `std::__1::basic_string`; the firmware's own `scripts/darwin.mk` hardcodes Homebrew GCC and libstdc++, whose `std::string` is `std::__cxx11::basic_string`. A GCC-built `.so` imports engine symbols the plugin never exports and fails to `dlopen` at unit-instantiate time. Override the compiler on the command line:

```bash
cd /path/to/er-301-vcv-firmware
make core ARCH=darwin PROFILE=testing CC=clang CPP=clang++
```

`CPP` is the variable that matters — the build never references `CXX`. If the package was previously built with GCC, `rm -rf testing/` first, or make will re-zip stale objects. Install by unzipping the resulting `.pkg` over `~/.od-vcv/rear/v0.7/libs/core/` — the package is a flat zip matching the card layout, so copying only the `.so` leaves the Lua tree version-skewed. (`make ... core-install` writes to `~/.od/rear`, which is the emulator's card, not the plugin's.) The plugin will fail to boot without this step.

To check a built package: `nm -u lib<pkg>.so | grep -c __cxx11` must be 0.

### Step 2 — Build and install the VCV plugin

```bash
git clone https://github.com/stolmine/er-301-vcv-plugin.git
cd er-301-vcv-plugin

# Symlinks: ER-301 firmware source tree and VCV Rack SDK
ln -s /path/to/er-301-vcv-firmware er-301
ln -s /path/to/Rack-SDK Rack-SDK

# Build and install. If brew isn't on PATH for the build shell, prepend it:
#   PATH="/opt/homebrew/bin:$PATH" make install
make install
```

For iterating during development, `make direct-install` copies files into the VCV plugins folder directly without going through zstd packaging.

### Step 3 — Verify

Launch VCV Rack at **48 kHz** (Engine → Sample Rate). Add the *ER-301 Sound Computer* module. The displays should boot and show the ER-301 home screen. If something goes wrong, the module overlays an error message and writes a log to `~/.od/er301-vcv.log`.

### Common pitfalls

- **Black screen / "engine init failed"** — usually means Step 1 was skipped or the firmware source path is wrong. Check `~/.od/er301-vcv.log`.
- **`ld: library 'fftw3f' not found`** (or `SDL.h` not found) — Homebrew isn't on the build shell's PATH. Run with `PATH="/opt/homebrew/bin:$PATH" make install`. Note that `brew --prefix sdl2` can resolve to a stale sdl2-compat path; the Makefile prefers `$(brew --prefix)/opt/sdl2` for exactly that reason.
- **A package's units won't instantiate** — check `~/.od/front/ER-301/logs/<pkg>.log`. A `symbol not found in flat namespace '...__cxx11...'` means that package is still a GCC build; rebuild it per Step 1.
- **`txo` units won't instantiate** — expected. The plugin implements only the I2C follower side, so the master-side symbols `txo` needs are undefined.
- **No audio / silence** — confirm VCV is at 48 kHz; the plugin shows a warning otherwise.
- **Second instance shows error overlay** — by design, only one ER-301 module can be loaded at a time (global state in the firmware).
- **Rack hangs on module delete or quit** — known issue; the destructor waits on the Lua thread with no timeout. Force-quit.

## File Structure

```
src/
  ER301Module.cpp      Main module: engine, widgets, panel layout, keyboard, context menu
  ER301IIExpander.h    II/i2c wire contract (mirrored in monome-rack-stolmine)
  plugin.cpp           VCV plugin registration
  hal/                 VCV HAL implementations (~20 files)
res/
  ER301.svg            SVG panel (Inkscape, 30HP)
  components/          SVG artwork for buttons, knob, toggles
er-301                 Symlink to the er-301-vcv-firmware checkout (branch vcv-compat)
sync_panel.sh          Automated SVG-to-C++ position syncing
```

See `documentation_index.md` for a map of the docs in this repo and the sibling repos.

## License

GPL-3.0-or-later

The ER-301 firmware is copyright [Orthogonal Devices](https://www.orthogonaldevices.com/) and licensed under GPL-3.0.
