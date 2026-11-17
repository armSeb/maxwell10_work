#!/bin/sh
# Extrait le ramdisk de l'image boot Android (eMMC) depuis postmarketOS, pour lire
# init.rc, les *.rc du constructeur, sepolicy, file_contexts… En root :
#
#   sudo tools/android/android-ramdisk.sh      → /tmp/android-ramdisk/ et /tmp/android-ramdisk.tar.gz
#
# Cherche les partitions qui commencent par « ANDROID! » (boot, recovery) et
# extrait le ramdisk de chacune dans un sous-dossier.
set -eu
[ "$(id -u)" = 0 ] || { echo "à lancer en root (sudo)" >&2; exit 1; }
out=/tmp/android-ramdisk
rm -rf "$out"; mkdir -p "$out"
u32() { od -An -tu4 -j"$2" -N4 "$1" | tr -d ' '; }
found=0
for p in /dev/mmcblk[0-9]p*; do
	[ -b "$p" ] || continue
	[ "$(dd if="$p" bs=8 count=1 2>/dev/null)" = "ANDROID!" ] || continue
	ksz=$(u32 "$p" 8); rsz=$(u32 "$p" 16); pg=$(u32 "$p" 36)
	roff=$(( pg * (1 + (ksz + pg - 1) / pg) ))
	name=$(basename "$p")
	echo "$p : image Android, noyau $ksz o, ramdisk $rsz o, page $pg"
	mkdir -p "$out/$name/root"
	dd if="$p" bs="$pg" skip=$(( roff / pg )) count=$(( (rsz + pg - 1) / pg )) 2>/dev/null | \
		head -c "$rsz" > "$out/$name/ramdisk.img"
	dd if="$p" bs=512 count=4 2>/dev/null | strings | grep -i -m3 -e cmdline -e console= > "$out/$name/cmdline.txt" || true
	( cd "$out/$name/root" && { gzip -dc ../ramdisk.img 2>/dev/null || lzop -dc ../ramdisk.img; } | cpio -id 2>/dev/null ) || \
		echo "  ramdisk non décompressé (format ?)"
	ls "$out/$name/root" | head -40 | tr '\n' ' '; echo
	found=1
done
[ "$found" = 1 ] || { echo "aucune image ANDROID! trouvée ; partitions :"; cat /proc/partitions; exit 1; }
tar -C /tmp -czf /tmp/android-ramdisk.tar.gz android-ramdisk
chmod 644 /tmp/android-ramdisk.tar.gz
echo "→ /tmp/android-ramdisk.tar.gz"
