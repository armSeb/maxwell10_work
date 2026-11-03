# Matériel

Source : dmesg du noyau vendeur (3.14.52, build 2018, firmware Android 5.1.1) et DT extrait
de `/proc/device-tree` ([dts/imx6q-maxwell10-vendor.dts](../dts/imx6q-maxwell10-vendor.dts)).
Carte observée : `max10`, révision matérielle 9.

## SoC et mémoire

| | |
|---|---|
| SoC | NXP i.MX6 **Quad**, silicium rev 1.5 (`Revision: 63015`, Cortex-A9 r2p10) |
| RAM | 2 Go @ `0x10000000` (CMA vendeur : 384 Mo) |
| eMMC | 3,68 Gio (`SEM04G`) sur **usdhc3** (`0x2198000`), 8 bits, `mmcblk2` |
| NOR SPI | Winbond **W25Q16** (2 Mo) sur **ecspi4**, CS = GPIO3_IO20 — U-Boot (1 Mo) + jffs2 `vendor_gigaset` (1 Mo) |
| Carte SD | lecteur **microSD** sous un cache, **usdhc4** (`0x219c000`), 4 bits, CD = GPIO2_IO6, WP = GPIO2_IO7 ; `mmc 1` dans U-Boot |

Secure boot HAB **fermé** (`HAB_CFG_CLOSED`, état `TRUSTED`) : U-Boot authentifie le noyau du boot.img
(voir [boot.md](boot.md#secure-boot-hab--fermé)). Le noyau vendeur réserve un crash log en RAM
(`crash_log_resource` à `0x3ff40000`, `0x3ff80000`, `0x3ffc0000`, nœud `gc,panic-handler`).

## Périphériques

| Bloc | Composant | Bus / adresse | Driver mainline |
|---|---|---|---|
| Écran | LVDS 1280×800 (`lp101wx1`, 66,6 MHz), canal LDB 0, IPU2-DI0 | — | imx-ldb + panel |
| Rétroéclairage | PWM1 (`0x2080000`), 50 µs | — | pwm-backlight |
| Tactile | FocalTech FT5x06 | i2c3 @ 0x38, IRQ GPIO1_IO2, reset GPIO2_IO19 | edt-ft5x06 |
| Capteur de lumière | Intersil ISL29023 | I2C | isl29018 |
| HDMI | dw-hdmi, DDC sur i2c2 | — | dw_hdmi-imx |
| Ethernet | FEC RGMII en fixed-link 1 Gb/s → **Marvell 88E6320** (DSA), ports `lan` (3), `pc` (4), CPU (5), reset GPIO1_IO9 | MDIO | mv88e6xxx |
| Wi-Fi | Atheros **AR6003** hw 2.1.1 | SDIO usdhc1, reset GPIO1_IO8 | ath6kl_sdio |
| Bluetooth | Atheros **AR3002** | UART + rfkill GPIO4_IO7 | hci_uart (ath3k) |
| Audio BT (SCO) | AR3002 ↔ SSI2 | AUDMUX int 7 ↔ ext 5 | — |
| **DECT + audio** | **DSPG DVF9919** | UART3 (`ttymxc2`), TDM ↔ SSI1, AUDMUX int 2 ↔ ext 4, reset GPIO4_IO22 | à écrire (voir [dect-cmbs.md](dect-cmbs.md)) |
| Caméra | OmniVision OV9740 MIPI CSI-2 1 voie | i2c1 @ 0x10, PWDN GPIO5_IO24, reset GPIO5_IO23, LED GPIO1_IO5 | à voir (driver soc_camera retiré) |
| VPU | CODA960 | — | coda |
| GPU | Vivante GC2000 / GC320 / GC355 | — | etnaviv |
| USB host | hub SMSC USB2512 sur usbh1 (2 ports A) | — | ci_hdrc |
| USB OTG | port mini-USB derrière le cache gauche du socle (ADB / fastboot) | usbotg | ci_hdrc (gadget) |
| Ventilateur | GPIO4_IO23 | — | gpio-fan |
| Détection PoE | GPIO4_IO24 | — | gpio-keys / gpio |
| Bouton power | GPIO4_IO10 (actif bas) | — | gpio-keys |
| Watchdog | WDOG1 | — | imx2_wdt |

## UARTs

| UART | Adresse | Linux | Usage |
|---|---|---|---|
| UART1 | `0x2020000` | ttymxc0 | ? |
| UART2 | `0x21e8000` | ttymxc1 | **console** 115200 (U-Boot + noyau) |
| UART3 | `0x21ec000` | ttymxc2 | **DVF99** (console U-Boot du module puis CMBS), 115200 8N1, sans contrôle de flux |
| UART5 | `0x21f4000` | ttymxc4 | RTS/CTS — probablement Bluetooth AR3002 |

## Piège : horloge du switch

Sur les cartes de révision ≤ 11, **le quartz du switch Marvell est fourni par la DVF99**.
Un reset de la DVF99 coupe brièvement le switch : il faut relancer les PHY (`lan`, `pc`) ensuite
(workaround Gigaset « AFM-307 », présent aussi dans leur driver DSA). Le GPIO de reset de la DVF99
ne doit donc jamais être tenu à l'état actif.

## Partitions eMMC (vendeur)

| Partition | Point de montage |
|---|---|
| p4 | `/data` |
| p5 | `/system` |
| p6 | `/cache` |
| p7 | `/device` |
| p10 | `/fotatemp` |

Les autres (boot, recovery, misc…) seront identifiées sur le dump.
