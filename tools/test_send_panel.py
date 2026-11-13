#!/usr/bin/env python3
"""Tests for send_panel.py's pure functions. No network, and skipped if Pillow isn't installed:

    cd <repo root> && python3 -m unittest tools/test_send_panel.py -v
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    from PIL import Image
    import send_panel
except ImportError:
    Image = None
    send_panel = None


@unittest.skipIf(send_panel is None, "Pillow or requests isn't installed")
class TestSendPanel(unittest.TestCase):
    def solid(self, color, w=None, h=None):
        return Image.new("RGB", (w or send_panel.TUBE_WIDTH, h or send_panel.TUBE_HEIGHT), color)

    def test_rgb565_be_colors(self):
        cases = {
            (255, 0, 0): b"\xf8\x00",
            (0, 255, 0): b"\x07\xe0",
            (0, 0, 255): b"\x00\x1f",
            (255, 255, 255): b"\xff\xff",
            (0, 0, 0): b"\x00\x00",
        }
        for color, expected in cases.items():
            with self.subTest(color=color):
                self.assertEqual(send_panel.to_rgb565_be(self.solid(color, 2, 1)), expected * 2)

    def test_rgb565_be_whole_tube_size(self):
        data = send_panel.to_rgb565_be(self.solid((12, 34, 56)))
        self.assertEqual(len(data), send_panel.TUBE_WIDTH * send_panel.TUBE_HEIGHT * 2)
        self.assertEqual(len(data), 64800)

    def test_cover(self):
        # Wider than a tube: cropped left and right, full height
        img = send_panel.cover(self.solid((255, 0, 0), 270, 240), send_panel.TUBE_WIDTH, send_panel.TUBE_HEIGHT)
        self.assertEqual(img.size, (send_panel.TUBE_WIDTH, send_panel.TUBE_HEIGHT))
        # Taller than a tube: cropped top and bottom, full width
        img = send_panel.cover(self.solid((0, 255, 0), 135, 480), send_panel.TUBE_WIDTH, send_panel.TUBE_HEIGHT)
        self.assertEqual(img.size, (send_panel.TUBE_WIDTH, send_panel.TUBE_HEIGHT))
        # Smaller than a tube: scaled up to fill it
        img = send_panel.cover(self.solid((0, 0, 255), 10, 10), send_panel.TUBE_WIDTH, send_panel.TUBE_HEIGHT)
        self.assertEqual(img.size, (send_panel.TUBE_WIDTH, send_panel.TUBE_HEIGHT))

    def test_split_strip(self):
        # Paint each tile's area of the strip a different color, then check the tiles come back
        # in left-to-right order
        colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255)]
        strip = Image.new("RGB", (send_panel.STRIP_WIDTH, send_panel.TUBE_HEIGHT))
        for i, color in enumerate(colors):
            for x in range(i * send_panel.TUBE_WIDTH, (i + 1) * send_panel.TUBE_WIDTH):
                for y in range(send_panel.TUBE_HEIGHT):
                    strip.putpixel((x, y), color)

        tiles = send_panel.split_strip(strip)
        self.assertEqual(len(tiles), 6)
        for i, tile in enumerate(tiles):
            with self.subTest(tile=i):
                self.assertEqual(tile.size, (send_panel.TUBE_WIDTH, send_panel.TUBE_HEIGHT))
                self.assertEqual(tile.getpixel((0, 0)), colors[i])
                self.assertEqual(tile.getpixel((send_panel.TUBE_WIDTH - 1, send_panel.TUBE_HEIGHT - 1)), colors[i])

    def test_encode_formats(self):
        img = self.solid((10, 200, 30))
        self.assertEqual(send_panel.encode(img, "raw"), send_panel.to_rgb565_be(img))
        self.assertEqual(send_panel.encode(img, "qoi")[:4], b"qoif")
        self.assertEqual(send_panel.encode(img, "jpeg")[:2], b"\xff\xd8")

    def test_batch_body(self):
        import struct
        tiles = [self.solid((i * 40, 0, 0)) for i in range(6)]
        body = send_panel.batch_body(tiles, "qoi")
        off, tubes = 0, []
        while off < len(body):
            tube, fmt, x, y, w, h, n = struct.unpack_from("<BBHHHHI", body, off)
            off += 14
            self.assertEqual((fmt, x, y, w, h), (1, 0, 0, 135, 240))
            self.assertEqual(body[off:off + 4], b"qoif")
            off += n
            tubes.append(tube)
        self.assertEqual(off, len(body))
        self.assertEqual(tubes, [0, 1, 2, 3, 4, 5])
        raw = send_panel.batch_body(tiles[:2], "raw", tubes=[4, 1])
        self.assertEqual(len(raw), 2 * (14 + 64800))
        self.assertEqual((raw[0], raw[1]), (4, 0))

    def test_transition_params(self):
        self.assertEqual(send_panel.transition_params(None), {})
        self.assertEqual(send_panel.transition_params("none", 500), {})
        self.assertEqual(send_panel.transition_params("wipe", 400, None, "5,4,3,2,1,0"),
                         {"transition": "wipe", "duration": "400", "order": "5,4,3,2,1,0"})
        self.assertEqual(send_panel.transition_params("cascade", None, 70),
                         {"transition": "cascade", "gap": "70"})


if __name__ == "__main__":
    unittest.main()
