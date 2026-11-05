# Hardware

*[Version française](../fr/hardware.md)*

Sources: dmesg of the vendor kernel (3.14.52, Android 5.1.1), the vendor device tree read from
`/proc/device-tree` ([dts/vendor/imx6q-maxwell10-vendor.dts](../../dts/vendor/imx6q-maxwell10-vendor.dts)),
and tests under mainline. Board observed: `max10`, hardware revision 9.

## SoC, memory, storage

| | |
|---|---|
| SoC | NXP i.MX6 **Quad**, silicon rev 1.5, 4 × Cortex-A9 |
| RAM | 2 GB at `0x10000000` |
| eMMC | 4 GB (`SEM04G`) on **usdhc3**, 8 bit: `mmcblk2` under mainline |
| SPI NOR | Winbond W25Q16 (2 MB) on **ecspi4**, CS = GPIO3_IO20: U-Boot (1 MB) + jffs2 `vendor_gigaset` (1 MB) |
| microSD | slot under a cover, **usdhc4**, 4 bit, CD = GPIO2_IO6, WP = GPIO2_IO7: `mmc 1` in U-Boot, `mmcblk3` under mainline |
| Secure boot | HAB **closed**: U-Boot authenticates the kernel of the boot image (see [boot.md](boot.md)) |

### eMMC partitions (Android)

| Partition | Content |
|---|---|
| p1, p2 | boot, recovery (8 MB each) |
| p4 | `/data` (holds `cmbs/nvram.bin`, the DECT base EEPROM) |
| p5 | `/system` |
| p6 | `/cache` |
| p7 | `/device` |
| p10 | `/fotatemp` |

## Peripherals and mainline status

| Block | Part | Bus / pins | Mainline driver | State |
|---|---|---|---|---|
| Display | LVDS 1280×800 panel, LDB channel 0, IPU2-DI0 | — | imx-ldb + panel-simple | ✅ |
| Backlight | PWM1, 50 µs period | — | pwm-backlight | ✅ on, brightness control 🧪 |
| Touch | FocalTech FT5x06 | i2c3 @ 0x38, IRQ GPIO1_IO2, reset GPIO2_IO19 | edt-ft5x06 | ✅ |
| Ethernet | FEC RGMII fixed-link 1 Gb/s → **Marvell 88E6320** switch: ports `lan` (3), `pc` (4), CPU (5) | MDIO, reset GPIO1_IO9 | mv88e6xxx (DSA), patch 0001 | ✅ |
| Wi-Fi | Atheros **AR6003** hw 2.1.1 | SDIO usdhc1, reset GPIO1_IO8 | ath6kl_sdio + `linux-firmware-ath6k` | ✅ |
| Bluetooth | Atheros **AR3002** | UART5 (`ttymxc4`, RTS/CTS, DCE pads), power GPIO4_IO7 (high = on) | hci_uart ath3k via `hciattach`, patch 0005 | ✅ |
| DECT + audio | **DSPG DVF9919** | UART3 (`ttymxc2`), TDM ↔ SSI2 through AUDMUX (int port 2 ↔ ext port 4), reset GPIO4_IO22 | `dvf99` daemon + ASoC patch 0003 | ✅ (see [audio-dect.md](audio-dect.md)) |
| Camera | OmniVision **OV9740**, MIPI CSI-2 1 lane | i2c1 @ 0x10, PWDN GPIO5_IO24, reset GPIO5_IO23, LED GPIO1_IO5 (active low) | ov9740 (patch 0004) + imx-media | ✅ (see [camera.md](camera.md)) |
| GPU | Vivante GC2000 / GC320 / GC355 | — | etnaviv (OpenGL ES 2.0) | ✅ |
| VPU | CODA960 | — | coda | 🧪 |
| Fan | GPIO4_IO23 | — | gpio-fan on the thermal zone, patch 0002 | ✅ |
| USB OTG | mini-USB behind the left cover of the base | usbotg | ci_hdrc (gadget) | ✅ |
| USB host | SMSC USB2512 hub, 2 × type A | usbh1 | ci_hdrc | 🧪 |
| Power button | GPIO4_IO10 (active low) | — | gpio-keys | 🧪 |
| Watchdog | WDOG1 | — | imx2_wdt | 🧪 |
| HDMI | dw-hdmi, DDC on i2c2 | — | dw_hdmi-imx | ⏳ |
| Light sensor | Intersil ISL29023 | I²C (bus not identified) | isl29018 | ⏳ |
| PoE detection | GPIO4_IO24 | — | — | ⏳ |
| Bluetooth SCO audio | AR3002 ↔ SSI3, AUDMUX int 7 ↔ ext 5 | — | — | ⏳ |

There is **no I²C audio codec**: i2c1 carries the camera, i2c2 the HDMI DDC, i2c3 the touch screen.
All analog audio (speaker, microphones, handset) goes through the DVF9919.

## UARTs

| UART | Address | Linux | Use |
|---|---|---|---|
| UART1 | `0x2020000` | ttymxc0 | unused |
| UART2 | `0x21e8000` | ttymxc1 | **console** (U-Boot and Linux), 115200 8N1 |
| UART3 | `0x21ec000` | ttymxc2 | **DVF9919**: its U-Boot console, then the CMBS protocol; 115200 8N1, no flow control |
| UART5 | `0x21f4000` | ttymxc4 | **Bluetooth** AR3002, RTS/CTS |

## Pitfall: the switch clock comes from the DVF9919

On boards up to revision 11, the crystal clock of the Marvell switch is **supplied by the DVF9919**.
Consequences:

- the DVF9919 reset line (GPIO4_IO22, active low) must never be held active: the device tree keeps it
  released with a GPIO hog. Resetting the DVF9919 briefly stops the switch, and Android then restarts
  the PHYs (Gigaset workaround "AFM-307");
- the switch must get a **reset pulse** (GPIO1_IO09) once the DVF9919 clock is present. Released too
  early it stays silent on MDIO (all `0xffff`). Mainline mv88e6xxx only pulses the reset after a
  successful detection, hence patch 0001 which pulses it before.

## Device tree

[dts/imx6q-gigaset-maxwell10.dts](../../dts/imx6q-gigaset-maxwell10.dts) is the mainline device tree,
written from the vendor one and verified block by block. The kernel command line is its
`/chosen/bootargs` (`console=ttymxc1,115200 console=tty0 cma=256M`), since `go` passes none.
