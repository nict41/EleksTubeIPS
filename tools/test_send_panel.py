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


if __name__ == "__main__":
    unittest.main()
