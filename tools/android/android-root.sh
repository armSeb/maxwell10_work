#!/bin/sh
# Dépose su, busybox et strace dans la partition système de l'Android d'origine
# (eMMC), depuis postmarketOS. À lancer en root sur la tablette :
#
#   sudo tools/android/android-root.sh            # installe
#   sudo tools/android/android-root.sh --remove   # retire ce qui a été installé
#
# Ce qui est posé dans /system/xbin d'Android :
#   su        setuid root, réservé à root et à l'utilisateur shell d'adb (su.c)
#   busybox   busybox statique d'Alpine (busybox-static)
#   strace    enveloppe qui lance le strace d'Alpine avec son chargeur musl,
#             copiés dans /system/xbin/musl
# Les fichiers reçoivent le label SELinux u:object_r:system_file:s0. Si Android
# est en SELinux « enforcing », su donnera un uid 0 resté dans le domaine shell :
# vérifier avec `getenforce` sous Android.
set -eu

die() { echo "android-root : $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "à lancer en root (sudo)"
here=$(cd "$(dirname "$0")" && pwd)
mnt=/mnt/android-system
files="su busybox strace musl"

mkdir -p "$mnt"
mountpoint -q "$mnt" && umount "$mnt"

# la partition système : ext4 de l'eMMC contenant build.prop
part=
for p in /dev/mmcblk[0-9]p*; do
	[ -b "$p" ] || continue
	mount -o ro "$p" "$mnt" 2>/dev/null || continue
	if [ -f "$mnt/build.prop" ] && [ -d "$mnt/bin" ]; then part=$p; break; fi
	umount "$mnt"
done
[ -n "$part" ] || die "partition système d'Android introuvable"
echo "partition système : $part ($(grep -m1 '^ro.build.display.id=' "$mnt/build.prop" | cut -d= -f2))"
mount -o remount,rw "$mnt"
trap 'sync; umount "$mnt" 2>/dev/null || true' EXIT

if [ "${1:-}" = "--remove" ]; then
	for f in $files; do rm -rf "${mnt:?}/xbin/$f"; done
	echo "retiré : $files"
	exit 0
fi

for f in $files; do
	[ -e "$mnt/xbin/$f" ] && [ ! -e "$mnt/xbin/$f.orig" ] && \
		{ echo "android-root : /system/xbin/$f existe déjà, gardé en $f.orig"; mv "$mnt/xbin/$f" "$mnt/xbin/$f.orig"; }
done

apk add -q busybox-static strace gcc musl-dev attr
gcc -static -Os -s -o /tmp/android-su "$here/su.c"

mkdir -p "$mnt/xbin/musl"
install -o 0 -g 0 -m 6755 /tmp/android-su "$mnt/xbin/su"
install -o 0 -g 0 -m 0755 /bin/busybox.static "$mnt/xbin/busybox"
install -o 0 -g 0 -m 0755 /usr/bin/strace "$mnt/xbin/musl/strace"
# chargeur musl + bibliothèques dont dépend strace
for lib in /lib/ld-musl-armhf.so.1 $(ldd /usr/bin/strace | awk '$3 ~ /^\// { print $3 }'); do
	install -o 0 -g 0 -m 0755 "$(readlink -f "$lib")" "$mnt/xbin/musl/$(basename "$lib")"
done
cat > "$mnt/xbin/strace" <<'WRAP'
#!/system/bin/sh
exec /system/xbin/musl/ld-musl-armhf.so.1 --library-path /system/xbin/musl /system/xbin/musl/strace "$@"
WRAP
chown 0:0 "$mnt/xbin/strace"; chmod 0755 "$mnt/xbin/strace"

lbl=u:object_r:system_file:s0
for f in su busybox strace musl musl/*; do
	setfattr -n security.selinux -v "$lbl" "$mnt/xbin/$f" 2>/dev/null || \
		echo "android-root : label SELinux non posé sur $f (noyau sans CONFIG_EXT4_FS_SECURITY ?)"
done
ls -l "$mnt/xbin/su" "$mnt/xbin/busybox" "$mnt/xbin/strace"
echo "installé. Sous Android : adb shell, puis su ; getenforce pour l'état SELinux."
