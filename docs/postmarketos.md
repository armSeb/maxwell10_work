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

## Carte microSD (2 Go)

U-Boot 2009.08 ne lit qu'une table **MBR** avec p1 d'un type FAT. pmbootstrap (constaté avec la v3.12.1)
crée une table **GPT** avec p1 typée partition système **EFI** : `install-sdcard.sh` convertit ensuite en MBR
(positions et contenu inchangés) et type p1 en `0x0C` (FAT32 LBA). Résultat :

| Partition | Label | FS | Taille | Contenu |
|---|---|---|---|---|
| p1 | `pmOS_boot` | FAT32 | 256 Mo | `go.bin`, `initramfs`, `vmlinuz`, `dtbs/` |
| p2 | `pmOS_root` | ext4 | reste (~1,7 Go) | rootfs |

Interface conseillée :

- **`buffyboard`** pour le premier démarrage : console texte avec clavier tactile à l'écran.
  Léger, et suffisant pour valider écran, tactile, réseau et Wi-Fi. Tout reste accessible par
  l'UART et le SSH même si l'écran ou le tactile ne marchent pas encore.
- **`cage`** ensuite, pour le dashboard : compositeur Wayland kiosque qui affiche une seule
  application en plein écran. Choisir un navigateur léger : le Cortex-A9 et le GC2000 (etnaviv)
  peinent avec Firefox et un dashboard Home Assistant chargé.
- Phosh, Plasma Mobile, Sxmo : trop lourds pour 2 Go de carte, et peu utiles ici.

2 Go suffisent pour l'interface `console` (quelques centaines de Mo) ou un kiosque léger
(cage + navigateur, pour le dashboard Home Assistant). Phosh ou Plasma Mobile ne tiendront pas :
prévoir une carte plus grande pour une interface complète.

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

Premier boot réussi le 2026-11-04 (pmOS, UI buffyboard).

Tout ce qui suit est **écrit d'après le DT vendeur, pas encore testé** sur le noyau mainline,
sauf mention contraire.

| Bloc | DT mainline | Remarques |
|---|---|---|
| UART2 console | ✅ testé (image de dump) | |
| eMMC (usdhc3) | ✅ testé (image de dump) | lecture seule par convention |
| microSD (usdhc4) | ✅ | `mmcblk3` sous mainline ; boot et rootfs pmOS |
| Écran LVDS 1280×800 | ✅ | écran de démarrage affiché ; clavier buffyboard invisible (en cours) |
| Rétroéclairage | 🧪 | PWM1, 50 µs |
| Tactile FT5x06 | ✅ | `edt_ft5x06` sur i2c-2 @ 0x38, saisie buffyboard OK |
| Bouton power | 🧪 | GPIO4_IO10 |
| Ethernet (FEC + 88E6320 DSA) | ⚠️ | FEC OK (lien fixe 1 Gb/s) ; switch : nœud MDIO présent mais `bind` → ENODEV (ID non reconnu), même DVF99 hors reset. Diagnostic : [tools/mdio-scan](../tools/mdio-scan/) |
| USB OTG (gadget) | ✅ `usb0` en 172.16.42.1 |
| USB hôte (hub USB2512) | 🧪 | |
| Wi-Fi AR6003 (usdhc1) | ✅ | `wlan0` ; fw 3.2.0.144 api 3 avec `linux-firmware-ath6k` (fichiers `.zst`, d'où `CONFIG_FW_LOADER_COMPRESS_ZSTD`). Board file générique `bdata.SD31` : reprendre le `bdata.bin` d'Android pour la calibration (non publié) |
| NOR SPI | 🧪 | partitions en lecture seule |
| Ventilateur | ✅ | tourne à fond (gpio-hog) en attendant la régulation thermique |
| Watchdog | 🧪 | servi par le noyau |
| GPU (etnaviv), VPU | ⏳ | modules activés, non décrits au-delà de imx6q.dtsi |
| Capteur de lumière ISL29023 | ⏳ | bus à identifier |
| Bluetooth AR3002 | ⏳ | |
| Caméra OV9740 | ⏳ | |
| HDMI | ⏳ | |
| DECT / audio DVF99 | ⏳ | reset GPIO4_IO22 (actif bas) tenu **relâché** par un gpio-hog : la DVF99 fournit l'horloge du switch. Ne jamais le tenir actif |

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
