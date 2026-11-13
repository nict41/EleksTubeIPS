#!/usr/bin/env python3
"""Sends a picture to the clock's panel tubes over HTTP, see "Showing pictures from Home Assistant
(panels)" in docs/user-guide.md.

    python3 tools/send_panel.py <clock-host> <image> [--tube N | --strip] [--format raw|qoi|jpeg]
        [--transition none|wipe|cascade|flip [--duration MS] [--gap MS] [--order 2,3,1,4,0,5]]
        [--user U --password P] [--status]
    python3 tools/send_panel.py <clock-host> --test-pattern [wipe|cascade|flip|none]

--tube N resizes and crops the image to fill tube N, 0 to 5 left to right, and sends it there.
--strip crops the image to fill all six tubes side by side (810x240) and splits it into six 135x240
tiles. On HA Tubes 2.5 or later they go in one request (POST /panel/batch), and --transition has the
clock play a transition with them; older firmware gets them one after another. --status prints the
GET /panel reply. --test-pattern has the clock draw its own test pattern (HA Tubes 2.5 or later).

--format: raw is RGB565, big-endian, row by row (any firmware); qoi is lossless and much smaller;
jpeg is best for photos. The default is qoi when the clock takes it, raw otherwise. Needs Python 3
with Pillow and requests.
"""
import argparse
import io
import struct
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


def encode(img, fmt):
    """The picture's bytes in a format: "raw" (RGB565), "qoi" or "jpeg"."""
    if fmt == "raw":
        return to_rgb565_be(img)
    buf = io.BytesIO()
    if fmt == "qoi":
        img.convert("RGB").save(buf, format="QOI")
    else:
        img.convert("RGB").save(buf, format="JPEG", quality=90)
    return buf.getvalue()


BATCH_FORMATS = {"raw": 0, "qoi": 1, "jpeg": 2}


def batch_body(tiles, fmt, tubes=None):
    """A POST /panel/batch body: per tile a 14-byte little-endian header (tube, format, x, y, w, h,
    length) and then the picture. tubes gives each tile's tube (default 0, 1, 2, ...)."""
    parts = []
    for i, tile in enumerate(tiles):
        tube = tubes[i] if tubes else i
        pic = encode(tile, fmt)
        parts.append(struct.pack("<BBHHHHI", tube, BATCH_FORMATS[fmt], 0, 0, tile.width, tile.height, len(pic)))
        parts.append(pic)
    return b"".join(parts)


def transition_params(transition, duration=None, gap=None, order=None):
    """The query of a POST /panel/batch or /panel/test with a transition."""
    params = {}
    if transition and transition != "none":
        params["transition"] = transition
        if duration is not None:
            params["duration"] = str(duration)
        if gap is not None:
            params["gap"] = str(gap)
        if order:
            params["order"] = order
    return params


def split_strip(img):
    """Crops img to fill the six tubes side by side, then splits it into six tiles, left to right."""
    strip = cover(img, STRIP_WIDTH, TUBE_HEIGHT)
    return [strip.crop((i * TUBE_WIDTH, 0, (i + 1) * TUBE_WIDTH, TUBE_HEIGHT)) for i in range(NUM_TUBES)]


def send(host, img, tube, auth, fmt="raw"):
    """Sends img to a tube. Exits with the clock's reply if it isn't ok."""
    query = "/panel?tube=" + str(tube) + ("" if fmt == "raw" else "&format=" + fmt)
    response = requests.post(host + query, data=encode(img, fmt), auth=auth,
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
    parser.add_argument("--format", choices=("raw", "qoi", "jpeg"), help="picture format (default: qoi if the clock takes it)")
    parser.add_argument("--transition", choices=("none", "wipe", "cascade", "flip"), help="with --strip: a transition the clock plays")
    parser.add_argument("--duration", type=int, metavar="MS", help="how long a wipe takes, or each tube's flip (500)")
    parser.add_argument("--gap", type=int, metavar="MS", help="time between a cascade's tubes, or between the starts of their flips (90)")
    parser.add_argument("--order", metavar="TUBES", help="the tubes in the order they change, e.g. 2,3,1,4,0,5")
    parser.add_argument("--test-pattern", nargs="?", const="none", choices=("none", "wipe", "cascade", "flip"),
                        metavar="TRANSITION", help="have the clock draw its test pattern, with a transition")
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

    if args.test_pattern is not None:
        params = transition_params(args.test_pattern, args.duration, args.gap, args.order)
        response = requests.post(host + "/panel/test", params=params, auth=auth, timeout=30)
        print("test pattern: HTTP " + str(response.status_code) + " " + response.text.strip())
        if response.status_code != 200:
            sys.exit(1)
        return

    if args.image is None:
        if not args.status:
            parser.error("nothing to do: give an image, --test-pattern or --status")
        return

    info = {}
    try:
        info = requests.get(host + "/panel", auth=auth, timeout=30).json()
    except (requests.RequestException, ValueError):
        pass
    fmt = args.format or ("qoi" if "qoi" in info.get("formats", []) else "raw")
    if fmt != "raw" and fmt not in info.get("formats", []):
        print("warning: the clock doesn't say it takes " + fmt + " (older firmware?)", file=sys.stderr)

    img = Image.open(args.image)
    if args.strip:
        tiles = split_strip(img)
        body = batch_body(tiles, fmt) if info.get("batch") else None
        room = info.get("batch_buffer", 0)
        if body is not None and room and len(body) > room and not args.format and "jpeg" in info.get("formats", []):
            fmt = "jpeg"	# Photos: a QOI screen is far too big for the clock to hold, JPEG usually isn't
            body = batch_body(tiles, fmt)
        if body is not None and room and len(body) > room:
            print("warning: " + str(len(body)) + " bytes is more than the clock can hold (" + str(room)
                  + "), so the tubes go one by one, without a transition", file=sys.stderr)
            body = None
        if body is not None:
            params = transition_params(args.transition, args.duration, args.gap, args.order)
            response = requests.post(host + "/panel/batch", params=params, data=body, auth=auth,
                                     headers={"Content-Type": "application/octet-stream"}, timeout=60)
            print("six tubes, " + str(len(body)) + " bytes: HTTP " + str(response.status_code) + " "
                  + response.text.strip())
            if response.status_code != 200:
                sys.exit(1)
        else:
            if args.transition and args.transition != "none" and not info.get("batch"):
                print("warning: this clock can't play transitions (HA Tubes 2.5 can)", file=sys.stderr)
            for tube, tile in enumerate(tiles):
                send(host, tile, tube, auth, fmt)
    else:
        tube = args.tube if args.tube is not None else 0
        if tube < 0 or tube >= NUM_TUBES:
            parser.error("--tube must be 0 to " + str(NUM_TUBES - 1))
        send(host, cover(img, TUBE_WIDTH, TUBE_HEIGHT), tube, auth, fmt)


if __name__ == "__main__":
    main()
