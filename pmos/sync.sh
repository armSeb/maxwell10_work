#!/bin/sh
# Recopie dans les paquets pmaports les fichiers maintenus ailleurs dans le dépôt,
# puis (optionnel) les installe dans un checkout pmaports :
#   pmos/sync.sh                       → met à jour pmos/ seulement
#   pmos/sync.sh ~/.local/var/pmbootstrap/cache_git/pmaports
#                                      → copie aussi dans pmaports/device/testing/
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
k="$here/linux-gigaset-maxwell10"

cp "$top/dts/imx6q-gigaset-maxwell10.dts" "$k/"
cp "$top/tools/goshim/goshim.S" "$k/"
[ -f "$k/config-gigaset-maxwell10.armv7" ] || {
	echo "manque $k/config-gigaset-maxwell10.armv7 (voir pmos/README.md)" >&2; exit 1; }

if [ $# -ge 1 ]; then
	dst="$1/device/testing"
	[ -d "$dst" ] || { echo "$dst introuvable (chemin pmaports ?)" >&2; exit 1; }
	rm -rf "$dst/linux-gigaset-maxwell10" "$dst/device-gigaset-maxwell10"
	cp -r "$k" "$here/device-gigaset-maxwell10" "$dst/"
	rm -f "$dst/linux-gigaset-maxwell10/maxwell10.fragment"
	echo "copié dans $dst — ensuite : pmbootstrap checksum linux-gigaset-maxwell10 device-gigaset-maxwell10"
fi
