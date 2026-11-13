#!/usr/bin/env python3
"""Builds and runs the native tests of the panel decoders (test/native), with AddressSanitizer and
UndefinedBehaviorSanitizer, against QOI images made with Pillow.

    pip install pillow && python3 tools/native_tests.py

Needs g++ (or set CXX). CI runs it on every pull request."""
import os
import random
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def fixtures(out_dir):
    rnd = random.Random(7)
    images = {}
    w, h = 135, 240

    noise = Image.new("RGB", (w, h))
    noise.putdata([(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(w * h)])
    images["noise"] = noise                                  # QOI_OP_RGB everywhere

    grad = Image.new("RGB", (w, h))
    grad.putdata([(x * 255 // (w - 1), y * 255 // (h - 1), (x + y) % 256) for y in range(h) for x in range(w)])
    images["gradient"] = grad                                # DIFF and LUMA

    images["flat"] = Image.new("RGB", (w, h), (12, 34, 56))  # long runs, and runs across rows

    screen = Image.new("RGB", (w, h), (0, 0, 0))             # like a Tube Display tube: shapes, few colours
    d = ImageDraw.Draw(screen)
    d.ellipse([15, 50, 120, 155], outline=(120, 200, 255), width=8)
    d.rectangle([10, 180, 125, 200], fill=(255, 190, 0))
    d.text((30, 90), "47%", fill=(255, 255, 255))
    images["screen"] = screen                                # INDEX ops come back often

    rgba = Image.new("RGBA", (40, 30))
    rgba.putdata([(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), rnd.choice((0, 128, 255)))
                  for _ in range(40 * 30)])
    images["alpha"] = rgba                                   # 4 channels: QOI_OP_RGBA

    images["one"] = Image.new("RGB", (1, 1), (200, 100, 50))
    odd = Image.new("RGB", (17, 3))
    odd.putdata([(rnd.randrange(4) * 60, 0, 0) for _ in range(17 * 3)])
    images["odd"] = odd

    with open(os.path.join(out_dir, "manifest.txt"), "w") as manifest:
        for name, img in images.items():
            img.save(os.path.join(out_dir, name + ".qoi"), format="QOI")
            with open(os.path.join(out_dir, name + ".rgb"), "wb") as f:
                f.write(img.convert("RGB").tobytes())
            manifest.write(f"{name} {img.width} {img.height}\n")


def main():
    cxx = os.environ.get("CXX", "g++")
    if shutil.which(cxx) is None:
        sys.exit(f"{cxx} not found")
    with tempfile.TemporaryDirectory() as tmp:
        fixtures(tmp)
        exe = os.path.join(tmp, "native_tests")
        src = os.path.join(ROOT, "src")
        cmd = [cxx, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
               "-I", src, os.path.join(src, "QoiStream.cpp"), os.path.join(src, "PanelBatch.cpp"),
               os.path.join(ROOT, "test", "native", "test_main.cpp"), "-o", exe]
        print(" ".join(cmd), flush=True)
        subprocess.run(cmd, check=True)
        sys.exit(subprocess.run([exe, tmp]).returncode)


if __name__ == "__main__":
    main()
