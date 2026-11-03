# Paquets postmarketOS

| Paquet | Contenu |
|---|---|
| [linux-gigaset-maxwell10](linux-gigaset-maxwell10/) | noyau mainline 6.12, DT [imx6q-gigaset-maxwell10](../dts/imx6q-gigaset-maxwell10.dts), `/boot/go.bin` (goshim + zImage + DTB) |
| [device-gigaset-maxwell10](device-gigaset-maxwell10/) | `deviceinfo` (boot FAT32, pas de flash) ; sous-paquet `-nonfree-firmware` pour le Wi-Fi |

Fonctionnement du boot et environnement U-Boot : [docs/postmarketos.md](../docs/postmarketos.md).

## Fichiers et leur source

| Fichier du paquet noyau | Source de vérité | Mis à jour par |
|---|---|---|
| `imx6q-gigaset-maxwell10.dts` | [dts/](../dts/imx6q-gigaset-maxwell10.dts) | `sync.sh` |
| `goshim.S` | [tools/goshim/](../tools/goshim/goshim.S) | `sync.sh` |
| `config-gigaset-maxwell10.armv7` | `imx_v6_v7_defconfig` + [maxwell10.fragment](linux-gigaset-maxwell10/maxwell10.fragment) | à la main, voir ci-dessous |

Modifier le DT ou goshim **à leur place d'origine**, puis relancer `sync.sh`.

## Installer sur la carte (sur le PC)

### Script (recommandé)

```sh
pmbootstrap init                         # une fois, n'importe quel appareil : crée config + clone pmaports
pmos/install-sdcard.sh /dev/sdX          # UI buffyboard par défaut ; UI=cage pour un kiosque
```

[install-sdcard.sh](install-sdcard.sh) vérifie la carte (disque entier, non monté, < 64 Go,
confirmation en retapant le chemin), met à jour pmaports, copie les paquets, calcule les sommes,
règle `device` et `ui`, lance `pmbootstrap install --sdcard`, contrôle que p1 contient
`go.bin` et `initramfs`, puis affiche l'environnement U-Boot à saisir.

### À la main


```sh
pmbootstrap init                 # choisir n'importe quel appareil pour l'instant, canal edge
pmos/sync.sh "$(pmbootstrap config work)/cache_git/pmaports"
pmbootstrap checksum linux-gigaset-maxwell10 device-gigaset-maxwell10
pmbootstrap init                 # vendor : gigaset, device : maxwell10 ; UI : console (ou none)
pmbootstrap kconfig check linux-gigaset-maxwell10   # signale les options pmOS manquantes
pmbootstrap install --sdcard=/dev/sdX
```

`sync.sh` copie les deux paquets dans `pmaports/device/testing/`. Le premier `pmbootstrap init`
sert seulement à obtenir le checkout pmaports ; le second sélectionne l'appareil.

Carte de 2 Go : interface `console`, ou `none` pour construire un kiosque à la main.
pmbootstrap crée p1 `pmOS_boot` (FAT32) et p2 `pmOS_root` (ext4) en MBR.

Après installation, `/boot` (p1) doit contenir au moins `go.bin` et `initramfs` : ce sont les deux
fichiers lus par U-Boot.

## Régénérer la config noyau

Après une modification de `maxwell10.fragment`, ou pour changer de version :

```sh
cd linux        # sources mainline de la version de pkgver
make ARCH=arm imx_v6_v7_defconfig
scripts/kconfig/merge_config.sh -m -O . .config ../maxwell10_work/pmos/linux-gigaset-maxwell10/maxwell10.fragment
make ARCH=arm olddefconfig
cp .config ../maxwell10_work/pmos/linux-gigaset-maxwell10/config-gigaset-maxwell10.armv7
```

Vérifier ensuite que chaque option du fragment a bien la valeur voulue : une dépendance
non satisfaite la fait disparaître sans erreur (ex. : `NET_DSA` limité à `m` tant que `BRIDGE=m`,
`DM_CRYPT` absent sans `MD=y`).

Ou, dans pmbootstrap : `pmbootstrap kconfig edit linux-gigaset-maxwell10`.

## Mettre à jour le noyau

`pkgver` suit les sources de kernel.org (`linux-$pkgver.tar.xz`). Changer `pkgver`, régénérer la
config, `pmbootstrap checksum`, `pmbootstrap build linux-gigaset-maxwell10`, puis sur la tablette
`apk upgrade` : le paquet réécrit `/boot/go.bin`, pmOS régénère `/boot/initramfs`.
Le chemin `arch/arm/boot/dts/nxp/imx/` vaut depuis Linux 6.5.
