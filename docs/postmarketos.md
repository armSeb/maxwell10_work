# postmarketOS (Nura) sur microSD

> Le projet postmarketOS a été renommé **Nura** en septembre 2026 (nura.eco). Les outils et noms
> techniques (`pmbootstrap`, `pmaports`, paquets `postmarketos-*`, labels `pmOS_boot`/`pmOS_root`)
> restent ceux de postmarketOS pendant la transition : ce document les utilise tels quels.

Objectif : un noyau **mainline** et **postmarketOS** sur la carte microSD, sans toucher à l'eMMC
(Android reste intact et redémarre dès qu'on retire la carte).

## Chaîne de démarrage

```
ROM i.MX6 (HAB fermé)
 └─ U-Boot 2009.08 vendeur, NOR SPI (signé — on n'y touche pas)
     └─ bootcmd (environnement NOR, non signé)
         ├─ fatload mmc 1:1 0x12000000 go.bin       goshim + zImage + DTB appendé
         ├─ fatload mmc 1:1 0x14000000 initramfs    généré par pmOS (mkinitfs)
         └─ go 0x12000000 0x14000000 ${filesize}
             └─ goshim : caches/MMU coupés, ATAG_INITRD2(0x14000000, taille), saut au zImage
                 └─ décompresseur Linux : DTB appendé + ATAGs fusionnés (linux,initrd-*)
                     └─ noyau → initramfs pmOS → rootfs pmOS_root (microSD p2)
```

Pourquoi ce montage :

- `bootm`/`fastboot boot` passent par HAB : noyau non signé refusé. `go` n'authentifie rien.
- `go` ne transmet ni DTB, ni initrd, ni ligne de commande : le DTB est **appendé** au zImage
  (`CONFIG_ARM_APPENDED_DTB`), la ligne de commande est `/chosen/bootargs` du DTB,
  et l'initrd passe par une liste d'**ATAGs** que goshim fabrique à partir des arguments de `go`
  (`CONFIG_ARM_ATAG_DTB_COMPAT` la fusionne dans le DTB). Détails : [tools/goshim/goshim.S](../tools/goshim/goshim.S).
- `ext2load` fige ce U-Boot (ext4 à extents) : la partition de boot pmOS est donc en **FAT32**
  (`deviceinfo_boot_filesystem="fat32"`), lue par `fatload`.

### Plan mémoire

| Adresse | Contenu |
|---|---|
| `0x10000100` | ATAGs écrits par goshim |
| `0x10008000` | noyau décompressé |
| `0x12000000` | `go.bin` : goshim (1 Kio) puis zImage à `0x12000400`, DTB appendé derrière |
| `0x14000000` | initramfs |

## Environnement U-Boot

À taper une fois dans la console UART2 (interrompre l'autoboot), puis `saveenv` :

```
setenv android '<bootcmd par défaut d'origine>'
setenv sdgo 'go 0x12000000 0x14000000 ${filesize}'
setenv sdboot 'mmc dev 1 && fatload mmc 1:1 0x12000000 go.bin && fatload mmc 1:1 0x14000000 initramfs && run sdgo'
setenv bootcmd 'run sdboot; run android'
setenv bootdelay 2
saveenv
```

- **Piège hush** : une ligne est développée en entier avant d'être exécutée. `${filesize}` écrit
  directement après le `fatload` de l'initramfs vaudrait encore la taille de `go.bin`.
  D'où la variable `sdgo`, développée seulement au moment du `run`.
- Les guillemets simples empêchent `setenv` de développer `${filesize}` à la saisie.
- Sans carte (ou sans `go.bin`/`initramfs`) : `run android`, boot Android d'origine.
- Sans initramfs (image de dump, tests de noyau) : `go 0x12000000` seul, comme avant.

## Carte microSD

U-Boot 2009.08 ne lit qu'une table **MBR** avec p1 d'un type FAT. pmbootstrap (constaté avec la v3.12.1)
crée une table **GPT** avec p1 typée partition système **EFI**. `install-sdcard.sh` fait donc produire une
image, la convertit en MBR (positions et contenu inchangés) avec p1 en `0x0C` (FAT32 LBA), puis l'écrit
sur la carte. Résultat :

| Partition | Label | FS | Taille | Contenu |
|---|---|---|---|---|
| p1 | `pmOS_boot` | FAT32 | 256 Mo | `go.bin`, `initramfs`, `vmlinuz`, `dtbs/` |
| p2 | `pmOS_root` | ext4 | reste de la carte (agrandie par `install-sdcard.sh` après écriture) | rootfs |

Interface (`UI=…` pour le script, défaut `phosh`) :

| UI | Pour | Contre |
|---|---|---|
| **`phosh`** (défaut) | interface tactile la plus aboutie de pmOS (GNOME mobile), clavier virtuel squeekboard, applis GNOME adaptatives, s'utilise au doigt sur 10" | la plus lourde des trois sur Cortex-A9 + GC2000 : fluide sans plus ; ~2 Go installée, carte de 8 Go ou plus |
| `xfce4` | bureau classique, léger, très configurable ; parfait avec clavier et souris sur les 2 ports USB | pensé pour la souris : petits éléments, pas de clavier virtuel par défaut |
| `cage` | kiosque : une seule appli (navigateur) en plein écran, le plus léger pour le dashboard Home Assistant | rien d'autre que cette appli |
| `buffyboard` | console + clavier tactile, pour le débogage | clavier invisible sur cet appareil (non résolu) |

**Cartes testées** : une 2 Go SDSC et une 16 Go SDHC fonctionnent ; une **Lexar 32 Go UHS-I** n'est pas
initialisée par U-Boot 2009.08 (`mmcinfo` : capacité 0, bloquée à 400 kHz) — préférer une carte sans UHS.

Affichage par KMS (imx-drm) et rendu GPU par etnaviv (Mesa) ; carte SDHC de 32 Go maximum
(U-Boot 2009.08 ne gère probablement pas le SDXC).

## Construire et installer

Voir [pmos/README.md](../pmos/README.md). Le plus simple, sur le PC :

```sh
pmbootstrap init                         # une fois
pmos/install-sdcard.sh /dev/sdX
```

Ou étape par étape :

```sh
pmbootstrap init                                    # device : gigaset-maxwell10 (après sync.sh)
pmos/sync.sh "$(pmbootstrap config work)/cache_git/pmaports"
pmbootstrap checksum linux-gigaset-maxwell10 device-gigaset-maxwell10
pmbootstrap install --sdcard=/dev/sdX               # mot de passe demandé
```

Puis la carte dans la tablette, et l'environnement U-Boot ci-dessus.
Accès : console série (`getty` sur `ttymxc1`), ou réseau USB par le mini-USB caché
(`ssh user@172.16.42.1`), ou l'écran.

## État des périphériques (DT `dts/imx6q-gigaset-maxwell10.dts`)

Premier boot réussi le 2026-11-04 (pmOS, UI buffyboard), puis Phosh (systemd) sur carte 16 Go.

Tout ce qui suit est **écrit d'après le DT vendeur, pas encore testé** sur le noyau mainline,
sauf mention contraire.

| Bloc | DT mainline | Remarques |
|---|---|---|
| UART2 console | ✅ testé (image de dump) | |
| eMMC (usdhc3) | ✅ testé (image de dump) | lecture seule par convention |
| microSD (usdhc4) | ✅ | `mmcblk3` sous mainline ; boot et rootfs pmOS |
| Écran LVDS 1280×800 | ✅ | console et écran de démarrage ; clavier buffyboard invisible (abandonné) |
| Rétroéclairage | 🧪 | PWM1, 50 µs |
| Tactile FT5x06 | ✅ | `edt_ft5x06` sur i2c-2 @ 0x38, saisie buffyboard OK |
| Bouton power | 🧪 | GPIO4_IO10 |
| Ethernet (FEC + 88E6320 DSA) | ✅ | switch 88E6320 rév. 2 (ID `0x1152`), ports `lan` et `pc`. Il restait muet sur le MDIO tant qu'on ne pulsait pas son reset (GPIO1_IO09) une fois l'horloge de la DVF99 présente : patch noyau `0001-net-dsa-mv88e6xxx-pulse-reset-before-detection` |
| USB OTG (gadget) | ✅ `usb0` en 172.16.42.1 |
| USB hôte (hub USB2512) | 🧪 | |
| Wi-Fi AR6003 (usdhc1) | ✅ | `wlan0` ; fw 3.2.0.144 api 3 avec `linux-firmware-ath6k` (fichiers `.zst`, d'où `CONFIG_FW_LOADER_COMPRESS_ZSTD`). Board file générique `bdata.SD31` : reprendre le `bdata.bin` d'Android pour la calibration (non publié) |
| NOR SPI | 🧪 | partitions en lecture seule |
| Ventilateur | ✅ | `gpio-fan` géré par le noyau : zone `imx_thermal_zone`, point actif à Tmax − 30 °C (65 °C en gamme commerciale), arrêt 10 °C plus bas — patch `0002-thermal-imx-add-an-active-trip-for-fans` |
| Watchdog | 🧪 | servi par le noyau |
| GPU (etnaviv), cpufreq | ✅ | accélération Phosh OK (GC2000), fréquence CPU variable |
| Capteur de lumière ISL29023 | ⏳ | bus à identifier |
| Bluetooth AR3002 | ⏳ | |
| Caméra OV9740 | ⏳ | pas de pilote mainline (soc_camera retiré) : `pipeline start failed with -32` si une appli l'ouvre. Détails : [hardware.md](hardware.md#caméra-ov9740) |
| HDMI | ⏳ | |
| DECT / audio DVF99 | 🔍 | **pas de son** pour l'instant : la DVF99 reste dans son U-Boot. UART3 activé, outil [tools/dvf99](../tools/dvf99/) pour la démarrer et décoder le CMBS. Feuille de route : [dect-cmbs.md](dect-cmbs.md#sous-linux-mainline--premiers-pas) |

## Optimisations

Intégrées aux paquets :

| Où | Quoi | Pourquoi |
|---|---|---|
| noyau | gouverneur **schedutil** par défaut, **HZ=250**, **préemption complète** | réactivité de l'interface |
| noyau | zram en **zstd** (et lz4) | swap compressé en RAM utilisable (2 Go de RAM) |
| paquet device | **BFQ** sur `mmcblk*` (règle udev) | l'interface reste fluide quand la carte SD travaille |
| noyau | ventilateur sur la zone thermique (point actif, patch 0002) | silence au repos, sans service en espace utilisateur |
| deviceinfo | `deviceinfo_gpu_accelerated="true"` | etnaviv fonctionne : pmOS propose les UI accélérées |

À faire soi-même, selon l'usage :

```sh
gsettings set org.gnome.desktop.interface enable-animations false   # gros gain perçu sur GC2000
gsettings set org.gnome.desktop.session idle-delay 0                # pas de mise en veille (dashboard)
gsettings set org.gnome.desktop.input-sources sources "[('xkb','fr')]"
```

Carte SD : la vitesse dépend surtout des accès aléatoires de la carte. Le contrôleur est limité au mode
3,3 V « high speed » (~20 Mo/s) ; prendre une carte de bonne marque **sans UHS-I** (une Lexar UHS-I n'est
pas lue par U-Boot 2009.08).

## Si ça ne démarre pas

| Symptôme sur UART2 | Piste |
|---|---|
| rien après `goshim: caches/MMU off` | DTB non trouvé derrière le zImage (`go.bin` mal assemblé), ou `CONFIG_ARM_APPENDED_DTB` absent |
| `fatload`/`fatls` ne voient pas p1, et le PC ne monte pas p1 tout seul | table **GPT** ou p1 typée **ESP (0xEF)** par pmbootstrap : U-Boot 2009.08 ne lit que le MBR, udisks ignore les ESP. `sudo blkid -p -o value -s PTTYPE /dev/sdX` ; si `gpt` : `sudo sgdisk --gpttombr=1:2 /dev/sdX` ; puis `sudo sfdisk --part-type /dev/sdX 1 c`. Le contenu est conservé. `install-sdcard.sh` le fait d'office |
| `fatload` échoue sur la carte pmOS mais marchait avant | p1 en FAT32 créée par pmbootstrap : le FAT32 d'U-Boot 2009.08 est fragile. Reformater p1 en FAT16 (`mkfs.vfat -F 16 -n pmOS_boot`) et recopier son contenu, puis `deviceinfo_boot_filesystem="fat16"` |
| `goshim: initrd -> ATAG_INITRD2` absent | `go` lancé sans les deux arguments, ou `${filesize}` vide (voir piège hush) |
| `Kernel panic … VFS: Unable to mount root fs` | initrd non vu : vérifier `linux,initrd-start` dans `/sys/firmware/devicetree/base/chosen` depuis l'image de dump ; `CONFIG_ARM_ATAG_DTB_COMPAT` |
| `Initramfs unpacking failed` | taille transmise fausse : relancer `fatload` puis `printenv filesize` à la main |
| initramfs pmOS : « pmOS_root introuvable » | microSD non vue par le noyau (usdhc4, CD) : `cat /proc/partitions` dans le shell de debug |
| écran noir, UART OK | LVDS : horloge ldb, mapping `vesa-24` vs `jeida-24`, ou rétroéclairage (`/sys/class/backlight`) |
