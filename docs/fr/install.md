# Installation

*[English version](../en/install.md)*

Ce guide mène d'un Gigaset Maxwell 10 d'origine à postmarketOS démarré depuis une carte microSD.
Rien n'est écrit sur l'eMMC : Android continue de fonctionner et redémarre dès que la carte est absente.

1. [Prérequis](#prérequis)
2. [Construire et écrire la carte](#construire-et-écrire-la-carte)
3. [Console série](#console-série)
4. [Premier démarrage et réglage d'U-Boot](#premier-démarrage-et-réglage-du-boot)
5. [Après le premier démarrage](#après-le-premier-démarrage)
6. [Mettre à jour](#mettre-à-jour)
7. [Revenir à Android](#revenir-à-android)
8. [Dépannage](#dépannage)

## Prérequis

**Matériel**

- Un Gigaset Maxwell 10 (i.MX6 Quad). Testé sur une carte de révision matérielle 9.
- Une **carte microSD** SDHC (**32 Go maximum**), 8 Go ou plus pour l'interface Phosh. Le U-Boot
  d'origine (2009.08) est difficile : une SDSC de 2 Go et une SDHC de 16 Go fonctionnent, une Lexar
  32 Go **UHS-I** n'est pas détectée. Préférer une carte de bonne marque sans UHS-I.
- Un **adaptateur USB-série 3,3 V** (FTDI, CP2102, CH340…) et des fils fins, pour modifier une fois
  l'environnement d'U-Boot. Jamais d'adaptateur 5 V ni RS-232.

**Sur le PC** (distribution Linux récente, x86_64 ou arm64)

- [pmbootstrap](https://wiki.postmarketos.org/wiki/Pmbootstrap) (3.x), git, sudo ;
- `sgdisk` (paquet *gdisk*), `sfdisk`, `lsblk`, `findmnt`, `partx` (*util-linux*), `e2fsck` et
  `resize2fs` (*e2fsprogs*) ;
- un terminal série : `picocom`, `minicom` ou `screen` ;
- environ 15 Go d'espace libre. La première construction compile le noyau : de 30 minutes à quelques
  heures selon le PC.

## Construire et écrire la carte

```sh
git clone https://github.com/armSeb/maxwell10_work
cd maxwell10_work
pmbootstrap init
```

`pmbootstrap init` ne sert qu'une fois. À ce stade pmbootstrap ne connaît pas encore le Maxwell 10 :
choisir **n'importe quel** appareil et le canal **edge**, puis le nom d'utilisateur et le mot de passe
(ce seront ceux de la tablette). Répondre oui si la question des firmwares non libres est posée : le
Wi-Fi et le Bluetooth en ont besoin.

Insérer la carte dans le PC, trouver son nom avec `lsblk` (par exemple `/dev/sdb` ou `/dev/mmcblk0`),
puis :

```sh
pmos/install-sdcard.sh /dev/sdX                # interface Phosh (défaut)
UI=xfce4 pmos/install-sdcard.sh /dev/sdX       # ou une autre interface, voir plus bas
```

Le script :

1. vérifie que la cible est un disque entier, non monté, de moins de 64 Go, et demande de retaper son
   chemin pour confirmer ;
2. met à jour le clone pmaports et y copie les deux paquets du dépôt
   (`device/testing/linux-gigaset-maxwell10`, `device/testing/device-gigaset-maxwell10`) ;
3. choisit l'appareil et l'interface, et fait construire une **image** disque par `pmbootstrap install` ;
4. convertit l'image de GPT en **MBR**, partition 1 de type FAT32 LBA (`0x0C`) : le U-Boot d'origine ne
   lit rien d'autre ;
5. vérifie que la partition 1 contient `go.bin` et `initramfs`, les deux fichiers chargés par U-Boot ;
6. écrit l'image, puis agrandit la partition racine à toute la carte.

`REUSE_IMAGE=1` réécrit la dernière image construite sur une autre carte, sans reconstruire.

| Interface (`UI=`) | Remarques |
|---|---|
| `phosh` (défaut) | interface mobile GNOME : tactile, clavier virtuel. La plus lourde, suffisamment fluide sur ce matériel. Carte de 8 Go ou plus |
| `xfce4` | bureau classique, léger, idéal avec clavier et souris USB |
| `cage` | kiosque : une seule application en plein écran (par exemple un navigateur affichant un tableau de bord) |
| `console` | pas d'interface graphique |

## Console série

Le U-Boot d'origine est sur l'**UART2** (`ttymxc1`), **115200 8N1**, niveaux 3,3 V.

> **À COMPLÉTER** : emplacement des pastilles de l'UART2 sur la carte (photo). La masse peut être
> prise sur un blindage métallique ou sur la carcasse d'un connecteur USB.

Relier la masse, le TX de la carte au RX de l'adaptateur et le RX de la carte au TX de l'adaptateur
(une résistance de 1 kΩ en série sur cette ligne est une bonne précaution). Puis sur le PC :

```sh
picocom -b 115200 /dev/ttyUSB0
```

## Premier démarrage et réglage d'U-Boot

Le lecteur microSD est sous un cache du socle. Insérer la carte, ouvrir le terminal série, et allumer
le téléphone en **appuyant sur une touche à répétition** dans le terminal : le délai d'autoboot vaut 0,
la touche doit donc déjà être enfoncée quand U-Boot démarre. On obtient l'invite d'U-Boot.

### 1. Noter la commande de démarrage d'origine

```
printenv bootcmd
```

Recopier sa valeur : c'est la commande qui démarre Android, elle sert plus bas.

### 2. Essayer la carte sans rien modifier

Taper ces lignes une par une :

```
mmc dev 1
fatload mmc 1:1 0x12000000 go.bin
fatload mmc 1:1 0x14000000 initramfs
go 0x12000000 0x14000000 ${filesize}
```

La console série affiche `goshim: …` puis le démarrage de Linux, et l'écran le logo de postmarketOS
puis l'écran de connexion. Sinon, voir [Dépannage](#dépannage) : rien n'a encore été modifié, une
coupure de courant ramène Android.

### 3. Facultatif : sauvegarder la NOR SPI

L'étape suivante écrit l'environnement d'U-Boot dans la flash NOR SPI. Pour garder une copie de la
flash d'origine, une fois Linux démarré :

```sh
cat /proc/mtd                                  # partitions de la NOR, si elles sont exposées
sudo dd if=/dev/mtd0 of=nor-mtd0.bin           # un fichier par partition
```

et ranger les fichiers hors de la tablette. (Rien de secret dedans, mais ils sont propres à l'appareil.)

### 4. Démarrer automatiquement sur la carte

Couper le courant, interrompre de nouveau U-Boot, puis saisir (remplacer `<bootcmd d'origine>` par la
valeur notée à l'étape 1, en gardant les guillemets simples) :

```
setenv android '<bootcmd d'origine>'
setenv sdgo 'go 0x12000000 0x14000000 ${filesize}'
setenv sdboot 'mmc dev 1 && fatload mmc 1:1 0x12000000 go.bin && fatload mmc 1:1 0x14000000 initramfs && run sdgo'
setenv bootcmd 'run sdboot; run android'
setenv bootdelay 1
saveenv
```

Désormais le téléphone démarre postmarketOS quand la carte est présente, Android sinon. L'adaptateur
série n'est plus nécessaire.

Pourquoi la variable `sdgo` : U-Boot développe une ligne entière avant de l'exécuter, donc `${filesize}`
écrit sur la même ligne que le `fatload` de l'initramfs vaudrait encore la taille de `go.bin`. Les
guillemets simples empêchent `setenv` de le développer à la saisie. Détails dans [boot.md](boot.md).

## Après le premier démarrage

Se connecter avec l'utilisateur et le mot de passe choisis dans `pmbootstrap init`. Tout ce qui suit est
automatique ; voici comment le vérifier.

| Fonction | Vérification | Remarques |
|---|---|---|
| Réseau | `ip a` | ports Ethernet `lan` et `pc` ; Wi-Fi `wlan0` dans les paramètres ; réseau USB par le port mini-USB derrière le cache gauche du socle : `ssh <utilisateur>@172.16.42.1` |
| Base DECT et audio | `journalctl -u dvf99` | au premier démarrage l'EEPROM de la base est copiée depuis Android (`dvf99: EEPROM imported from Android`) vers `/var/lib/dvf99/nvram.bin`, avec une copie intacte `nvram.android.bin` |
| Son | paramètres du son | sorties **Haut-parleur** (défaut) et **Combiné** (combiné DECT), entrée **Micro du combiné** |
| Firmwares du constructeur | `journalctl -u maxwell-firmware-import` | au premier démarrage, le firmware du VPU et la calibration Wi-Fi de l'appareil sont copiés depuis la partition système d'Android vers `/lib/firmware/updates` (aucun paquet Alpine ne les fournit) |
| Bluetooth | `bluetoothctl show` | `maxwell-bluetooth.service` |
| Caméra | application Caméra, ou `gst-launch-1.0 v4l2src device=/dev/video-camera ! videoconvert ! waylandsink` | voir [camera.md](camera.md) |

Le **combiné DECT** reste appairé s'il l'était sous Android : l'appairage est dans l'EEPROM copiée
ci-dessus. Un appui sur sa touche verte ouvre le chemin audio (écouteur et micro du combiné disponibles
dans PulseAudio). Pour appairer de nouveau un combiné, voir
[audio-dect.md](audio-dect.md#appairer-un-combiné).

Les fichiers `nvram.bin` sont propres à l'appareil (identité DECT, appairages, calibration RF) : en
garder une copie hors de la tablette, ne jamais les publier.

Réglages utiles pour un tableau de bord mural (Phosh) :

```sh
gsettings set org.gnome.desktop.interface enable-animations false   # gros gain de fluidité perçue
gsettings set org.gnome.desktop.session idle-delay 0                # l'écran ne s'éteint jamais
```

## Mettre à jour

Les paquets se mettent à jour depuis le PC : on les reconstruit et on les pousse sur la tablette.

```sh
git pull
pmos/sync.sh "$(pmbootstrap config work)/cache_git/pmaports"
pmbootstrap checksum linux-gigaset-maxwell10 device-gigaset-maxwell10
pmbootstrap build linux-gigaset-maxwell10 device-gigaset-maxwell10
pmbootstrap sideload --host <IP de la tablette> --user <utilisateur> --arch armv7 \
	linux-gigaset-maxwell10 device-gigaset-maxwell10
```

puis redémarrer la tablette. Le paquet noyau réécrit `/boot/go.bin` et postmarketOS régénère
`/boot/initramfs`. Les autres paquets postmarketOS se mettent à jour normalement (`sudo apk upgrade`).

Après une mise à jour du noyau ou de `dvf99`, préférer une coupure de courant à un redémarrage à chaud :
la base DECT et la puce Bluetooth ne sont pas réinitialisées par un redémarrage à chaud.

## Revenir à Android

- **Temporairement** : éteindre et retirer la carte. `bootcmd` se rabat sur `run android`.
- **Définitivement** : dans U-Boot,

  ```
  setenv bootcmd '<bootcmd d'origine>'
  setenv android
  setenv sdgo
  setenv sdboot
  saveenv
  ```

## Dépannage

| Symptôme (console série) | Cause et solution |
|---|---|
| `fatload` / `fatls` ne voient aucune partition | la carte a une table GPT, ou la partition 1 est de type EFI (`0xEF`). Vérifier avec `sudo blkid -p -o value -s PTTYPE /dev/sdX` ; convertir avec `sudo sgdisk --gpttombr=1:2 /dev/sdX` puis `sudo sfdisk --part-type /dev/sdX 1 c`. `install-sdcard.sh` le fait déjà |
| `mmc dev 1` échoue, ou la capacité lue est 0 | carte non gérée par U-Boot 2009.08 (UHS-I, SDXC) : changer de carte |
| `fatload` échoue alors que la table est bonne | le FAT32 de ce U-Boot est fragile : reformater la partition 1 en FAT16 (`mkfs.vfat -F 16 -n pmOS_boot`), y recopier ses fichiers, et mettre `deviceinfo_boot_filesystem="fat16"` |
| rien après `goshim: caches/MMU off` | `go.bin` mal assemblé (pas de DTB derrière le zImage) |
| pas de ligne `goshim: initrd -> ATAG_INITRD2` | `go` lancé sans ses deux arguments, ou `${filesize}` vide (voir la note sur le développement ci-dessus) |
| `Initramfs unpacking failed` | mauvaise taille transmise : refaire le `fatload` de l'initramfs, vérifier `printenv filesize` |
| shell de l'initramfs : `pmOS_root` introuvable | le noyau ne voit pas la carte : `cat /proc/partitions` dans le shell de débogage |
| écran noir, console série vivante | affichage : vérifier `/sys/class/backlight` et `dmesg | grep -i -e ldb -e drm` |
| U-Boot se fige | `ext2load` a été utilisé : il se fige sur l'ext4 à extents. N'utiliser que le FAT |
| plus de son, ou le combiné ne réagit plus | couper le courant (pas un redémarrage à chaud) : la base DECT supporte mal les redémarrages à chaud. Voir [audio-dect.md](audio-dect.md#robustesse) |
| application Caméra : « impossible de diffuser le flux » | `systemctl status maxwell-camera` (mise en place de la chaîne) |

Ne jamais utiliser ces commandes U-Boot sans sauvegarde : `destroyenv`, `erase`, `protect`, `sf erase`,
`sf write`, `mmc write`, `fastboot flash`, et surtout toute écriture `imxotp` (fusibles, irréversible).
