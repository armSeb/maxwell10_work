#!/bin/bash
# Dump des LUNs exposés par le boot.img "maxwell-dump" (lecture seule côté tablette).
# Usage : sudo ./dump-maxwell.sh [dossier_de_sortie]
set -euo pipefail

OUT="${1:-maxwell-dump-$(date +%Y%m%d-%H%M)}"
mkdir -p "$OUT"

mapfile -t DISKS < <(lsblk -dnpo NAME,VENDOR,MODEL | awk '$2=="Maxwell" {print $1" "$3}')
if [ "${#DISKS[@]}" -eq 0 ]; then
    echo "Aucun disque 'Maxwell' trouvé. Vérifie 'lsblk -o NAME,VENDOR,MODEL,SIZE' et le dmesg."
    exit 1
fi

for entry in "${DISKS[@]}"; do
    dev="${entry%% *}"
    name="${entry##* }"
    echo "== $name ($dev, $(lsblk -dnbo SIZE "$dev") octets)"
    dd if="$dev" of="$OUT/$name.img" bs=4M status=progress conv=fsync
done

( cd "$OUT" && sha256sum ./*.img | tee SHA256SUMS )
echo "Terminé : $OUT"
