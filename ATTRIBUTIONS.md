# Attributions

obs-ffgl is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### OBS Studio (libobs)

<https://github.com/obsproject/obs-studio>  
Licence: GPL-2.0-or-later  
Copyright: Lain Bailey and the OBS Project contributors

Headers only, fetched at a pinned tag by scripts/bootstrap.sh rather than vendored. The plugin links the libobs that ships inside the user's own OBS.app; nothing from obs-studio is redistributed.

The plugin ABI. An OBS filter or source is defined by libobs's headers, and there is no other way to be loadable by OBS. Note the consequence: a binary linking libobs is subject to the GPL, even where the source beside it is MIT.

### SIMD Everywhere (simde)

<https://github.com/simd-everywhere/simde>  
Licence: MIT  
Copyright: Evan Nemerson and the SIMDe contributors

Headers only, fetched at a pinned tag by scripts/bootstrap.sh rather than vendored.

Not a choice so much as a consequence: libobs's util/sse-intrin.h reaches for <simde/x86/sse2.h> on any non-x86 target, so on Apple Silicon every libobs header pulls it in. obs-studio's own build does the same.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
