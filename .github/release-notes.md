# What's new #
The first release of **HA Tubes**, based on [EleksTubeIPS](https://github.com/judge2005/EleksTubeIPS) 2.1.6 by Paul Andrews.

* **Panels:** each tube can show a 135x240 picture pushed from Home Assistant (`POST /panel`), in the new **Panels** and **Four + Panel** display modes. Use the [Tube Display](https://github.com/nict41/nict41-s-HA-Apps/tree/main/tube_display) Home Assistant app to design and rotate screens; in Panels mode the clock's button shows its next screen.
* **Updates over Wi-Fi** on the 8 MB IPSTube clocks: the Info page, `curl --data-binary @firmware.bin http://<clock>/update`, or the Home Assistant **Firmware** update entity. `?target=fs` (or picking a `littlefs*.bin` on the Info page) updates the file system image too.
* **A new web UI look:** HA Tubes theme in light and dark, a grouped menu, and new Panels and About pages.

Clocks coming from EleksTubeIPS or an earlier HA Tubes build without two app slots need one **Full install** over USB (below). Settings and Wi-Fi details are kept; uploaded faces, icons and slides go back to the built-in ones.

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
| IPSTube with hardware dimming | `firmware-ipstube_dim-$version$.bin` |

Some clocks sold as an EleksTube IPS V2 need the V1 firmware. If every display shows the same digit after installing, install the other EleksTube version's firmware file.

`SHA256SUMS` has a checksum for each file. `firmware-symbols-$version$.zip` is only for decoding a crash's backtrace.
