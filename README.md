# obs-ffgl

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The render path has been
> verified against 22 FFGL bundles inside a running OBS Studio, and
> [Status](#status) says what that does and doesn't cover.

Run FreeFrame (FFGL) video plugins natively inside OBS Studio.

OBS has no FFGL support, and the usual workaround is a network round trip: send
a source out over NDI, run the effect in another application, and bring it back
again, trusting the network to behave. `obs-ffgl` cuts out the trip. It loads
any FFGL 2.x bundle, reads the plugin's own parameters into OBS's properties
(groups, dropdowns and all) and renders it in OBS's own OpenGL context, so
there's no second context, no readback and no copy back over the network.

Two things get registered:

- **FFGL Effect**, a filter, for FFGL effects.
- **FFGL Source**, an input, for FFGL generators, so a generator is a source
  in its own right rather than a filter on some throwaway picture underneath.

It's a companion to [oxbow](https://github.com/stoatworks-labs/oxbow), which does
the round-trip version for hosts with no plugin interface at all (vMix). This
repo compiles oxbow's FFGL host core straight from a submodule rather than
copying it, so there's only one implementation of the format.

**Video:** [What it does, in 50 seconds](https://www.youtube.com/watch?v=lAqGsz5TQqI)

<!-- downloads:start -->

## Download

**[v0.1.2](https://github.com/stoatworks-labs/obs-ffgl/releases/tag/v0.1.2)** — prebuilt for macOS. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .zip archive | [`obs-ffgl-0.1.2-macos-universal.zip`](https://github.com/stoatworks-labs/obs-ffgl/releases/download/v0.1.2/obs-ffgl-0.1.2-macos-universal.zip) | 101 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/obs-ffgl/releases](https://github.com/stoatworks-labs/obs-ffgl/releases).

macOS builds are signed and notarised by Apple, so they open normally — no Gatekeeper warning and no quarantine step.

<!-- downloads:end -->

## Status

**Early days, but the render path is there and it's measured.** Of 22 FFGL
bundles from this fleet, driven through an actual OBS 32.1.2 and compared per
pixel:

| result | count | which |
|---|---|---|
| renders correctly | 17 | Asciify, Coinop Over, Downpour, Downpour Over, Flipbook Over, Idler, Idler Mask, LumaKey, NESolume, Nib, Orrery, Orrery Mask, Outrun, Outrun Trace, Porthole, Scopes, Tinsel |
| needs a file parameter | 2 | Cartridge (ROM), Flipbook (sprite sheet) — set the path and they render |
| pre-existing plugin bugs | 2 | Old Cathode (`FF_INSTANTIATE_GL` fails), Coinop (`ProcessOpenGL` fails) — **both fail identically under oxbow**, so they are not this plugin's doing |
| correct refusal | 1 | `ofxwrapper` — the OFX bridge shell with no guest bundle |

As sources, the three FFGL generators I tested (Downpour, Orrery and
Idler) all render. Outrun declares itself an effect, so the source list doesn't
offer it.

**macOS only, and only on OBS's OpenGL renderer.** FFGL is an OpenGL format; if
OBS is running its Metal or D3D11 backend the module logs why and registers
nothing, rather than producing black. Windows and Linux aren't built.

**Not done:** audio-reactive `FF_TYPE_BUFFER` parameters, and anything that
needs OBS to drive the plugin's clock other than wall time.

## Build

```bash
git submodule update --init --recursive
./scripts/bootstrap.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
cmake --install build
```

`bootstrap.sh` fetches pinned, sparse checkouts of obs-studio's headers and
simde (~23 MB); neither is vendored, and the reasons are in the script. The
build links against the libobs inside your own `/Applications/OBS.app`.

Restart OBS, then either **add a source → Filters → + → FFGL Effect**, or
**+ → FFGL Source** for a generator.

## Where plugins are found

```
$OBS_FFGL_PATH                                     (colon-separated, wins)
~/Library/Application Support/obs-studio/plugin_config/obs-ffgl/plugins
~/Library/Graphics/FreeFrame Plug-Ins
/Library/Graphics/FreeFrame Plug-Ins
~/Documents/Resolume {Arena,Avenue}/Extra Effects
```

The filter's properties dialog lists these, so a plugin that doesn't appear
tells you where to look without a round trip.

## Verifying

```bash
python3 tools/verify.py ~/Projects/resolume/porthole/build/Porthole.bundle …
python3 tools/verify.py --source ~/Projects/resolume/downpour/build/Downpour.bundle …
```

It drives a running OBS over obs-websocket: builds a test card with hard edges,
screenshots it, attaches the filter, screenshots again, and compares. It
rejects both "nothing changed" **and** a flat output, because a plugin handed a
bad input texture renders uniform black, which differs from the input in every
pixel and would otherwise pass. Images land in `build/verify/`.

`OBS_FFGL_DEBUG=1..4` adds per-frame diagnostics; `4` dumps the input and
output textures to `/tmp` as PPMs. See `src/ffgl_effect.cpp`.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT (see `LICENSE`). Note that a binary linking libobs is subject to OBS's
GPL-2.0 terms; MIT is compatible, so the source here stays MIT and the
combined work is GPL.
