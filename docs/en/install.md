# Installation

*[Version française](../fr/install.md)*

This guide takes you from a stock Gigaset Maxwell 10 to postmarketOS running from a microSD card.
Nothing is written to the eMMC: Android keeps working and boots again whenever the card is absent.

1. [Requirements](#requirements)
2. [Build and write the card](#build-and-write-the-card)
3. [Serial console](#serial-console)
4. [First boot and U-Boot setup](#first-boot-and-u-boot-setup)
5. [After the first boot](#after-the-first-boot)
6. [Updating](#updating)
7. [Going back to Android](#going-back-to-android)
8. [Troubleshooting](#troubleshooting)

## Requirements

**Hardware**

- A Gigaset Maxwell 10 (i.MX6 Quad). Tested on a hardware revision 9 board.
- A **microSD card**, SDHC (**32 GB maximum**), 8 GB or more for the Phosh interface. The vendor
  U-Boot (2009.08) is picky: a 2 GB SDSC and a 16 GB SDHC card work, a Lexar 32 GB **UHS-I** card is
  not detected. Prefer a good brand card without UHS-I.
- A **3.3 V USB-serial adapter** (FTDI, CP2102, CH340…) and thin wires, used once to change the
  U-Boot environment. Never use a 5 V or RS-232 adapter.

**On the PC** (any recent Linux distribution, x86_64 or arm64)

- [pmbootstrap](https://wiki.postmarketos.org/wiki/Pmbootstrap) (3.x), git, sudo;
- `sgdisk` (package *gdisk*), `sfdisk`, `lsblk`, `findmnt`, `partx` (*util-linux*), `e2fsck` and
  `resize2fs` (*e2fsprogs*);
- a serial terminal: `picocom`, `minicom` or `screen`;
- about 15 GB of free disk space. The first build compiles the kernel and takes from 30 minutes to a
  few hours depending on the PC.

## Build and write the card

```sh
git clone https://github.com/armSeb/maxwell10_work
cd maxwell10_work
pmbootstrap init
```

`pmbootstrap init` only has to run once. At this point the Maxwell 10 is not yet known to
pmbootstrap: pick **any** device and the **edge** channel, and choose your username and password
(they are the login of the tablet). Answer yes if you are asked about non-free firmware: Wi-Fi and
Bluetooth need it.

Insert the card in the PC, find its device name with `lsblk` (for example `/dev/sdb` or
`/dev/mmcblk0`), then:

```sh
pmos/install-sdcard.sh /dev/sdX                # Phosh interface (default)
UI=xfce4 pmos/install-sdcard.sh /dev/sdX       # or another interface, see below
```

The script:

1. checks that the target is a whole, unmounted disk smaller than 64 GB, and asks you to type its
   path again to confirm;
2. updates the pmaports clone and copies the two packages of this repository into it
   (`device/testing/linux-gigaset-maxwell10`, `device/testing/device-gigaset-maxwell10`);
3. selects the device and the interface, and builds a disk **image** with `pmbootstrap install`;
4. converts the image from GPT to **MBR** with partition 1 typed FAT32 LBA (`0x0C`): the vendor
   U-Boot reads nothing else;
5. checks that partition 1 contains `go.bin` and `initramfs`, the two files U-Boot loads;
6. writes the image, then grows the root partition to the whole card.

Rerun with `REUSE_IMAGE=1` to write the last built image to another card without rebuilding.

| Interface (`UI=`) | Notes |
|---|---|
| `phosh` (default) | GNOME mobile shell: touch friendly, on-screen keyboard. The heaviest option, smooth enough on this hardware. 8 GB card or more |
| `xfce4` | classic desktop, light, best with a USB keyboard and mouse |
| `cage` | kiosk: one full-screen application (for example a browser showing a dashboard) |
| `console` | no graphical interface |

## Serial console

The vendor U-Boot is reached on **UART2** (`ttymxc1`), **115200 8N1**, 3.3 V levels.

> **TODO**: location of the UART2 pads on the board (photo). Ground can be taken from any metal
> shield or from the USB connector shell.

Connect GND, the board TX to the adapter RX, and the board RX to the adapter TX (a 1 kΩ resistor in
series on that line is a good precaution). Then on the PC:

```sh
picocom -b 115200 /dev/ttyUSB0
```

## First boot and U-Boot setup

The microSD slot is under a cover of the base. Insert the card, open the serial terminal, and power
the phone on while **pressing a key repeatedly** in the terminal: the autoboot delay is 0, so the key
must already be pressed when U-Boot starts. You get the U-Boot prompt.

### 1. Note the original boot command

```
printenv bootcmd
```

Copy the value somewhere: it is the command that boots Android, needed below.

### 2. Try the card without changing anything

Type these lines one at a time:

```
mmc dev 1
fatload mmc 1:1 0x12000000 go.bin
fatload mmc 1:1 0x14000000 initramfs
go 0x12000000 0x14000000 ${filesize}
```

The serial console shows `goshim: …` then the Linux boot log, and the screen shows the postmarketOS
boot splash, then the login screen. If it does not, see [Troubleshooting](#troubleshooting): nothing
has been changed yet, a power cycle brings Android back.

### 3. Optional: back up the SPI NOR

The next step writes the U-Boot environment to the SPI NOR flash. To keep a copy of the flash as
shipped, once Linux runs:

```sh
cat /proc/mtd                                  # list the NOR partitions, if exposed
sudo dd if=/dev/mtd0 of=nor-mtd0.bin           # one file per partition
```

and store the files off the tablet. (They contain nothing secret, but they are specific to your unit.)

### 4. Make the card boot automatically

Power cycle, interrupt U-Boot again, then enter (replace `<original bootcmd>` with the value noted in
step 1, keeping the single quotes):

```
setenv android '<original bootcmd>'
setenv sdgo 'go 0x12000000 0x14000000 ${filesize}'
setenv sdboot 'mmc dev 1 && fatload mmc 1:1 0x12000000 go.bin && fatload mmc 1:1 0x14000000 initramfs && run sdgo'
setenv bootcmd 'run sdboot; run android'
setenv bootdelay 1
saveenv
```

From now on the phone boots postmarketOS when the card is present, and Android otherwise. The serial
adapter is no longer needed.

Why the extra `sdgo` variable: U-Boot expands a whole line before running it, so `${filesize}` written
on the same line as the `fatload` of the initramfs would still hold the size of `go.bin`. The single
quotes keep `setenv` from expanding it while you type. Details in [boot.md](boot.md).

## After the first boot

Log in with the username and password chosen in `pmbootstrap init`. Everything below is automatic;
this is how to check it.

| Function | Check | Notes |
|---|---|---|
| Network | `ip a` | Ethernet ports `lan` and `pc`; Wi-Fi `wlan0` in the settings; USB networking on the mini-USB port behind the left cover of the base: `ssh <user>@172.16.42.1` |
| DECT base and audio | `journalctl -u dvf99` | on first boot the base EEPROM is copied from Android (`dvf99: EEPROM imported from Android`) to `/var/lib/dvf99/nvram.bin`, with a pristine copy `nvram.android.bin` |
| Sound | sound settings | outputs **Haut-parleur** (speaker, default) and **Combiné** (DECT handset), input **Micro du combiné** (handset microphone) |
| Bluetooth | `bluetoothctl show` | `maxwell-bluetooth.service` |
| Camera | Camera application, or `gst-launch-1.0 v4l2src device=/dev/video-camera ! videoconvert ! waylandsink` | see [camera.md](camera.md) |

The **DECT handset** stays paired as long as it was paired under Android: the pairing lives in the
EEPROM copied above. Pressing its green key opens the audio path (handset earpiece and microphone
become available in PulseAudio). To pair a handset again, see
[audio-dect.md](audio-dect.md#pairing-a-handset).

The `nvram.bin` files are specific to your unit (DECT identity, pairings, RF calibration): keep a
copy off the tablet and never publish them.

Useful settings for a wall dashboard (Phosh):

```sh
gsettings set org.gnome.desktop.interface enable-animations false   # big perceived speed gain
gsettings set org.gnome.desktop.session idle-delay 0                # never blank the screen
```

## Updating

Packages are updated from the PC: rebuild them and push them to the tablet.

```sh
git pull
pmos/sync.sh "$(pmbootstrap config work)/cache_git/pmaports"
pmbootstrap checksum linux-gigaset-maxwell10 device-gigaset-maxwell10
pmbootstrap build linux-gigaset-maxwell10 device-gigaset-maxwell10
pmbootstrap sideload --host <tablet IP> --user <user> --arch armv7 \
	linux-gigaset-maxwell10 device-gigaset-maxwell10
```

then reboot the tablet. The kernel package rewrites `/boot/go.bin`, and postmarketOS regenerates
`/boot/initramfs`. Regular postmarketOS packages update as usual (`sudo apk upgrade`).

Prefer a full power cycle over a warm reboot after a kernel or `dvf99` update: the DECT base and the
Bluetooth chip are not reset by a warm reboot.

## Going back to Android

- **Temporarily**: power off and remove the card. `bootcmd` falls back to `run android`.
- **For good**: in U-Boot,

  ```
  setenv bootcmd '<original bootcmd>'
  setenv android
  setenv sdgo
  setenv sdboot
  saveenv
  ```

## Troubleshooting

| Symptom (serial console) | Cause and fix |
|---|---|
| `fatload` / `fatls` see no partition | the card has a GPT table, or partition 1 is typed EFI (`0xEF`). Check with `sudo blkid -p -o value -s PTTYPE /dev/sdX`; convert with `sudo sgdisk --gpttombr=1:2 /dev/sdX` then `sudo sfdisk --part-type /dev/sdX 1 c`. `install-sdcard.sh` already does it |
| `mmc dev 1` fails, or the card size reads 0 | card not supported by U-Boot 2009.08 (UHS-I, SDXC): use another card |
| `fatload` fails although the table is right | the FAT32 driver of this U-Boot is fragile: reformat partition 1 in FAT16 (`mkfs.vfat -F 16 -n pmOS_boot`), copy its files back, and set `deviceinfo_boot_filesystem="fat16"` |
| nothing after `goshim: caches/MMU off` | `go.bin` badly assembled (no DTB after the zImage) |
| no `goshim: initrd -> ATAG_INITRD2` line | `go` was run without its two extra arguments, or `${filesize}` was empty (see the expansion note above) |
| `Initramfs unpacking failed` | wrong size passed: run the `fatload` of the initramfs again, check `printenv filesize` |
| initramfs shell: `pmOS_root` not found | the kernel does not see the card: `cat /proc/partitions` in the debug shell |
| black screen, serial console alive | display problem: check `/sys/class/backlight` and `dmesg | grep -i -e ldb -e drm` |
| U-Boot hangs | `ext2load` was used: it hangs on ext4 with extents. Use FAT only |
| no sound, or the handset no longer reacts | power cycle (not a warm reboot): the DECT base does not like warm restarts. See [audio-dect.md](audio-dect.md#robustness) |
| Camera application: "unable to stream" | `systemctl status maxwell-camera` (pipeline setup) |

Never run these U-Boot commands without a backup: `destroyenv`, `erase`, `protect`, `sf erase`,
`sf write`, `mmc write`, `fastboot flash`, and above all any `imxotp` write (fuses, irreversible).
