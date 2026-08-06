# obs-ffgl — invariants and traps

Read this before touching the render path. Everything here cost real debugging
time and none of it is guessable from the APIs.

## The shape of the thing

One OBS filter (`ffgl_effect`) hosting FFGL 2.x bundles in **OBS's own GL
context**. The FFGL host core is oxbow's, compiled from `third_party/oxbow`
rather than copied — `FfglInstance::process(inputTexture, w, h, hostFbo)` is
already the exact shape OBS needs, and the fleet has been bitten before by a
second copy of a library quietly becoming the real one. Changes to the host
belong in oxbow, with a submodule bump here.

`ffglguest` in resolume-ofx-bridge is the *other* FFGL host and is not used
here: its `render()` is CPU-in/CPU-out, which would mean two readbacks a frame.

## Why this only works on the OpenGL backend

`gs_get_device_type() == GS_DEVICE_OPENGL` is checked in `obs_module_load`, and
the module registers nothing otherwise. FFGL is an OpenGL format. macOS OBS
happens to run **GL 4.1 core** (`4.1 Metal - 90.5`), which is the profile FFGL
2.x targets, so plugins compile in the same driver Resolume uses. If OBS
switches to Metal by default this needs IOSurface interop — the pattern is
already solved in resolume-ofx-bridge.

## Traps

**`gs_texture_get_obj` returns a pointer to the GLuint, not the GLuint.** On
D3D11 the same call returns the `ID3D11Texture2D*` itself. `gl-texture2d.c`
ends `return &tex2d->base.texture;`. Dereference it. Getting this wrong hands
FFGL a pointer as a texture name and renders plausible black.

**Never `obs_source_video_render(obs_filter_get_target(...))` to capture the
input.** It re-enters the filter chain this filter is in, OBS's re-entrancy
guard declines to draw, and the texrender comes back entirely black — so a
pass-through plugin faithfully outputs black and looks broken. The symptom is
three errors per frame in OBS's log, not ours:

```
effect_setval_inline: invalid param
No vertex shader specified
device_draw (GL) failed
```

The supported idiom is `obs_source_process_filter_begin`, then
`obs_source_process_filter_end` **inside** the texrender's scope, so OBS's own
machinery renders the input into our texture.

**libobs shadows its GL state and skips redundant binds.** `struct gs_device`
caches `cur_fbo`, `cur_textures[]`, `cur_program`, `cur_viewport`…, and every
setter compares first (`if (device->cur_fbo != fbo)`). An FFGL plugin that
leaves its own framebuffer bound therefore makes libobs render the *rest of the
scene* into it, one frame later, with no error. `GlStateGuard` restores — never
resets — everything libobs caches. FFGL plugins are expected to trash bindings;
the stock SDK's `Scoped*` helpers restore to 0 rather than to the previous
value.

**OBS's leftover GL state is hostile to a guest renderer.** Scissor, stencil,
depth, cull, colour mask and `GL_FRAMEBUFFER_SRGB` are all legitimately set by
OBS and all silently discard or recolour a plugin's draws — measured as
`FF_SUCCESS` plus a complete, entirely black framebuffer. `resetForPlugin()`
puts GL into the state Resolume would hand a plugin. Also unbind libobs's
**sampler objects**: they override the texture's own filter and wrap settings,
which FFGL plugins set on the texture.

**Do not push harvested parameter defaults.** The plugin's constructor has
already applied its own defaults; what we harvested is our *reading* of them,
and where the two disagree the push replaces working values with wrong ones.
Measured: pushing defaults made Porthole render at quarter scale in the corner,
while the same bundle under oxbow — which pushes only what `--set` names —
fills the frame. Push only dirty values, and stay silent on the first frame
except for values genuinely off their defaults. The OFX bridge reached the same
rule independently. `obs_data_get_*` answers 0 for a key the operator never
touched, so gate on `obs_data_has_user_value` or a fresh filter zeroes every
parameter.

**`OBS_SOURCE_CUSTOM_DRAW` is an input flag, not a filter one.** Setting it on
a filter leaves the direct draw without a shader loaded.

**Do not scan for plugins in `obs_module_load`.** The search path includes
`~/Documents/Resolume …`, and the first read of `~/Documents` triggers a macOS
TCC consent prompt. From module load that blocks OBS's startup until it is
answered — measured at **22 seconds** the first time against 137 ms after —
before the main window exists, so the operator sees a hung OBS and a permission
dialog with nothing behind it. Scan lazily, when properties are first built.

## Verification

`tools/verify.py` drives a real OBS over obs-websocket. Two controls exist
because both failures were actually hit:

- The baseline must not be flat. A test card that fails to load makes every
  plugin "change nothing" and reads as a uniform plugin failure. (OBS resolves
  a relative path against **its own** working directory — pass absolute paths.)
- The output must not be flat. "It differs from the input" passes for a plugin
  rendering uniform black, which is exactly what a bad input texture produces.

Report counts, not percentages — plugin-bench's lesson: percentages make two
very different failures look identical.

The honest control for "is this our bug or the plugin's" is
`oxbow selftest <bundle>`. Old Cathode and Coinop fail identically there.
