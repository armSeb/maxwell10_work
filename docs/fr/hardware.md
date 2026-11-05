# Matériel

*[English version](../en/hardware.md)*

Sources : dmesg du noyau vendeur (3.14.52, Android 5.1.1), DT vendeur lu dans `/proc/device-tree`
([dts/vendor/imx6q-maxwell10-vendor.dts](../../dts/vendor/imx6q-maxwell10-vendor.dts)) et essais sous
mainline. Carte observée : `max10`, révision matérielle 9.

## SoC, mémoire, stockage

| | |
|---|---|
| SoC | NXP i.MX6 **Quad**, silicium rév. 1.5, 4 × Cortex-A9 |
| RAM | 2 Go à `0x10000000` |
| eMMC | 4 Go (`SEM04G`) sur **usdhc3**, 8 bits : `mmcblk2` sous mainline |
| NOR SPI | Winbond W25Q16 (2 Mo) sur **ecspi4**, CS = GPIO3_IO20 : U-Boot (1 Mo) + jffs2 `vendor_gigaset` (1 Mo) |
| microSD | lecteur sous un cache, **usdhc4**, 4 bits, CD = GPIO2_IO6, WP = GPIO2_IO7 : `mmc 1` dans U-Boot, `mmcblk3` sous mainline |
| Secure boot | HAB **fermé** : U-Boot authentifie le noyau de l'image de boot (voir [boot.md](boot.md)) |

### Partitions de l'eMMC (Android)

| Partition | Contenu |
|---|---|
| p1, p2 | boot, recovery (8 Mo chacune) |
| p4 | `/data` (contient `cmbs/nvram.bin`, l'EEPROM de la base DECT) |
| p5 | `/system` |
| p6 | `/cache` |
| p7 | `/device` |
| p10 | `/fotatemp` |

## Périphériques et état sous mainline

| Bloc | Composant | Bus / broches | Pilote mainline | État |
|---|---|---|---|---|
| Écran | dalle LVDS 1280×800, canal LDB 0, IPU2-DI0 | — | imx-ldb + panel-simple | ✅ |
| Rétroéclairage | PWM1, période 50 µs | — | pwm-backlight | ✅ allumé, réglage de luminosité 🧪 |
| Tactile | FocalTech FT5x06 | i2c3 @ 0x38, IRQ GPIO1_IO2, reset GPIO2_IO19 | edt-ft5x06 | ✅ |
| Ethernet | FEC RGMII fixed-link 1 Gb/s → switch **Marvell 88E6320** : ports `lan` (3), `pc` (4), CPU (5) | MDIO, reset GPIO1_IO9 | mv88e6xxx (DSA), patch 0001 | ✅ |
| Wi-Fi | Atheros **AR6003** hw 2.1.1 | SDIO usdhc1, reset GPIO1_IO8 | ath6kl_sdio + `linux-firmware-ath6k` | ✅ |
| Bluetooth | Atheros **AR3002** | UART5 (`ttymxc4`, RTS/CTS, pads DCE), alimentation GPIO4_IO7 (haut = marche) | hci_uart ath3k par `hciattach`, patch 0005 | ✅ |
| DECT + audio | **DSPG DVF9919** | UART3 (`ttymxc2`), TDM ↔ SSI2 par l'AUDMUX (port interne 2 ↔ port externe 4), reset GPIO4_IO22 | démon `dvf99` + patch ASoC 0003 | ✅ (voir [audio-dect.md](audio-dect.md)) |
| Caméra | OmniVision **OV9740**, MIPI CSI-2 1 voie | i2c1 @ 0x10, PWDN GPIO5_IO24, reset GPIO5_IO23, LED GPIO1_IO5 (active basse) | ov9740 (patch 0004) + imx-media | ✅ (voir [camera.md](camera.md)) |
| GPU | Vivante GC2000 / GC320 / GC355 | — | etnaviv (OpenGL ES 2.0) | ✅ |
| VPU | CODA960 | — | coda | 🧪 |
| Ventilateur | GPIO4_IO23 | — | gpio-fan sur la zone thermique, patch 0002 | ✅ |
| USB OTG | mini-USB derrière le cache gauche du socle | usbotg | ci_hdrc (gadget) | ✅ |
| USB hôte | hub SMSC USB2512, 2 × type A | usbh1 | ci_hdrc | 🧪 |
| Bouton power | GPIO4_IO10 (actif bas) | — | gpio-keys | 🧪 |
| Watchdog | WDOG1 | — | imx2_wdt | 🧪 |
| HDMI | dw-hdmi, DDC sur i2c2 | — | dw_hdmi-imx | ⏳ |
| Capteur de lumière | Intersil ISL29023 | I²C (bus non identifié) | isl29018 | ⏳ |
| Détection PoE | GPIO4_IO24 | — | — | ⏳ |
| Audio Bluetooth SCO | AR3002 ↔ SSI3, AUDMUX int 7 ↔ ext 5 | — | — | ⏳ |

Il n'y a **aucun codec audio I²C** : i2c1 porte la caméra, i2c2 le DDC HDMI, i2c3 le tactile. Tout
l'audio analogique (haut-parleur, micros, combiné) passe par la DVF9919.

## UART

| UART | Adresse | Linux | Usage |
|---|---|---|---|
| UART1 | `0x2020000` | ttymxc0 | inutilisé |
| UART2 | `0x21e8000` | ttymxc1 | **console** (U-Boot et Linux), 115200 8N1 |
| UART3 | `0x21ec000` | ttymxc2 | **DVF9919** : sa console U-Boot, puis le protocole CMBS ; 115200 8N1, sans contrôle de flux |
| UART5 | `0x21f4000` | ttymxc4 | **Bluetooth** AR3002, RTS/CTS |

## Piège : l'horloge du switch vient de la DVF9919

Sur les cartes jusqu'à la révision 11, l'horloge du switch Marvell est **fournie par la DVF9919**.
Conséquences :

- la ligne de reset de la DVF9919 (GPIO4_IO22, active basse) ne doit jamais être tenue active : le DT la
  maintient relâchée par un hog GPIO. Un reset de la DVF9919 arrête brièvement le switch, et Android
  relance alors les PHY (contournement Gigaset « AFM-307 ») ;
- le switch doit recevoir une **impulsion de reset** (GPIO1_IO09) une fois l'horloge de la DVF9919
  présente. Relâché trop tôt, il reste muet sur le MDIO (tout à `0xffff`). Le mv88e6xxx mainline ne
  pulse le reset qu'après une détection réussie, d'où le patch 0001 qui le pulse avant.

## Device tree

[dts/imx6q-gigaset-maxwell10.dts](../../dts/imx6q-gigaset-maxwell10.dts) est le DT mainline, écrit
d'après celui du vendeur et vérifié bloc par bloc. La ligne de commande du noyau est son
`/chosen/bootargs` (`console=ttymxc1,115200 console=tty0 cma=256M`), puisque `go` n'en transmet aucune.
