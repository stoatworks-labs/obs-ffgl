# obs-ffgl

Run FFGL video plugins natively inside OBS Studio — one filter, OBS's own GL
context, no network round trip. C++17/CMake. MIT (binary is GPL via libobs).

Read `AGENTS.md` before touching the render path or GL state handling.

## Commands
- Fetch headers: `./scripts/bootstrap.sh` (pinned sparse obs-studio + simde)
- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Install: `cmake --install build` (into `~/Library/Application Support/obs-studio/plugins`)
- Verify: `python3 tools/verify.py <bundle>…` (needs OBS running; uses obs-websocket)
- Control for "our bug or theirs": `oxbow selftest <bundle>`

## Notes
- Submodule: `third_party/oxbow` — the FFGL host core, compiled not copied.
  Host changes go to oxbow first, then bump the submodule here.
- `third_party/obs-studio` and `third_party/simde` are gitignored; bootstrap
  fetches them.
- OBS must be on its **OpenGL** renderer; the module refuses otherwise.
- `OBS_FFGL_PATH` adds plugin search directories. `OBS_FFGL_DEBUG=1..4` adds
  per-frame diagnostics (4 dumps textures to /tmp).
- Launch OBS as `/Applications/OBS.app/Contents/MacOS/OBS` to see plugin stdout;
  `open -a OBS` discards it.
