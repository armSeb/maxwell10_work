# maxwell10_work

Libération du **Gigaset Maxwell 10** (téléphone de bureau IP/DECT avec tablette Android 10,1")
pour en faire un terminal Linux tactile : dashboard Home Assistant, et gestion du combiné DECT.

Cible : **postmarketOS** sur noyau **mainline**, avec un démon maison pour la base DECT (DSPG DVF99).

## État

| Étape | État |
|---|---|
| Inventaire matériel (dmesg, DT vendeur) | ✅ [docs/hardware.md](docs/hardware.md) |
| Accès ADB (USB, port mini-USB caché) | ✅ |
| Fastboot U-Boot NXP, `fastboot boot` en RAM accepté | ✅ [docs/boot.md](docs/boot.md) |
| Noyau mainline 6.12 + DT minimal pour dump | ✅ [dump/](dump/), [dts/imx6q-maxwell-dump.dts](dts/imx6q-maxwell-dump.dts) |
| Dump eMMC / NOR | ⏳ |
| Root Android (boot.img patché) | ⏳ |
| Protocole DECT CMBS (DVF99) | 🔍 en cours — [docs/dect-cmbs.md](docs/dect-cmbs.md) |
| DT mainline complet (écran, tactile, Ethernet DSA, Wi-Fi…) | ⏳ |
| Port postmarketOS (rootfs sur carte SD) | ⏳ |

## Matériel en bref

i.MX6 Quad (rev 1.5), 2 Go de RAM, eMMC 4 Go, NOR SPI 2 Mo (U-Boot), écran LVDS 1280×800,
tactile FocalTech FT5x06, switch Marvell 88E6320 (2 ports gigabit, PoE), Wi-Fi Atheros AR6003,
Bluetooth AR3002, caméra OV9740, base DECT + AFE audio **DSPG DVF9919**.

## Arborescence

```
docs/      notes de rétro-ingénierie
dts/       DT vendeur extrait (nettoyé) et DT mainline
dump/      init minimal + script PC pour dumper eMMC/NOR via mass storage USB
tools/     mkbootimg v0 minimal, extracteur de types DWARF → header C
```

## Ce dépôt ne contient pas

- de binaires ou firmwares Gigaset / DSPG, ni de headers dérivés de leurs sources ;
- d'identifiants propres à un appareil (numéro de série, MAC, RFPI DECT, calibration RF).

Les outils permettent de régénérer localement ce qui est nécessaire à partir de son propre appareil.
