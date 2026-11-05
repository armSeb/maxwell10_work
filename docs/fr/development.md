# Développement

*[English version](../en/development.md)*

## Organisation du dépôt

```
dts/
  imx6q-gigaset-maxwell10.dts      device tree mainline (source de vérité)
  vendor/imx6q-maxwell10-vendor.dts   device tree vendeur, tiré de /proc/device-tree (référence seulement)
pmos/
  install-sdcard.sh                construit une image et écrit une carte (voir install.md)
  sync.sh                          copie les paquets (et leurs sources externes) dans pmaports
  linux-gigaset-maxwell10/         paquet noyau : patches, config, fragment de config
  device-gigaset-maxwell10/        paquet de l'appareil : deviceinfo, services, règles udev, scripts
tools/
  dvf99/                           hôte CMBS et moteur audio de la DVF9919
  goshim/                          cale qui permet au `go` d'U-Boot de lancer un zImage avec initrd
  camera/ov9740-capture.sh         capture d'image fixe par la chaîne imx-media
  debug/                           ssireg, memrw, mdio-scan (accès aux registres par /dev/mem), dwarf2h.py
docs/en, docs/fr                   documentation
```

## Paquets

| Paquet | Contenu |
|---|---|
| `linux-gigaset-maxwell10` | noyau mainline, le device tree, `/boot/go.bin` (goshim + zImage + DTB) |
| `device-gigaset-maxwell10` | `deviceinfo`, `dvf99` et ses services, services Bluetooth et caméra, règles udev, raccordement PulseAudio |
| `device-gigaset-maxwell10-nonfree-firmware` | tire `linux-firmware-ath6k` (Wi-Fi), `linux-firmware-ar3k` (Bluetooth), `linux-firmware-imx` (SDMA) |

Certaines sources des paquets sont ailleurs dans le dépôt et sont copiées par `pmos/sync.sh` (les copies
sont ignorées par git). Les modifier **à leur place d'origine**, puis lancer `sync.sh` :

| Fichier du paquet | Source de vérité |
|---|---|
| `linux-gigaset-maxwell10/imx6q-gigaset-maxwell10.dts` | `dts/imx6q-gigaset-maxwell10.dts` |
| `linux-gigaset-maxwell10/goshim.S` | `tools/goshim/goshim.S` |
| `device-gigaset-maxwell10/dvf99.c` | `tools/dvf99/dvf99.c` |

Incrémenter le `pkgrel` du paquet modifié, puis :

```sh
pmos/sync.sh "$(pmbootstrap config work)/cache_git/pmaports"
pmbootstrap checksum linux-gigaset-maxwell10 device-gigaset-maxwell10
pmbootstrap build linux-gigaset-maxwell10        # ou device-gigaset-maxwell10
```

et installer avec `pmbootstrap sideload` (voir [install.md](install.md#mettre-à-jour)).

### Contenu du paquet de l'appareil

| Fichier | Installé comme | Rôle |
|---|---|---|
| `dvf99.service`, `dvf99-service` | unité système, `/usr/libexec/dvf99-service` | base DECT et moteur audio ; importe `nvram.bin` d'Android au premier démarrage |
| `dvf99-pulse.service`, `dvf99-pulse` | unité utilisateur, `/usr/libexec/dvf99-pulse` | sorties/entrée PulseAudio par tubes dans chaque session utilisateur |
| `dvf99-tmpfiles.conf` | `/usr/lib/tmpfiles.d/dvf99.conf` | `/run/dvf99`, mode 1777 |
| `61-dvf99-pulseaudio.rules` | udev | cache à PulseAudio la carte ALSA brute |
| `maxwell-bluetooth.service`, `maxwell-bluetooth` | unité système | coupe et rallume l'AR3002 (GPIO4_IO7), décompresse le firmware `ar3k`, lance `hciattach … ath3k 3000000 flow` |
| `62-maxwell-camera.rules`, `maxwell-camera.service`, `maxwell-camera` | udev + unité oneshot | chaîne media de la caméra, `/dev/video-camera` |
| `60-maxwell-ioscheduler.rules` | udev | ordonnanceur d'E/S BFQ sur `mmcblk*` |
| `80-gigaset-maxwell10*.preset` | presets systemd | activent les services |

## Noyau

`pkgver` suit kernel.org (`linux-$pkgver.tar.xz`), actuellement **6.12**.

| Patch | Rôle |
|---|---|
| `0001-net-dsa-mv88e6xxx-pulse-reset-before-detection` | le 88E6320 reste muet sur le MDIO si son reset n'est pas pulsé une fois l'horloge de la DVF9919 présente |
| `0002-thermal-imx-add-an-active-trip-for-fans` | point de déclenchement actif (Tmax − 30 °C, hystérésis 10 °C) pour que `gpio-fan` suive la zone thermique |
| `0003-ASoC-DVF9919-TDM-PCM-link` | codec factice pour la liaison TDM de la DVF9919 (8 kHz, 6 voies) ; garde `fsl_ssi` en mode réseau |
| `0004-media-i2c-add-OmniVision-OV9740-driver` | pilote de sous-périphérique V4L2 de l'OV9740 |
| `0005-Bluetooth-hci_ath-skip-broken-LE-Read-Supported-States` | le firmware de l'AR3002 répond à cette commande avec 5 octets au lieu de 9, ce qui faisait échouer `HCIDEVUP` |

La configuration `config-gigaset-maxwell10.armv7` est `imx_v6_v7_defconfig` plus
[maxwell10.fragment](../../pmos/linux-gigaset-maxwell10/maxwell10.fragment) (démarrage par `go`,
exigences de postmarketOS et de systemd, pilotes, schedutil, HZ=250, préemption complète, zram en zstd).
Pour la régénérer après une modification du fragment ou de la version :

```sh
cd linux        # sources du noyau correspondant à pkgver
make ARCH=arm imx_v6_v7_defconfig
scripts/kconfig/merge_config.sh -m -O . .config ../maxwell10_work/pmos/linux-gigaset-maxwell10/maxwell10.fragment
make ARCH=arm olddefconfig
cp .config ../maxwell10_work/pmos/linux-gigaset-maxwell10/config-gigaset-maxwell10.armv7
```

Vérifier ensuite que chaque option du fragment a gardé sa valeur : une dépendance non satisfaite la fait
disparaître sans erreur. Lancer aussi `pmbootstrap kconfig check linux-gigaset-maxwell10`.

Pour retoucher un patch : appliquer la série sur un arbre git du noyau, modifier, et régénérer le fichier
avec `git diff` (en gardant l'en-tête du patch).

## Outils de mise au point

Binaires ARM statiques, construits avec `arm-linux-gnueabihf-gcc` par chaque `build.sh`, à lancer en root
sur la tablette.

| Outil | Usage |
|---|---|
| `tools/debug/ssireg` | décode et modifie les registres SSI2/AUDMUX, mesure la cadence des trames TDM et l'impulsion de synchro. **Uniquement pendant que la carte ALSA est utilisée** : horloge du SSI coupée, un accès fige le bus |
| `tools/debug/memrw` | lit/écrit un registre 32 bits par `/dev/mem` (busybox n'a pas `devmem`) |
| `tools/debug/mdio-scan` | parcourt directement le bus MDIO du FEC, pour voir si le switch répond |
| `tools/debug/dwarf2h.py` | exporte enums et structures d'un ELF avec DWARF vers un header C (voir [audio-dect.md](audio-dect.md#référence-de-dvf99)) |

## Règles pour contribuer

- Ne jamais commiter de binaires vendeur, de fichiers qui en sont extraits, ni de données propres à un
  appareil (EEPROM, calibration, RFPI, numéros de série, adresses MAC, journaux qui les contiennent).
  `.gitignore` couvre les noms habituels.
- Garder `dts/` et `tools/` comme sources de vérité ; `sync.sh` fait les copies.
- Documenter chaque découverte matérielle dans `docs/en` et `docs/fr`.
