#!/bin/sh
# Installe postmarketOS (Nura) pour le Gigaset Maxwell 10 sur une carte microSD,
# de façon reproductible.
#
#   pmos/install-sdcard.sh /dev/sdX            # UI par défaut : phosh
#   UI=xfce4 pmos/install-sdcard.sh /dev/sdX   # ou buffyboard, cage, console…
#   APORTS=~/src/pmaports pmos/install-sdcard.sh /dev/sdX   # clone pmaports ailleurs
#   REUSE_IMAGE=1 pmos/install-sdcard.sh /dev/sdX   # réécrire la dernière image, sans rien reconstruire
#
# Prérequis : pmbootstrap installé, `pmbootstrap init` lancé au moins une fois
# (n'importe quel appareil : il sert à créer la config et le clone pmaports) ;
# sgdisk (paquet gdisk), sfdisk/findmnt (util-linux).
#
# pmbootstrap crée une table GPT avec p1 typée ESP, illisible par U-Boot 2009.08.
# On ne le laisse donc pas écrire la carte : il produit une IMAGE, que le script
# convertit en MBR (p1 en FAT32 LBA 0x0C), vérifie, puis seulement écrit sur la
# carte. La carte reçoit directement le bon format.
#
# Étapes :
#   1. vérifie la carte (disque entier, pas monté, < 64 Go) et demande confirmation ;
#   2. met à jour le clone pmaports (origin/main doit exister) ;
#   3. copie les paquets du dépôt dans pmaports/device/testing (sync.sh) et calcule les sommes ;
#   4. sélectionne l'appareil et l'UI dans la config pmbootstrap ;
#   5. pmbootstrap install → image disque ;
#   6. image : GPT → MBR, p1 0x0C active, p2 0x83 ;
#   7. image : vérifie que p1 contient go.bin et initramfs ;
#   8. écrit l'image sur la carte et revérifie la table ;
#   9. affiche l'environnement U-Boot à saisir.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
dev=${1:-}
ui=${UI:-phosh}
device=gigaset-maxwell10

die() { echo "ERREUR : $*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

[ -n "$dev" ] || die "usage : $0 /dev/sdX   (variable UI=… facultative, défaut phosh)"
command -v pmbootstrap >/dev/null || die "pmbootstrap introuvable"
for t in lsblk blkid sfdisk findmnt sgdisk dd; do
	command -v "$t" >/dev/null || die "$t introuvable (util-linux, gdisk, coreutils)"
done

# --- 1. la carte -----------------------------------------------------------
step "Carte $dev"
[ -b "$dev" ] || die "$dev n'est pas un périphérique bloc"
[ "$(lsblk -dno TYPE "$dev")" = disk ] || die "$dev n'est pas un disque entier (donner /dev/sdX, pas /dev/sdX1)"
[ "$(lsblk -dno RM "$dev" | tr -d ' ')" = 1 ] || \
	echo "ATTENTION : $dev n'est pas signalé comme amovible (normal pour certains lecteurs USB)."
size=$(lsblk -dbno SIZE "$dev")
[ "$size" -le $((64 * 1024 * 1024 * 1024)) ] || die "$dev fait plus de 64 Go : ce n'est sans doute pas la carte"
if lsblk -no MOUNTPOINT "$dev" | grep -q .; then
	die "une partition de $dev est montée, la démonter d'abord"
fi
lsblk -o NAME,SIZE,MODEL,LABEL,FSTYPE "$dev"
printf 'Tout le contenu de %s sera EFFACÉ. Retaper le chemin pour confirmer : ' "$dev"
read -r answer
[ "$answer" = "$dev" ] || die "confirmation incorrecte, rien n'a été fait"

work=$(pmbootstrap config work 2>/dev/null) || die "pas de config pmbootstrap : lancer « pmbootstrap init » une fois"

if [ -n "${REUSE_IMAGE:-}" ]; then
	step "Réutilisation de la dernière image construite (REUSE_IMAGE)"
else
	# --- 2. pmaports ------------------------------------------------------------
	step "Clone pmaports"
	# APORTS=… pour forcer le chemin ; sinon le clone par défaut dans le dossier de travail
	aports=${APORTS:-$work/cache_git/pmaports}
	[ -d "$aports/.git" ] || die "$aports n'est pas un clone git (pmbootstrap init ?)"
	git -C "$aports" fetch origin || die "git fetch du clone pmaports a échoué ($(git -C "$aports" remote get-url origin))"
	git -C "$aports" rev-parse --verify -q origin/main >/dev/null || \
		die "origin/main absent dans $aports — branches distantes : $(git -C "$aports" branch -r | tr '\n' ' ')
	     (pmbootstrap et pmaports désaccordés : mettre pmbootstrap à jour, ou recloner pmaports, voir docs/postmarketos.md)"
	# avancer la copie de travail (le fetch seul laisse les recettes en retard sur le
	# dépôt binaire : « about to install … (local pmaports: …) »)
	if [ "$(git -C "$aports" rev-parse --abbrev-ref HEAD)" = main ]; then
		git -C "$aports" merge -q --ff-only origin/main || \
			echo "ATTENTION : avance rapide impossible dans $aports (modifications locales ?), on continue"
	else
		echo "ATTENTION : $aports n'est pas sur main, pas de mise à jour"
	fi

	# --- 3. paquets -------------------------------------------------------------
	step "Paquets $device"
	"$here/sync.sh" "$aports"
	pmbootstrap checksum "linux-$device" "device-$device"

	# --- 4. config ----------------------------------------------------------------
	step "Configuration : device=$device, ui=$ui"
	pmbootstrap config device "$device"
	pmbootstrap config ui "$ui"
	# Les UI GNOME/Plasma (phosh…) passent sur le canal systemd-edge : des chroots
	# créés pour un autre canal font échouer l'installation. On les supprime
	# (seulement les chroots : paquets construits et caches conservés).
	pmbootstrap -y zap

	# --- 5. installation dans une image --------------------------------------
	step "Installation dans une image (compilation du noyau au premier passage : long)"
	pmbootstrap install
fi
img="$work/chroot_native/home/pmos/rootfs/$device.img"
[ -f "$img" ] || die "image introuvable : $img"
img_size=$(stat -c %s "$img")
echo "  image : $img ($((img_size / 1024 / 1024)) Mio)"
[ "$img_size" -le "$size" ] || die "l'image ($img_size o) ne tient pas sur la carte ($size o) : carte plus grande, ou UI plus légère"

# --- 6. table de partitions lisible par U-Boot 2009.08 ----------------------
# GPT → MBR (positions et contenu des partitions inchangés, labels et UUID des
# systèmes de fichiers aussi), p1 en FAT32 LBA (0x0C) active, p2 en Linux.
# Sans cela : U-Boot ne voit pas p1, et udisks ignore p1 (ESP) sur le PC.
step "Image : table MBR, p1 en 0x0C"
pttype=$(sudo blkid -p -o value -s PTTYPE "$img" || true)
echo "  table produite par pmbootstrap : ${pttype:-inconnue}"
if [ "$pttype" = gpt ]; then
	sudo sgdisk --gpttombr=1:2 "$img" >/dev/null
fi
pttype=$(sudo blkid -p -o value -s PTTYPE "$img" || true)
[ "$pttype" = dos ] || die "table « $pttype » après conversion, MBR attendu"
sudo sfdisk -q --part-type "$img" 1 c
sudo sfdisk -q --part-type "$img" 2 83
sudo sfdisk -q --activate "$img" 1
sudo sfdisk -l "$img" | sed -n '/^Device/,$p'

# --- 7. vérification de /boot dans l'image ---------------------------------
step "Image : contenu de la partition de boot"
# montage par décalage (pas de losetup -P : pas besoin d'udev pour les nœuds pN)
p1_start=$(sudo sfdisk -d "$img" | sed -n 's/^[^ ]*1 : start= *\([0-9]*\),.*/\1/p')
[ -n "$p1_start" ] || die "début de p1 introuvable dans l'image"
mnt=$(mktemp -d)
cleanup() { sudo umount "$mnt" 2>/dev/null || true; rmdir "$mnt" 2>/dev/null || true; }
trap cleanup EXIT
sudo mount -o ro,loop,offset=$((p1_start * 512)) "$img" "$mnt"
echo "  p1 : $(findmnt -no FSTYPE "$mnt"), commence au secteur $p1_start"
ok=1
for f in go.bin initramfs; do
	if [ -s "$mnt/$f" ]; then
		printf '  %-10s %s octets\n' "$f" "$(stat -c %s "$mnt/$f")"
	else
		echo "  $f MANQUANT"; ok=0
	fi
done
cleanup
trap - EXIT
[ "$ok" = 1 ] || die "p1 ne contient pas go.bin et initramfs : U-Boot ne pourra pas démarrer (carte non écrite)"

# --- 8. écriture sur la carte -----------------------------------------------
step "Écriture sur $dev"
if lsblk -no MOUNTPOINT "$dev" | grep -q .; then
	die "une partition de $dev a été montée entre-temps, la démonter puis relancer"
fi
sudo dd if="$img" of="$dev" bs=4M conv=fsync status=progress
sync
if command -v partprobe >/dev/null; then sudo partprobe "$dev" 2>/dev/null || true; fi
[ "$(sudo blkid -p -o value -s PTTYPE "$dev")" = dos ] || die "la carte ne lit pas un MBR après écriture"
sudo sfdisk -l "$dev" | sed -n '/^Device/,$p'
echo "  La partition racine sera agrandie à toute la carte au premier démarrage."

# --- 9. U-Boot --------------------------------------------------------------
step "Carte prête"
cat <<'EOF'
Dans la console U-Boot (UART2), une seule fois :

  setenv sdgo 'go 0x12000000 0x14000000 ${filesize}'
  setenv sdboot 'mmc dev 1 && fatload mmc 1:1 0x12000000 go.bin && fatload mmc 1:1 0x14000000 initramfs && run sdgo'
  setenv bootcmd 'run sdboot; run android'
  saveenv

(la variable « android » doit contenir le bootcmd d'origine)
EOF
