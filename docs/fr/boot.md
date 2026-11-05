# Chaîne de démarrage

*[English version](../en/boot.md)*

Comment un noyau mainline non signé démarre sur un appareil au secure boot fermé, et ce qu'il faut
savoir du bootloader d'origine.

## Bootloader d'origine

- U-Boot **2009.08** (build Gigaset `2009.08.maxwell.gc.014`, 2015) dans la **NOR SPI**, pas sur l'eMMC.
- Console sur l'UART2, 115200 8N1. `bootdelay` vaut 0 : appuyer sur une touche à l'allumage pour avoir
  l'invite.
- L'environnement stocké dans la NOR est invalide sur un appareil d'origine (`bad CRC, using default
  environment`) : les valeurs par défaut s'appliquent jusqu'au premier `saveenv`.
- `adb reboot bootloader` entre en fastboot (protocole 0.5, USB `18d1:0d02`) : seuls `getvar version`,
  `product` et `serialno` répondent, il n'y a ni verrouillage ni commande `oem`. `fastboot boot`
  fonctionne mais passe par HAB (ci-dessous). Ne jamais utiliser `fastboot flash` : il vise l'eMMC.

## Le secure boot (HAB) est fermé

```
HAB Configuration: 0xcc, HAB State: 0x99
```

`0xcc` = `HAB_CFG_CLOSED`, `0x99` = `HAB_STATE_TRUSTED` : les fusibles du secure boot sont grillés. La
ROM authentifie U-Boot, puis U-Boot authentifie le **noyau** de l'image de boot Android, au démarrage
normal comme en `fastboot boot`. Un noyau non signé est refusé :

```
Authenticate uImage from DDR location 0x14008000...
HAB Event 1: 0xdb 0x00 0x08 0x41 0x33 0x05 0x0a 0x00
Authentication Failed
```

(statut `0x33` HAB_FAILURE, raison `0x05` HAB_INV_IVT : pas d'IVT/CSF derrière le noyau). Seul le noyau
est signé : le ramdisk, le DTB (`second`) et la ligne de commande de l'image de boot ne le sont pas.

## Le contournement : `go`

La commande `go <adresse>` d'U-Boot saute à une adresse **sans aucune authentification**. Elle ne
transmet ni device tree, ni initrd, ni ligne de commande, et laisse le CPU dans l'état d'U-Boot. La
solution a trois parties :

1. **goshim** ([tools/goshim/goshim.S](../../tools/goshim/goshim.S)), un bout de code de 1 Kio,
   indépendant de sa position, placé devant le zImage. Il coupe caches, L2 et MMU, construit une liste
   d'ATAG avec `ATAG_INITRD2` à partir des deux arguments supplémentaires de `go`, et saute au zImage
   avec les registres qu'attend Linux.
2. Le **device tree est collé** derrière le zImage (`CONFIG_ARM_APPENDED_DTB`), et le décompresseur y
   fusionne les ATAG (`CONFIG_ARM_ATAG_DTB_COMPAT`) : la position de l'initrd arrive dans
   `/chosen/linux,initrd-*`. La ligne de commande est `/chosen/bootargs` du device tree.
3. Le paquet noyau construit `/boot/go.bin` = goshim + zImage + DTB.

```
ROM (HAB fermé)
 └─ U-Boot 2009.08 d'origine (NOR SPI, signé, intact)
     └─ bootcmd de l'environnement NOR (non signé)
         ├─ fatload mmc 1:1 0x12000000 go.bin       goshim + zImage + DTB collé
         ├─ fatload mmc 1:1 0x14000000 initramfs    produit par postmarketOS
         └─ go 0x12000000 0x14000000 ${filesize}
             └─ goshim : caches/MMU coupés, ATAG_INITRD2, saut au zImage
                 └─ Linux → initramfs postmarketOS → racine sur la microSD p2
```

### Plan mémoire

| Adresse | Contenu |
|---|---|
| `0x10000100` | ATAG écrits par goshim |
| `0x10008000` | noyau décompressé |
| `0x12000000` | `go.bin` : goshim (1 Kio), zImage à `0x12000400`, DTB derrière |
| `0x14000000` | initramfs |

## Environnement U-Boot

L'environnement réglé pendant l'[installation](install.md#4-démarrer-automatiquement-sur-la-carte) :

```
setenv android '<bootcmd d'origine>'
setenv sdgo 'go 0x12000000 0x14000000 ${filesize}'
setenv sdboot 'mmc dev 1 && fatload mmc 1:1 0x12000000 go.bin && fatload mmc 1:1 0x14000000 initramfs && run sdgo'
setenv bootcmd 'run sdboot; run android'
saveenv
```

- Le shell hush développe une ligne entière avant de l'exécuter : `${filesize}` doit être dans une
  variable à part (`sdgo`), développée seulement par `run`, sinon il vaudrait encore la taille de `go.bin`.
- Sans carte, ou sans `go.bin`/`initramfs`, `sdboot` échoue et `run android` démarre Android.
- Un noyau sans initramfs se lance avec `go 0x12000000` seul.

## Contraintes de la microSD

- **MBR seulement**, partition 1 d'un type FAT : U-Boot 2009.08 ignore le GPT. pmbootstrap crée une
  table GPT avec une partition 1 de type EFI, d'où la conversion de l'image par `install-sdcard.sh`
  (`sgdisk --gpttombr`, type `0x0C`).
- La partition de boot est en **FAT32** (`deviceinfo_boot_filesystem="fat32"`). `ext2load` se fige sur
  l'ext4 à extents : ne jamais l'utiliser.
- SDHC jusqu'à 32 Go ; les cartes UHS-I peuvent ne pas s'initialiser.
- Le contrôleur fonctionne en mode « high speed » 3,3 V (~20 Mo/s). La vitesse en accès aléatoire de
  la carte compte le plus pour la réactivité.

## Côté Android (pour mémoire)

- ADB : port mini-USB derrière le cache gauche du socle, options pour les développeurs → débogage USB.
  Build `user` avec clés de release : pas d'`adb root`.
- Image de boot d'origine (eMMC p1) : noyau à `0x10808000` suivi de son IVT et de sa CSF, ramdisk à
  `0x11800000`, DTB en `second` à `0x11700000`.
- Ligne de commande du noyau vendeur : `console=ttymxc1,115200 init=/init video=mxcfb0:dev=ldb,bpp=32 …
  androidboot.hardware=gigaset cma=384M`.
