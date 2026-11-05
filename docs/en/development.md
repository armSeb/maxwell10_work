# Development

*[Version française](../fr/development.md)*

## Repository layout

```
dts/
  imx6q-gigaset-maxwell10.dts      mainline device tree (source of truth)
  vendor/imx6q-maxwell10-vendor.dts   vendor device tree, from /proc/device-tree (reference only)
pmos/
  install-sdcard.sh                build an image and write a card (see install.md)
  sync.sh                          copy the packages (and their external sources) into pmaports
  linux-gigaset-maxwell10/         kernel package: patches, config, config fragment
  device-gigaset-maxwell10/        device package: deviceinfo, services, udev rules, scripts
tools/
  dvf99/                           CMBS host and audio engine for the DVF9919
  goshim/                          stub that lets U-Boot's `go` boot a zImage with an initrd
  camera/ov9740-capture.sh         still capture through the imx-media pipeline
  debug/                           ssireg, memrw, mdio-scan (register access through /dev/mem), dwarf2h.py
docs/en, docs/fr                   documentation
```

## Packages

| Package | Content |
|---|---|
| `linux-gigaset-maxwell10` | mainline kernel, the device tree, `/boot/go.bin` (goshim + zImage + DTB) |
| `device-gigaset-maxwell10` | `deviceinfo`, `dvf99` and its services, Bluetooth and camera services, udev rules, PulseAudio glue |
| `device-gigaset-maxwell10-nonfree-firmware` | pulls `linux-firmware-ath6k` (Wi-Fi), `linux-firmware-ar3k` (Bluetooth), `linux-firmware-imx` (SDMA) |

Some package sources live elsewhere in the repository and are copied by `pmos/sync.sh` (the copies are
git-ignored). Edit them **in place**, then run `sync.sh`:

| Packaged file | Source of truth |
|---|---|
| `linux-gigaset-maxwell10/imx6q-gigaset-maxwell10.dts` | `dts/imx6q-gigaset-maxwell10.dts` |
| `linux-gigaset-maxwell10/goshim.S` | `tools/goshim/goshim.S` |
| `device-gigaset-maxwell10/dvf99.c` | `tools/dvf99/dvf99.c` |

Bump `pkgrel` of the package you change, then:

```sh
pmos/sync.sh "$(pmbootstrap config work)/cache_git/pmaports"
pmbootstrap checksum linux-gigaset-maxwell10 device-gigaset-maxwell10
pmbootstrap build linux-gigaset-maxwell10        # or device-gigaset-maxwell10
```

and install with `pmbootstrap sideload` (see [install.md](install.md#updating)).

### Device package contents

| File | Installed as | Role |
|---|---|---|
| `dvf99.service`, `dvf99-service` | system unit, `/usr/libexec/dvf99-service` | DECT base and audio engine; imports `nvram.bin` from Android on first boot |
| `dvf99-pulse.service`, `dvf99-pulse` | user unit, `/usr/libexec/dvf99-pulse` | PulseAudio pipe sinks/source in each user session |
| `dvf99-tmpfiles.conf` | `/usr/lib/tmpfiles.d/dvf99.conf` | `/run/dvf99`, mode 1777 |
| `61-dvf99-pulseaudio.rules` | udev | hide the raw ALSA card from PulseAudio |
| `maxwell-bluetooth.service`, `maxwell-bluetooth` | system unit | power-cycles the AR3002 (GPIO4_IO7), decompresses the `ar3k` firmware, runs `hciattach … ath3k 3000000 flow` |
| `62-maxwell-camera.rules`, `maxwell-camera.service`, `maxwell-camera` | udev + oneshot unit | camera media pipeline, `/dev/video-camera` |
| `maxwell-firmware-import.service`, `maxwell-firmware-import` | oneshot unit | copies `vpu_fw_imx6q.bin` and the Wi-Fi `bdata.bin` from Android's `/system` (eMMC p5, read-only) to `/lib/firmware/updates`, then reloads `coda` / `ath6kl_sdio` |
| `90_gigaset-maxwell10.gschema.override` | `/usr/share/glib-2.0/schemas` | GNOME/Phosh defaults: no screen blanking, no idle suspend, dimming kept (the login screen does not reliably wake up from blank) |
| `60-maxwell-ioscheduler.rules` | udev | BFQ I/O scheduler on `mmcblk*` |
| `80-gigaset-maxwell10*.preset` | systemd presets | enable the services |

## Kernel

`pkgver` follows kernel.org (`linux-$pkgver.tar.xz`), currently **6.12**.

| Patch | Purpose |
|---|---|
| `0001-net-dsa-mv88e6xxx-pulse-reset-before-detection` | the 88E6320 stays silent on MDIO unless its reset is pulsed after the DVF9919 clock is up |
| `0002-thermal-imx-add-an-active-trip-for-fans` | active trip point (Tmax − 30 °C, 10 °C hysteresis) so `gpio-fan` is driven by the thermal zone |
| `0003-ASoC-DVF9919-TDM-PCM-link` | dummy codec for the DVF9919 TDM link (8 kHz, 6 channels); keeps `fsl_ssi` in network mode |
| `0004-media-i2c-add-OmniVision-OV9740-driver` | OV9740 V4L2 subdevice driver |
| `0005-Bluetooth-hci_ath-skip-broken-LE-Read-Supported-States` | the AR3002 firmware answers this command with 5 bytes instead of 9, which made `HCIDEVUP` fail |

The configuration `config-gigaset-maxwell10.armv7` is `imx_v6_v7_defconfig` plus
[maxwell10.fragment](../../pmos/linux-gigaset-maxwell10/maxwell10.fragment) (boot through `go`,
postmarketOS and systemd requirements, drivers, schedutil, HZ=250, full preemption, zram with zstd).
To regenerate it after changing the fragment or the version:

```sh
cd linux        # kernel sources matching pkgver
make ARCH=arm imx_v6_v7_defconfig
scripts/kconfig/merge_config.sh -m -O . .config ../maxwell10_work/pmos/linux-gigaset-maxwell10/maxwell10.fragment
make ARCH=arm olddefconfig
cp .config ../maxwell10_work/pmos/linux-gigaset-maxwell10/config-gigaset-maxwell10.armv7
```

Then check that every fragment option kept its value: an unmet dependency drops it silently. Also run
`pmbootstrap kconfig check linux-gigaset-maxwell10`.

To refresh a patch, apply the series to a kernel git tree, edit, and regenerate the file with
`git diff` (keep the patch header).

## Debug tools

Static ARM binaries, built with `arm-linux-gnueabihf-gcc` by each `build.sh`, run as root on the tablet.

| Tool | Use |
|---|---|
| `tools/debug/ssireg` | decode and modify SSI2/AUDMUX registers, measure the TDM frame rate and sync pulse. **Only while the ALSA card is in use**: with the SSI clock off, an access hangs the bus |
| `tools/debug/memrw` | read/write one 32-bit register through `/dev/mem` (busybox has no `devmem`) |
| `tools/debug/mdio-scan` | scan the FEC MDIO bus directly, to see whether the switch answers |
| `tools/debug/dwarf2h.py` | export enums and structs of an ELF with DWARF to a C header (see [audio-dect.md](audio-dect.md#dvf99-reference)) |

## Rules for contributions

- Never commit vendor binaries, files extracted from them, or per-device data (EEPROM, calibration,
  RFPI, serial numbers, MAC addresses, logs that contain them). `.gitignore` covers the usual names.
- Keep `dts/` and `tools/` as the source of truth; `sync.sh` does the copies.
- Document every hardware finding in `docs/en` and `docs/fr`.
