# Boot, ADB, fastboot

## ADB

- Port **mini-USB** derrière le cache gauche du socle (doc Gigaset teamwork wiki).
- Options développeur → débogage USB. Enumère en `18d1:0d02` (valeurs par défaut du BSP Android NXP).
- ADB Wi-Fi impossible sans passer d'abord par l'USB : `adb tcpip 5555` (non persistant sans root).
- Build `user`, `release-keys`, `ro.debuggable=0` : pas d'`adb root`, pas de sideload signé AOSP.

## Bootloader

- U-Boot NXP (~2014) **sur la NOR SPI** (W25Q16, 1er Mo), pas sur l'eMMC.
- Console sur **UART2** (`ttymxc1`) 115200 8N1.
- `adb reboot bootloader` → fastboot, également en `18d1:0d02`
  (Manufacturer `Freescale`, Product `i.mx6q maxwell`, numéro de série par défaut du BSP).
- Fastboot **protocole 0.5** : seules `version`, `product`, `serialno` répondent à `getvar`,
  pas de commandes `oem`, **aucune notion de verrouillage**.
- **`fastboot boot <boot.img>` fonctionne** : U-Boot accepte un boot.img v0 non signé et saute dans le noyau.

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
