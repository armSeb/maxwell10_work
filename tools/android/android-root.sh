#!/bin/sh
# Accès root sur l'Android d'origine (build « user », SELinux enforcing), préparé
# depuis postmarketOS. À lancer en root sur la tablette :
#
#   sudo ./android/android-root.sh            # installe
#   sudo ./android/android-root.sh --remove   # retire tout, remet macaddress.sh d'origine
#
# Un su setuid ne marche pas ici : le domaine SELinux « shell » d'adb n'a pas le
# droit de changer d'uid. Mais init lance /system/etc/macaddress.sh (service
# macaddr_script, à chaque boot) par /system/bin/sh, donc en root dans le domaine
# non confiné « init_shell ». On y ajoute le lancement de rootd (rootd.c), un
# démon qui offre des terminaux d'accueil /dev/pts/N ouvrables seulement par
# root et l'utilisateur shell d'adb (init_shell n'a pas le droit de créer de
# socket TCP) ; su (su.c) est son client. init_shell n'a pas non plus le droit
# ptrace : strace n'y marche pas.
#
# Posé dans /system/xbin : su, rootd, busybox (statique, d'Alpine).
set -eu

die() { echo "android-root : $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "à lancer en root (sudo)"
here=$(cd "$(dirname "$0")" && pwd)
mnt=/mnt/android-system
mark="# maxwell10_work : rootd"
hook=etc/macaddress.sh
lbl=u:object_r:system_file:s0

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
[ -f "$mnt/$hook" ] || die "/system/$hook absent : ce n'est pas l'Android attendu"
mount -o remount,rw "$mnt"
trap 'sync; umount "$mnt" 2>/dev/null || true' EXIT

# réécrit le script sans changer son inode (garde son label SELinux)
unhook() {
	grep -v -F "$mark" "$mnt/$hook" > /tmp/android-hook
	cat /tmp/android-hook > "$mnt/$hook"
}

# fichiers posés par ce script (les autres déjà présents sont gardés en .orig)
manifest="$mnt/xbin/.maxwell10_work"
# première version (su setuid + strace) : ses su et busybox sont à nous aussi
[ -d "$mnt/xbin/musl" ] && [ ! -f "$manifest" ] && echo "su busybox" > "$manifest"
rm -rf "$mnt/xbin/strace" "$mnt/xbin/musl"
ours() { [ -f "$manifest" ] && grep -qw "$1" "$manifest"; }

if [ "${1:-}" = "--remove" ]; then
	unhook
	for f in su rootd busybox; do ours $f && rm -f "$mnt/xbin/$f"; done
	rm -f "$manifest"
	for f in su busybox; do
		[ -e "$mnt/xbin/$f.orig" ] && mv "$mnt/xbin/$f.orig" "$mnt/xbin/$f"
	done
	echo "retiré"
	exit 0
fi

for f in su busybox rootd; do
	[ -e "$mnt/xbin/$f" ] && [ ! -e "$mnt/xbin/$f.orig" ] && ! ours $f && \
		{ echo "android-root : /system/xbin/$f existe déjà, gardé en $f.orig"; mv "$mnt/xbin/$f" "$mnt/xbin/$f.orig"; }
done

apk add -q busybox-static gcc musl-dev attr
gcc -static -Os -s -o /tmp/android-su "$here/su.c"
gcc -static -Os -s -o /tmp/android-rootd "$here/rootd.c"
install -o 0 -g 0 -m 0755 /tmp/android-su "$mnt/xbin/su"
install -o 0 -g 0 -m 0755 /tmp/android-rootd "$mnt/xbin/rootd"
install -o 0 -g 0 -m 0755 /bin/busybox.static "$mnt/xbin/busybox"
echo "su rootd busybox" > "$manifest"
for f in su rootd busybox; do
	setfattr -n security.selinux -v "$lbl" "$mnt/xbin/$f" || die "label SELinux non posé sur $f"
done

# lancement de rootd au tout début de macaddress.sh (avant un éventuel exit)
unhook
awk -v l="/system/xbin/rootd $mark" 'NR == 1 && /^#!/ { print; print l; next } NR == 1 { print l } { print }' \
	"$mnt/$hook" > /tmp/android-hook
cat /tmp/android-hook > "$mnt/$hook"
head -3 "$mnt/$hook"
ls -lZ "$mnt/xbin/su" "$mnt/xbin/rootd" "$mnt/xbin/busybox" 2>/dev/null || ls -l "$mnt/xbin/su" "$mnt/xbin/rootd" "$mnt/xbin/busybox"
echo "installé. Sous Android : adb shell, puis su (ou su -c 'commande')."
