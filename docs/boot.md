# Boot, ADB, fastboot

## ADB

- Port **mini-USB** derrière le cache gauche du socle (doc Gigaset teamwork wiki).
- Options développeur → débogage USB. Enumère en `18d1:0d02` (valeurs par défaut du BSP Android NXP).
- ADB Wi-Fi impossible sans passer d'abord par l'USB : `adb tcpip 5555` (non persistant sans root).
- Build `user`, `release-keys`, `ro.debuggable=0` : pas d'`adb root`, pas de sideload signé AOSP.

## Bootloader

- U-Boot Freescale **2009.08** (build Gigaset `2009.08.maxwell.gc.014`, nov. 2015) **sur la NOR SPI** (W25Q16, 1er Mo), pas sur l'eMMC.
- Environnement NOR invalide (`bad CRC, using default environment`) : ne **pas** lancer `saveenv` avant d'avoir dumpé la NOR.
- `bootdelay` à 0.
- Console sur **UART2** (`ttymxc1`) 115200 8N1.
- `adb reboot bootloader` → fastboot, également en `18d1:0d02`
  (Manufacturer `Freescale`, Product `i.mx6q maxwell`, numéro de série par défaut du BSP).
- Fastboot **protocole 0.5** : seules `version`, `product`, `serialno` répondent à `getvar`,
  pas de commandes `oem`, **aucune notion de verrouillage**.
- `fastboot boot <boot.img>` télécharge et lance l'image, **mais le noyau passe par l'authentification HAB** :
  un zImage non signé est refusé (voir ci-dessous). Le fastboot annonce aussi `flash target is MMC:0` : ne pas utiliser `fastboot flash`.

## Secure boot (HAB) : fermé

Relevé sur la console UART2 :

```
HAB Configuration: 0xcc, HAB State: 0x99
```

`0xcc` = `HAB_CFG_CLOSED`, `0x99` = `HAB_STATE_TRUSTED` : les fusibles de secure boot sont grillés.
La ROM authentifie U-Boot, puis U-Boot authentifie **le noyau** du boot.img (`Authenticate uImage from DDR location …`),
au boot normal comme en `fastboot boot`. Conséquences :

- noyau mainline non signé refusé ; mode Serial Download (`uuu`) limité aux images signées ; JTAG probablement verrouillé ;
- **seul le noyau est authentifié** d'après les traces : ramdisk et ligne de commande du boot.img sont pris tels quels.
  Piste : noyau Gigaset signé, intact, + ramdisk et cmdline à nous.

Échec observé avec l'image de dump mainline :

```
Authenticate uImage from DDR location 0x14008000...
--------- HAB Event 1 -----------------
event data:
        0xdb 0x00 0x08 0x41 0x33 0x05 0x0a 0x00
Authentication Failed
```

Décodage : statut `0x33` `HAB_FAILURE`, raison `0x05` `HAB_INV_IVT`, contexte `0x0a` `HAB_CTX_AUTHENTICATE` → pas d'IVT/CSF derrière le noyau.

Adresses du boot.img vendeur (boot normal) : noyau @ `0x10808000`, ramdisk @ `0x11800000`, soit base `0x10800000`.
En `fastboot boot`, U-Boot respecte les adresses de l'en-tête. L'IVT d'un noyau signé contient sa propre adresse :
pour réutiliser le noyau vendeur, garder **exactement** `kernel_addr = 0x10808000`.

### boot.img vendeur (eMMC p1)

| Champ | Taille | Adresse |
|---|---|---|
| kernel | `0x609000` | `0x10808000` |
| ramdisk | `0xe141c` | `0x11800000` |
| second (**DTB**) | `0x9d23` | `0x11700000` |
| tags | — | `0x10800100` |

Le noyau est un zImage complété à `0x606000`, suivi de l'IVT puis de la CSF :

| IVT (`+0x606000`) | Valeur |
|---|---|
| header | `0x402000d1` |
| entry | `0x10808000` |
| self | `0x10e0e000` (= `0x10808000 + 0x606000`) |
| csf | `0x10e0f000` (CSF dans les `0x2000` derniers octets) |

La signature couvre la zone du noyau à son adresse de chargement : DTB (`second`), ramdisk et cmdline ne sont pas signés.
Un boot.img qui garde le noyau octet pour octet à `0x10808000` passe donc HAB avec un ramdisk et un DTB modifiés.

Partitions eMMC (MBR) : p1 boot, p2 recovery (8 Mo chacune), p4 `/data`, p5 `/system`, p6 `/cache`, p7 `/device`,
p10 `/fotatemp`, p11/p13 FAT16, p12 à identifier. `mmcblk2boot0/1` sont vides.

## Contournement : commande `go`

La console U-Boot est accessible (taper une touche au reset). `go <addr>` saute à une adresse **sans
authentification HAB** : on charge un binaire en RAM (`loady`, `tftpboot`, `fatload`) et on l'exécute.
Pour un zImage, [tools/goshim](../tools/goshim/) remet le CPU dans l'état attendu par Linux
(caches, L2, MMU, registres) avant de sauter.
Avec deux arguments de plus (`go <addr> <adresse initrd> <taille>`), goshim passe aussi un
initramfs par ATAG_INITRD2 : c'est ce qu'utilise postmarketOS, voir [postmarketos.md](postmarketos.md).
Le zImage commence à l'offset `0x400` de `go.bin` (cale de 1 Kio).

### Boot automatique depuis la microSD ✅

Un lecteur **microSD** (usdhc4) se trouve sous un cache. U-Boot le voit en `mmc 1` et lit le FAT.
Carte : MBR, p1 FAT contenant `go.bin` (goshim + zImage + DTB, cf. [tools/goshim](../tools/goshim/)),
p2 ext4 réservée au rootfs.

Environnement U-Boot (une fois, `saveenv`) — l'eMMC n'est pas modifiée :

```
setenv android '<bootcmd par défaut d'origine>'
setenv bootcmd 'mmc dev 1 && fatload mmc 1:1 0x12000000 go.bin && go 0x12000000; run android'
saveenv
```

Carte absente ou `go.bin` introuvable → retour à Android.
Pour postmarketOS (initramfs en plus), l'environnement complet est dans [postmarketos.md](postmarketos.md#environnement-u-boot). Retirer la carte suffit pour revenir au boot d'origine.

`ext2load` (ext4 à extents) fige ce U-Boot : ne pas l'utiliser.

### Charger le zImage depuis l'eMMC, déposé par Android (sans root)

Évite la console série pour le transfert (`loady` à 115200 : ~10 min pour 6 Mo) et le TFTP.

- `ext2load` d'U-Boot 2009.08 ne sait pas lire l'ext4 à extents : inutilisable sur `/data`.
- Mais l'utilisateur `shell` (adb) peut écrire dans `/data/local/tmp` et obtenir l'emplacement physique
  de **son** fichier par l'ioctl `FIEMAP` (non privilégié, contrairement à `FIBMAP`).
- Côté U-Boot, `mmc read` lit des secteurs bruts : lecture seule, sans risque.

[tools/fiemap2uboot](../tools/fiemap2uboot/) fait le calcul (début de la partition lu dans
`/sys/dev/block/M:m/start`, extents, CRC32) et imprime les commandes U-Boot :

```sh
sh tools/goshim/build.sh zImage-dtb go.bin
sh tools/fiemap2uboot/build.sh
adb push go.bin tools/fiemap2uboot/fiemap2uboot /data/local/tmp/
adb shell /data/local/tmp/fiemap2uboot -i /data/local/tmp/go.bin /data/local/tmp/go.raw
```

`-i` recopie le fichier en le préallouant (`fallocate`) pour qu'il soit contigu : une seule ligne `mmc read`.
Prérequis : `/data` **non chiffré** (`getprop ro.crypto.state` → `unencrypted` ; l'outil refuse un device-mapper).

Puis dans U-Boot :

1. **numéro de périphérique MMC** : l'eMMC est sur usdhc3, son index U-Boot n'est pas connu d'avance
   (`mmcinfo`, `help mmc` pour la syntaxe). La première commande générée lit le superbloc de la partition :
   `md.w` doit afficher `ef53`. Sinon, changer `-d` ;
2. `mmc read …` ; 3. `crc32 …` doit donner la valeur annoncée ; 4. `go`.

Ne pas supprimer ni réécrire `go.raw` entre la génération des commandes et le boot : ses blocs changeraient.

Commandes à ne **jamais** utiliser sans sauvegarde : `saveenv`, `destroyenv`, `erase`, `protect`,
`sf erase`/`sf write`, `mmc write`, `fastboot flash`, et surtout `imxotp` en écriture (fusibles, irréversible).

## Console série

UART2 (`ttymxc1`) 115200 8N1, accessible sur des pads de la carte (TX côté i.MX : niveau 3,3 V au repos).
Écoute seule recommandée (GND + RX de l'adaptateur), adaptateur 3,3 V, résistance série sur le RX.

### Paramètres boot.img utilisés

| | |
|---|---|
| Format | en-tête Android v0, page 2048 |
| base | `0x14000000` (valeur des BSP i.MX6 NXP) |
| kernel | base + `0x8000` |
| ramdisk | base + `0x1000000` |
| second | base + `0xf00000` |
| tags | base + `0x100` |

Le DTB est **appendé au zImage** (`CONFIG_ARM_APPENDED_DTB`) pour ne pas dépendre de la façon dont
ce U-Boot gère les DTB. [tools/mkbootimg_v0.py](../tools/mkbootimg_v0.py) génère l'image.

Ligne de commande du noyau vendeur (pour référence) :

```
console=ttymxc1,115200 init=/init video=mxcfb0:dev=ldb,bpp=32 video=mxcfb1:dev=hdmi,bpp=32
video=mxcfb2:off video=mxcfb3:off vmalloc=256M androidboot.console=ttymxc1 consoleblank=0
androidboot.hardware=gigaset cma=384M androidboot.selinux=enforcing androidboot.dm_verity=disabled
```

## Image de dump

[dump/](../dump/) : noyau mainline 6.12 (`imx_v6_v7_defconfig` allégé, config fournie),
DT [dts/imx6q-maxwell-dump.dts](../dts/imx6q-maxwell-dump.dts), initramfs = un unique `init` statique.

Une fois booté via `fastboot boot`, la tablette se présente en USB (`1d6b:0104`) comme :

- 4 disques **en lecture seule** : `mmcblk2`, `mmcblk2boot0`, `mmcblk2boot1`, `mtdblock0` (NOR complète) ;
- un port série ACM qui recopie le dmesg en continu.

Côté PC : `sudo dump/dump-maxwell.sh`.

### Reconstruire

```sh
git clone --depth 1 --branch v6.12 https://github.com/torvalds/linux.git
cp dts/imx6q-maxwell-dump.dts linux/arch/arm/boot/dts/nxp/imx/
echo 'dtb-$(CONFIG_SOC_IMX6Q) += imx6q-maxwell-dump.dtb' >> linux/arch/arm/boot/dts/nxp/imx/Makefile
arm-linux-gnueabihf-gcc -static -Os -o dump/init dump/init.c
# éditer CONFIG_INITRAMFS_SOURCE dans la config pour pointer vers une liste contenant /init et /dev/console
cp dump/kernel-6.12-dump.config linux/.config
make -C linux ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- olddefconfig zImage nxp/imx/imx6q-maxwell-dump.dtb
cat linux/arch/arm/boot/zImage linux/arch/arm/boot/dts/nxp/imx/imx6q-maxwell-dump.dtb > zImage-dtb
python3 tools/mkbootimg_v0.py --kernel zImage-dtb --ramdisk empty.cpio.gz -o maxwell-dump-boot.img
fastboot boot maxwell-dump-boot.img
```
