# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*obs-ffgl — one OBS filter hosting any FFGL 2.x bundle natively in OBS's own GL context; 17 of 22 fleet bundles verified rendering inside real OBS*

**obs-ffgl** (`~/Projects/obs-ffgl`, started 2026-08-06, MIT, **PUBLIC** at
stoatworks-labs/obs-ffgl, pushed 2026-08-06). Registers **two** OBS objects — `ffgl_effect` (filter) and
`ffgl_source` (input, for FF_SOURCE generators) — that load any FFGL 2.x bundle, harvests its parameters into obs_properties, and renders it
**in OBS's own OpenGL context** — no second context, no readback, no NDI round
trip. Chosen over per-plugin native `.effect` ports because it delivers the whole
fleet plus third-party FFGL for one effort (the user picked this explicitly).

**The FFGL host core is oxbow's, compiled from `third_party/oxbow` as a
submodule, never copied** — `FfglInstance::process(inputTexture, w, h, hostFbo)`
is already exactly OBS's shape. resolume-ofx-bridge's `ffglguest` was rejected
for this: its `render()` is CPU-in/CPU-out, two readbacks a frame. Host changes
go to oxbow first, then bump the submodule.

**Why it works at all:** macOS OBS 32.1.2 runs **GL 4.1 core** (`4.1 Metal -
90.5`), the exact profile FFGL 2.x targets. `obs_module_load` checks
`gs_get_device_type() == GS_DEVICE_OPENGL` and registers nothing otherwise.
Metal/D3D11 would need IOSurface interop (already solved in
[resolume ofx bridge](https://github.com/stoatworks-labs/resolume-ofx-bridge/blob/main/docs/NOTES.md) (`resolume-ofx-bridge`)). Building needs only pinned sparse checkouts of
obs-studio headers + simde (`scripts/bootstrap.sh`, ~23 MB) and links against
the libobs inside the user's own OBS.app; OBS ships
`com.apple.security.cs.disable-library-validation`, so unsigned local plugins
load.

**Measured, in a real OBS, per pixel: 17 of 22 fleet bundles render correctly.**
The other 5 are all explained and none is obs-ffgl's fault — Cartridge and
Flipbook need a file parameter (ROM / sprite sheet), `ofxwrapper` correctly
refuses with no guest, and **Old Cathode and Coinop fail identically under
`oxbow selftest`** (pre-existing plugin bugs; Old Cathode is the known
[ffgl instantiate sweep trap](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_instantiate_sweep_trap.md)).

`tools/verify.py` drives a running OBS over **obs-websocket** (enabled here on
4455, password in `plugin_config/obs-websocket/config.json`) — builds a
hard-edged test card, screenshots, attaches the filter, screenshots again,
compares. It rejects a flat *baseline* and a flat *output*, both of which were
actually hit: "differs from the input" passes for a plugin rendering uniform
black.

Traps are catalogued in the repo's AGENTS.md ([agents md convention](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_agents_md_convention.md))
— the load-bearing ones are that `gs_texture_get_obj` returns a **pointer** to
the GLuint, that `obs_source_video_render` on a filter's own target re-enters the
chain and yields black, that libobs shadows GL state and skips redundant binds,
and that **pushing harvested parameter defaults corrupts values the plugin's
constructor already set**.

Parameter **groups** and **FF_TYPE_OPTION dropdowns** are done (the harvesting
was added to oxbow's host, ported from ffglguest, and the submodule bumped) —
Porthole's Quality reports range 0..1 while its elements run 0..4, so a slider
was actively wrong. Downpour, Orrery and Idler all verified rendering as OBS
*sources*; Outrun declares itself an effect, which is why the source list
filters on FF_SOURCE. Still open: `FF_TYPE_BUFFER` audio params, Windows/Linux.

See [oxbow](https://github.com/stoatworks-labs/oxbow/blob/main/docs/NOTES.md) (`oxbow`), [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md), [plugin bench](https://github.com/stoatworks-labs/plugin-bench/blob/main/docs/NOTES.md) (`plugin-bench`).

**Release/onboarding done 2026-08-06** (tag still to cut): macOS `build.yml` +
`release.yml`, `ATTRIBUTIONS.md`, `docs/USER-GUIDE.md`, and a **live website page
+ guide** at `/software/obs-ffgl/`. Attributions needed two new registry entries
(`obs_studio` GPL-2.0, `simde` MIT) and two detector rules — obs-ffgl reaches the
FFGL SDK through a **submodule of a submodule** (it carries oxbow, oxbow carries
resolume/ffgl), and its headers are **fetched by bootstrap.sh into a gitignored
dir**, so neither was visible to `vendored()`.

**The universal-build trap.** `-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64` **cannot
work here**: the plugin links the libobs inside an OBS.app, and OBS ships
*separate* Apple Silicon and Intel builds whose **libobs is single-architecture**
(arm64-only in the Apple download). A universal link against it fails, and
building only the runner's arch silently ships an Apple-Silicon-only plugin. The
release workflow therefore builds each slice against **its own OBS download** and
`lipo`s the two Mach-Os together. Validated locally — x86_64 cross-compiles
against the Intel libobs, lipo gives `x86_64 arm64` with a libobs ref per slice,
and the combined bundle **loads in a real OBS**.
