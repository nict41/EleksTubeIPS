# What's new #
* **Flip transition:** the tubes flip like a split-flap display. The old top half folds down onto the hinge across the middle, darkening as it goes and uncovering the new top half, then the new bottom half folds down over the old one, each tube starting a moment after the one before it. The clock keeps the top half of each tube's last picture (compressed, about 2 to 5 KB a dashboard tube) in a part of the ESP32's memory nothing else used, so it costs no heap and no extra upload. A tube whose old picture it hasn't got (after a restart, a GIF or a photo) uncovers its new top half behind a dark edge instead. Use `transition=flip` with `duration` (each tube's flip) and `gap` (between their starts) in `POST /panel/batch`, or try it with **Flip** on the Panels page. Needs Tube Display 1.3 to use from Home Assistant.
* **Faster drawing:** a whole screen is drawn in about 150 ms instead of about 210 ms, thanks to a quicker QOI decoder and dimming. GIFs and every other picture benefit too.
* A whole screen is now always taken into the display buffer when no GIF plays. Before, a screen sent at the moment the clock's own loop held that buffer could be limited to 24 KB, and then lost its transition.

# Installation #
The IPSTube clocks (H401/H402 and the hardware-dimming one) can now update themselves over Wi-Fi, see [Updating over Wi-Fi](https://github.com/nict41/HA-Tubes/blob/main/docs/user-guide.md#updating-over-wi-fi). The other clocks still can't, so flash them over USB. The easiest way is the [web installer]($installer$), which works in Chrome or Edge on a computer. The [installation guide](https://github.com/judge2005/EleksTubeIPS/wiki/Installation) covers connecting the clock and backing up its original firmware.

**Upgrading from 1.9.4 or later:** choose **Upgrade** in the web installer, or flash only the firmware file for your clock at `0x10000`. Your settings, Wi-Fi details and uploaded images are kept. In Home Assistant, the existing entities keep their IDs.

```
esptool.py --chip esp32 --port <port> write_flash 0x10000 firmware-elekstubev1-$version$.bin
```

**New install, or upgrading from before 1.9.4:** choose **Full install** in the web installer, or flash `bootloader.bin` at `0x1000`, `partitions.bin` at `0x8000`, `boot_app0.bin` at `0xe000`, the firmware at `0x10000` and `littlefs-$version$.bin` at `0x180000`. A full install replaces your uploaded images with the ones that come with the firmware.

**IPSTube clocks moving from an older release to this one** have a new flash layout with two 2 MB app slots, so they need one **Full install** over USB (or the web installer's full install): `bootloader8MB.bin` at `0x1000`, `partitions8MB.bin` at `0x8000`, `boot_app0.bin` at `0xe000`, the firmware at `0x10000` and `littlefs8MB-$version$.bin` at `0x410000`. Settings and Wi-Fi details are kept, since nvs and otadata stay where they were, but the file system moves, so clock faces, weather icons or slides you uploaded yourself must be uploaded again. After that one full install, later updates can go over Wi-Fi from the Info page, with curl, or from Home Assistant. Recovery is still by USB if an update goes wrong.

NOTE: littlefs8MB-$version$.bin, bootloader8MB.bin and partitions8MB.bin are only for the IPSTube clocks. For all other clocks use littlefs-$version$.bin, bootloader.bin and partitions.bin.

| Clock | Firmware file |
|---|---|
| EleksTube IPS V1, Xunfeng IPS | `firmware-elekstubev1-$version$.bin` |
| EleksTube IPS V2 | `firmware-elekstubev2-$version$.bin` |
| NovelLife SE | `firmware-novellifese-$version$.bin` |
| Si Hai | `firmware-sihai-$version$.bin` |
| PCBWay PunkCyber | `firmware-punkcyber-$version$.bin` |
| IPSTube (H401/H402) | `firmware-ipstube-$version$.bin` |
| IPSTube used only through Home Assistant | `firmware-ipstube_ha-$version$.bin` |
| IPSTube with hardware dimming | `firmware-ipstube_dim-$version$.bin` |

Some clocks sold as an EleksTube IPS V2 need the V1 firmware. If every display shows the same digit after installing, install the other EleksTube version's firmware file.

`SHA256SUMS` has a checksum for each file. `firmware-symbols-$version$.zip` is only for decoding a crash's backtrace.
