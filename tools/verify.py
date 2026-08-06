#!/usr/bin/env python3
"""Render FFGL plugins through a real OBS and prove the picture changed.

    ./tools/verify.py ~/Projects/porthole/build/Porthole.bundle …

What it does, per plugin: build a test image with hard edges in it, put it in a
scratch scene, screenshot it, attach the `ffgl_effect` filter, screenshot it
again, and compare the two.

**Why a comparison and not an eyeball.** A filter that silently does nothing
looks exactly like a filter that works on footage you have not studied, and a
filter handed the wrong texture renders a plausible black frame. So the check
that has to pass is "the output differs from the input", with the per-pixel
numbers reported rather than a percentage — the fleet's own lesson from
plugin-bench is that percentages make two very different failures look alike.

**Why edges.** Outline and edge-detect plugins correctly output black on a
smooth ramp. A test card that is only a gradient makes a working plugin look
broken (oxbow hit exactly this in its selftest).

The scratch scene is created in whatever scene collection is loaded and removed
again at the end, and the program scene is put back. Nothing else is touched.
"""

import argparse
import base64
import io
import pathlib
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import obsws  # noqa: E402

SCENE = "obs-ffgl-verify"
SOURCE = "obs-ffgl-verify-input"
FILTER = "obs-ffgl-verify-filter"


def test_card(path: pathlib.Path, width: int = 640, height: int = 360) -> None:
    """Hard edges, flat areas, and a ramp — so that edge-detectors, blurs and
    colour operations all have something to bite on."""
    y, x = np.mgrid[0:height, 0:width]
    image = np.zeros((height, width, 3), dtype=np.uint8)

    # Checkerboard: the edges.
    checker = (((x // 40) + (y // 40)) % 2).astype(np.uint8) * 255
    image[..., 0] = checker

    # Horizontal ramp: the smooth part.
    image[..., 1] = (x * 255 // max(width - 1, 1)).astype(np.uint8)

    # A disc, so there is a curved edge that is not axis-aligned.
    cx, cy, r = width * 0.7, height * 0.5, min(width, height) * 0.25
    image[..., 2] = np.where((x - cx) ** 2 + (y - cy) ** 2 < r * r, 255, 30).astype(np.uint8)

    Image.fromarray(image).save(path)


def screenshot(obs: obsws.Obs, name: str, width: int, height: int) -> np.ndarray:
    reply = obs.call(
        "GetSourceScreenshot",
        {
            "sourceName": name,
            "imageFormat": "png",
            "imageWidth": width,
            "imageHeight": height,
        },
    )
    data = reply["imageData"].split(",", 1)[1]
    return np.array(Image.open(io.BytesIO(base64.b64decode(data))).convert("RGB"), dtype=np.int16)


def compare(before: np.ndarray, after: np.ndarray) -> tuple[int, int]:
    """(pixels differing, worst single-channel difference)."""
    delta = np.abs(after - before)
    differing = int(np.count_nonzero(delta.max(axis=2)))
    return differing, int(delta.max())


def verify_sources(args) -> int:
    """The ffgl_source input: an FFGL generator as a real OBS source.

    There is no input picture here, so "it changed" is not available as a
    check. What is left is that the output must have structure — a generator
    that renders nothing, or that is handed a broken context, produces a flat
    frame, and flat is the failure this catches.
    """
    args.outdir = args.outdir.resolve()
    args.outdir.mkdir(parents=True, exist_ok=True)

    obs = obsws.Obs(obsws.password_from_config())
    original_scene = obs.call("GetCurrentProgramScene")["sceneName"]
    obs.call("CreateScene", {"sceneName": SCENE})
    failures = 0
    try:
        obs.call("SetCurrentProgramScene", {"sceneName": SCENE})
        print(f"{'plugin (as source)':<28} {'min..max':>12}  result")
        print("-" * 60)

        for plugin in args.plugins:
            name = plugin.stem
            obs.call(
                "CreateInput",
                {
                    "sceneName": SCENE,
                    "inputName": SOURCE,
                    "inputKind": "ffgl_source",
                    "inputSettings": {
                        "ffgl_plugin": str(plugin),
                        "ffgl_width": args.width,
                        "ffgl_height": args.height,
                    },
                },
            )
            time.sleep(1.2)

            frame = screenshot(obs, SOURCE, args.width, args.height)
            Image.fromarray(frame.astype(np.uint8)).save(args.outdir / f"source-{name}.png")

            spread = int(frame.max()) - int(frame.min())
            verdict = "ok" if spread >= 16 else f"FLAT ({frame.min()}..{frame.max()})"
            failures += 0 if verdict == "ok" else 1
            print(f"{name:<28} {int(frame.min()):>4}..{int(frame.max()):<6}  {verdict}")

            obs.call("RemoveInput", {"inputName": SOURCE})
            time.sleep(0.3)
    finally:
        obs.call("SetCurrentProgramScene", {"sceneName": original_scene})
        obs.call("RemoveScene", {"sceneName": SCENE})
        obs.close()

    print(f"\nimages in {args.outdir}")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("plugins", nargs="+", type=pathlib.Path)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--outdir", type=pathlib.Path, default=pathlib.Path("build/verify"))
    parser.add_argument(
        "--source",
        action="store_true",
        help="test the ffgl_source input instead of the ffgl_effect filter. There is no "
        "input picture to compare against, so the check is only that the output has "
        "structure in it.",
    )
    args = parser.parse_args()

    if args.source:
        return verify_sources(args)

    # Absolute, always. OBS resolves a relative path against *its own* working
    # directory, so a relative one loads nothing, the source reports 0x0, the
    # filter passes through, and both screenshots match — which reads exactly
    # like a plugin that does nothing.
    args.outdir = args.outdir.resolve()
    args.outdir.mkdir(parents=True, exist_ok=True)
    card = args.outdir / "input.png"
    test_card(card, args.width, args.height)

    obs = obsws.Obs(obsws.password_from_config())
    original_scene = obs.call("GetCurrentProgramScene")["sceneName"]

    # A scratch scene, removed in the finally block whatever happens.
    obs.call("CreateScene", {"sceneName": SCENE})
    failures = 0
    try:
        obs.call("SetCurrentProgramScene", {"sceneName": SCENE})
        obs.call(
            "CreateInput",
            {
                "sceneName": SCENE,
                "inputName": SOURCE,
                "inputKind": "image_source",
                "inputSettings": {"file": str(card)},
            },
        )
        time.sleep(0.5)  # let the image actually load before screenshotting

        before = screenshot(obs, SOURCE, args.width, args.height)
        Image.fromarray(before.astype(np.uint8)).save(args.outdir / "before.png")

        # The control this whole test rests on. If the input never loaded, the
        # baseline is a flat frame, every plugin "changes nothing", and the run
        # reports a uniform failure that looks like a plugin problem. It is
        # not: it is the harness testing nothing at all.
        if int(before.max()) - int(before.min()) < 16:
            raise SystemExit(
                f"baseline is flat (min {before.min()}, max {before.max()}) — the test "
                f"card did not load into OBS, so nothing below would mean anything. "
                f"Check {card} exists and is readable by OBS."
            )

        print(f"{'plugin':<28} {'px differing':>13} {'worst chan':>11}  result")
        print("-" * 68)

        for plugin in args.plugins:
            obs.call(
                "CreateSourceFilter",
                {
                    "sourceName": SOURCE,
                    "filterName": FILTER,
                    "filterKind": "ffgl_effect",
                    "filterSettings": {"ffgl_plugin": str(plugin)},
                },
            )
            # The plugin is loaded and instantiated on the graphics thread, so
            # the first frame after attaching may still be the pass-through.
            time.sleep(1.0)

            after = screenshot(obs, SOURCE, args.width, args.height)
            differing, worst = compare(before, after)
            total = args.width * args.height

            name = plugin.stem
            Image.fromarray(after.astype(np.uint8)).save(args.outdir / f"after-{name}.png")

            # "It changed" is not "it worked". A plugin handed a bad input
            # texture renders a perfectly uniform black frame, which differs
            # from the test card in every single pixel and so sails through a
            # difference test. Demand structure in the output as well.
            spread = int(after.max()) - int(after.min())
            if differing == 0:
                verdict = "UNCHANGED — filter did nothing"
            elif spread < 16:
                verdict = f"FLAT ({int(after.min())}..{int(after.max())}) — no input?"
            else:
                verdict = "ok"

            failures += 0 if verdict == "ok" else 1
            print(f"{name:<28} {differing:>7}/{total:<5} {worst:>11}  {verdict}")

            obs.call("RemoveSourceFilter", {"sourceName": SOURCE, "filterName": FILTER})
    finally:
        obs.call("SetCurrentProgramScene", {"sceneName": original_scene})
        obs.call("RemoveScene", {"sceneName": SCENE})
        obs.close()

    print(f"\nimages in {args.outdir}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
