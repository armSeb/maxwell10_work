# Gigaset Maxwell 10 — mainline Linux and postmarketOS

*[Version française](README.fr.md)*

The **Gigaset Maxwell 10** is a 10.1" Android desk phone (IP + DECT) built around an NXP i.MX6 Quad.
This project runs a **mainline Linux kernel** and **postmarketOS** on it from a **microSD card**,
without modifying the internal eMMC: Android stays intact and boots again as soon as the card is
removed. Typical uses: a Home Assistant wall dashboard, a small touch terminal, a DECT handset you
can drive from Linux.

> postmarketOS was renamed **Nura** in 2026. The tools (`pmbootstrap`, `pmaports`) keep their
> names, and so does this documentation.

## Status

| Feature | State | Notes |
|---|---|---|
| Boot from microSD (U-Boot `go`, HAB untouched) | ✅ | [boot chain](docs/en/boot.md) |
| Display LVDS 1280×800, touch screen | ✅ | KMS (imx-drm), FT5x06 |
| GPU (Vivante GC2000, etnaviv) | ✅ | OpenGL ES 2.0 only (see the GTK4 note in [camera.md](docs/en/camera.md#performance)) |
| CPU frequency scaling, fan | ✅ | fan driven by the kernel thermal zone |
| Ethernet (2 ports, Marvell 88E6320 switch) | ✅ | ports `lan` and `pc` |
| Wi-Fi (Atheros AR6003) | ✅ | needs the non-free firmware |
| Bluetooth (Atheros AR3002) | ✅ | scan, pairing, A2DP |
| USB OTG (gadget networking) | ✅ | `ssh user@172.16.42.1` |
| USB host (2 type-A ports) | 🧪 | |
| Speaker (DVF9919 woofer + tweeter) | ✅ | PulseAudio output, sleeps when idle |
| DECT handset: earpiece, microphone, hook events | ✅ | audio path only, no telephony stack yet |
| Camera (OmniVision OV9740) | ✅ | 1280×720 / 640×480, 30 fps |
| Front microphone | ❌ | routing unknown |
| HDMI, ambient light sensor | ⏳ | not done |

✅ works · 🧪 written, not fully verified · ❌ not working · ⏳ not started

## Getting started

You need a Linux PC, a microSD card (SDHC, 32 GB max, preferably without UHS-I) and a 3.3 V
USB-serial adapter for a one-time U-Boot setup. The whole procedure is in
**[docs/en/install.md](docs/en/install.md)**. In short:

```sh
git clone https://github.com/armSeb/maxwell10_work && cd maxwell10_work
pmbootstrap init                        # once, any device: creates the config and the pmaports clone
pmos/install-sdcard.sh /dev/sdX         # builds the image, fixes the partition table, writes the card
```

then set the U-Boot environment once over the serial console.

## Documentation

| Document | Content |
|---|---|
| [install.md](docs/en/install.md) | building, writing the card, U-Boot setup, first boot, updates, troubleshooting |
| [hardware.md](docs/en/hardware.md) | hardware inventory, buses, GPIOs, mainline driver status |
| [boot.md](docs/en/boot.md) | secure boot (HAB), how `go` + goshim boot an unsigned kernel, U-Boot pitfalls |
| [audio-dect.md](docs/en/audio-dect.md) | DSPG DVF9919: CMBS protocol, audio paths, `dvf99` daemon, DECT handset |
| [camera.md](docs/en/camera.md) | OV9740 driver, imx-media pipeline, applications |
| [development.md](docs/en/development.md) | repository layout, packages, kernel patches and config, debug tools |

## What this repository does not contain

- no Gigaset or DSPG binaries or firmware, and no headers derived from their sources;
- no per-device data (serial numbers, MAC addresses, DECT RFPI, RF calibration, EEPROM contents).

Everything device-specific is read from your own device. For example, the DECT base EEPROM is
copied from the Android data partition on first boot.

## Disclaimer

This is an unofficial community project, not affiliated with Gigaset. Use it at your own risk.
The procedure never writes to the eMMC. The only persistent change is the U-Boot environment in the
SPI NOR flash, which can be reverted (see [install.md](docs/en/install.md#going-back-to-android)).
