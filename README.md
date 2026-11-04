# maxwell10_work

Libération du **Gigaset Maxwell 10** (téléphone de bureau IP/DECT avec tablette Android 10,1")
pour en faire un terminal Linux tactile : dashboard Home Assistant, et gestion du combiné DECT.

Cible : **postmarketOS** (renommé **Nura** en 2026) sur noyau **mainline**, avec un démon maison pour la base DECT (DSPG DVF99).

## État

| Étape | État |
|---|---|
| Inventaire matériel (dmesg, DT vendeur) | ✅ [docs/hardware.md](docs/hardware.md) |
| Accès ADB (USB, port mini-USB caché) | ✅ |
| Console UART2 (U-Boot 2009.08 + noyau) | ✅ [docs/boot.md](docs/boot.md#console-série) |
| Fastboot U-Boot, `fastboot boot` en RAM | ⚠️ HAB fermé : noyau signé exigé — [docs/boot.md](docs/boot.md#secure-boot-hab--fermé) |
| Noyau mainline 6.12 + DT minimal pour dump | ✅ démarre via `go` + [tools/goshim](tools/goshim/) — [dump/](dump/), [dts/imx6q-maxwell-dump.dts](dts/imx6q-maxwell-dump.dts) |
| Boot automatique du noyau mainline depuis la microSD (`bootcmd` + `go`) | ✅ [docs/boot.md](docs/boot.md#boot-automatique-depuis-la-microsd-) |
| Chargement depuis `/data` sans root (FIEMAP + `mmc read`) | 🧪 alternative, [tools/fiemap2uboot](tools/fiemap2uboot/) |
| Dump eMMC / NOR | ⏳ |
| Root Android (boot.img patché) | ⏳ |
| Protocole DECT CMBS (DVF99) | 🔍 UART3 activé, outil [tools/dvf99](tools/dvf99/) — [docs/dect-cmbs.md](docs/dect-cmbs.md#sous-linux-mainline--premiers-pas) |
| Initramfs passé par `go` (goshim → ATAG_INITRD2) | 🧪 [docs/postmarketos.md](docs/postmarketos.md#chaîne-de-démarrage) |
| DT mainline complet (écran, tactile, SD, Ethernet DSA, Wi-Fi…) | 🧪 écrit, compile — [dts/imx6q-gigaset-maxwell10.dts](dts/imx6q-gigaset-maxwell10.dts), [état par bloc](docs/postmarketos.md#état-des-périphériques-dt-dtsimx6q-gigaset-maxwell10dts) |
| Port postmarketOS (microSD, boot FAT32) | 🧪 paquets prêts, à installer — [pmos/](pmos/), [docs/postmarketos.md](docs/postmarketos.md) |

## Matériel en bref

i.MX6 Quad (rev 1.5), 2 Go de RAM, eMMC 4 Go, NOR SPI 2 Mo (U-Boot), écran LVDS 1280×800,
tactile FocalTech FT5x06, switch Marvell 88E6320 (2 ports gigabit, PoE), Wi-Fi Atheros AR6003,
Bluetooth AR3002, caméra OV9740, base DECT + AFE audio **DSPG DVF9919**.

## Arborescence

```
docs/      notes de rétro-ingénierie, boot, postmarketOS
dts/       DT vendeur extrait (nettoyé), DT mainline de dump, DT mainline complet
dump/      init minimal + script PC pour dumper eMMC/NOR via mass storage USB
pmos/      paquets pmaports (noyau + device), script de synchronisation
tools/     goshim (lancer un zImage par `go`), fiemap2uboot, mkbootimg v0, dump RAM, scan MDIO (/dev/mem), dvf99 (console/boot/décodage CMBS), DWARF → header C
```

## Ce dépôt ne contient pas

- de binaires ou firmwares Gigaset / DSPG, ni de headers dérivés de leurs sources ;
- d'identifiants propres à un appareil (numéro de série, MAC, RFPI DECT, calibration RF).

Les outils permettent de régénérer localement ce qui est nécessaire à partir de son propre appareil.
