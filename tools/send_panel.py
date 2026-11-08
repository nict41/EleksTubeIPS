#!/usr/bin/env python3
"""Sends a picture to the clock's panel tubes over HTTP, see "Showing pictures from Home Assistant
(panels)" in docs/user-guide.md.

    python3 tools/send_panel.py <clock-host> <image> [--tube N | --strip] [--user U --password P] [--status]

--tube N resizes and crops the image to fill tube N, 0 to 5 left to right, and sends it there.
--strip crops the image to fill all six tubes side by side (810x240), splits it into six 135x240
tiles and sends them to tubes 0 to 5 one after another. --status prints the GET /panel reply.

The pixels are sent as RGB565, big-endian (high byte first), row by row. Needs Python 3 with
Pillow and requests.
"""
import argparse
import sys

import requests
from PIL import Image
from requests.auth import HTTPDigestAuth

TUBE_WIDTH = 135
TUBE_HEIGHT = 240
NUM_TUBES = 6
STRIP_WIDTH = TUBE_WIDTH * NUM_TUBES	# 810


def cover(img, w, h):
    """Resizes img to fill w x h, cropping the overflow, centered."""
    img = img.convert("RGB")
    scale = max(w / img.width, h / img.height)
    resized = img.resize((max(w, round(img.width * scale)), max(h, round(img.height * scale))), Image.LANCZOS)
    left = (resized.width - w) // 2
    top = (resized.height - h) // 2
    return resized.crop((left, top, left + w, top + h))


def to_rgb565_be(img):
    """The pixels of img as RGB565, big-endian (high byte first), row by row."""
    data = img.convert("RGB").tobytes()
    out = bytearray(len(data) // 3 * 2)
    for i in range(0, len(data), 3):
        pixel = ((data[i] & 0xF8) << 8) | ((data[i + 1] & 0xFC) << 3) | (data[i + 2] >> 3)
        j = i // 3 * 2
        out[j] = pixel >> 8
        out[j + 1] = pixel & 0xFF
    return bytes(out)


def split_strip(img):
    """Crops img to fill the six tubes side by side, then splits it into six tiles, left to right."""
    strip = cover(img, STRIP_WIDTH, TUBE_HEIGHT)
    return [strip.crop((i * TUBE_WIDTH, 0, (i + 1) * TUBE_WIDTH, TUBE_HEIGHT)) for i in range(NUM_TUBES)]


def send(host, img, tube, auth):
    """Sends img to a tube. Exits with the clock's reply if it isn't ok."""
    response = requests.post(host + "/panel?tube=" + str(tube), data=to_rgb565_be(img), auth=auth,
                             headers={"Content-Type": "application/octet-stream"}, timeout=60)
    if response.status_code == 200:
        print("tube " + str(tube) + ": " + response.text.strip())
    else:
        sys.exit("tube " + str(tube) + ": HTTP " + str(response.status_code) + " " + response.text.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host", help="the clock's hostname or IP address, such as elekstubeips.local")
    parser.add_argument("image", nargs="?", help="the picture to send")
    parser.add_argument("--tube", type=int, metavar="N", help="the tube to send the picture to, 0 to 5 left to right (default 0)")
    parser.add_argument("--strip", action="store_true", help="send the image split across all six tubes")
    parser.add_argument("--user", help="the web UI login's user")
    parser.add_argument("--password", help="the web UI login's password")
    parser.add_argument("--status", action="store_true", help="print the GET /panel reply")
    args = parser.parse_args()

    if args.image is not None and args.strip and args.tube is not None:
        parser.error("give either --tube or --strip, not both")

    host = args.host if args.host.startswith("http") else "http://" + args.host
    auth = HTTPDigestAuth(args.user, args.password) if args.user and args.password else None

    if args.status:
        response = requests.get(host + "/panel", auth=auth, timeout=30)
        print(response.text.strip())
        if response.status_code != 200:
            sys.exit(response.status_code)

    if args.image is None:
        if not args.status:
            parser.error("nothing to do: give an image, or --status")
        return

    img = Image.open(args.image)
    if args.strip:
        for tube, tile in enumerate(split_strip(img)):
            send(host, tile, tube, auth)
    else:
        tube = args.tube if args.tube is not None else 0
        if tube < 0 or tube >= NUM_TUBES:
            parser.error("--tube must be 0 to " + str(NUM_TUBES - 1))
        send(host, cover(img, TUBE_WIDTH, TUBE_HEIGHT), tube, auth)


if __name__ == "__main__":
    main()
