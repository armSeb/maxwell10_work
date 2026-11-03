#!/bin/sh
# Installe postmarketOS (Nura) pour le Gigaset Maxwell 10 sur une carte microSD,
# de façon reproductible, puis vérifie la partition de boot.
#
#   pmos/install-sdcard.sh /dev/sdX            # UI par défaut : buffyboard
#   UI=cage pmos/install-sdcard.sh /dev/sdX
#   APORTS=~/src/pmaports pmos/install-sdcard.sh /dev/sdX   # clone pmaports ailleurs
#
# Prérequis : pmbootstrap installé, `pmbootstrap init` lancé au moins une fois
# (n'importe quel appareil : il sert à créer la config et le clone pmaports).
#
# Étapes :
#   1. vérifie la carte (disque amovible, pas monté, < 64 Go) et demande confirmation ;
#   2. met à jour le clone pmaports (origin/main doit exister) ;
#   3. copie les paquets du dépôt dans pmaports/device/testing (sync.sh) et calcule les sommes ;
#   4. sélectionne l'appareil et l'UI dans la config pmbootstrap ;
#   5. pmbootstrap install --sdcard (EFFACE la carte) ;
#   6. vérifie que p1 contient go.bin et initramfs ;
#   7. affiche l'environnement U-Boot à saisir.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
dev=${1:-}
ui=${UI:-buffyboard}
device=gigaset-maxwell10

die() { echo "ERREUR : $*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

[ -n "$dev" ] || die "usage : $0 /dev/sdX   (variable UI=… facultative, défaut buffyboard)"
command -v pmbootstrap >/dev/null || die "pmbootstrap introuvable"
command -v lsblk >/dev/null || die "lsblk introuvable"

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

# --- 2. pmaports ------------------------------------------------------------
step "Clone pmaports"
# APORTS=… pour forcer le chemin ; sinon le clone par défaut dans le dossier de travail
work=$(pmbootstrap config work 2>/dev/null) || die "pas de config pmbootstrap : lancer « pmbootstrap init » une fois"
aports=${APORTS:-$work/cache_git/pmaports}
[ -d "$aports/.git" ] || die "$aports n'est pas un clone git (pmbootstrap init ?)"
git -C "$aports" fetch origin || die "git fetch du clone pmaports a échoué ($(git -C "$aports" remote get-url origin))"
git -C "$aports" rev-parse --verify -q origin/main >/dev/null || \
	die "origin/main absent dans $aports — branches distantes : $(git -C "$aports" branch -r | tr '\n' ' ')
     (pmbootstrap et pmaports désaccordés : mettre pmbootstrap à jour, ou recloner pmaports, voir docs/postmarketos.md)"

# --- 3. paquets -------------------------------------------------------------
step "Paquets $device"
"$here/sync.sh" "$aports"
pmbootstrap checksum "linux-$device" "device-$device"

# --- 4. config ----------------------------------------------------------------
step "Configuration : device=$device, ui=$ui"
pmbootstrap config device "$device"
pmbootstrap config ui "$ui"

# --- 5. installation --------------------------------------------------------
step "Installation (compilation du noyau au premier passage : long)"
pmbootstrap install --sdcard="$dev"

# --- 6. vérification de /boot ----------------------------------------------
step "Vérification de la partition de boot"
sync
case "$dev" in
	*[0-9]) p1="${dev}p1" ;;	# /dev/mmcblk0 → /dev/mmcblk0p1
	*) p1="${dev}1" ;;
esac
if command -v partprobe >/dev/null; then sudo partprobe "$dev" 2>/dev/null || true; fi
sleep 2
[ -b "$p1" ] || die "$p1 introuvable après l'installation"
mnt=$(mktemp -d)
sudo mount -o ro "$p1" "$mnt"
ok=1
for f in go.bin initramfs; do
	if [ -s "$mnt/$f" ]; then
		printf '  %-10s %s octets\n' "$f" "$(stat -c %s "$mnt/$f")"
	else
		echo "  $f MANQUANT"; ok=0
	fi
done
echo "  FS de p1 : $(lsblk -no FSTYPE,LABEL "$p1")"
sudo umount "$mnt"
rmdir "$mnt"
[ "$ok" = 1 ] || die "p1 ne contient pas go.bin et initramfs : U-Boot ne pourra pas démarrer"

# --- 7. U-Boot --------------------------------------------------------------
step "Carte prête"
cat <<'EOF'
Dans la console U-Boot (UART2), une seule fois :

  setenv sdgo 'go 0x12000000 0x14000000 ${filesize}'
  setenv sdboot 'mmc dev 1 && fatload mmc 1:1 0x12000000 go.bin && fatload mmc 1:1 0x14000000 initramfs && run sdgo'
  setenv bootcmd 'run sdboot; run android'
  saveenv

(la variable « android » doit contenir le bootcmd d'origine)
EOF
