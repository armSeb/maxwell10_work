#!/bin/sh
# Monte la chaîne imx-media de la caméra OV9740 et capture une image.
#   ov9740-capture.sh [LARGEURxHAUTEUR] [fichier]      (défaut 1280x720, /tmp/ov9740.yuv)
# Chaîne (i.MX6Q) : ov9740 → imx6-mipi-csi2 (VC0) → ipu1_csi0_mux → ipu1_csi0 → capture
# Paquet requis : v4l-utils (media-ctl, v4l2-ctl).
set -eu
run() { echo "+ $*"; "$@"; }
size=${1:-1280x720}
out=${2:-/tmp/ov9740.yuv}
w=${size%x*}; h=${size#*x}
cam=$(media-ctl -p | sed -n "s/.*entity [0-9]*: \(ov9740 [0-9]*-0010\).*/\1/p" | head -1)
[ -n "$cam" ] || { echo "capteur ov9740 absent du graphe media (dmesg | grep ov9740)" >&2; exit 1; }
echo "capteur : $cam"
run media-ctl -r
run media-ctl -l "'$cam':0 -> 'imx6-mipi-csi2':0[1]"
run media-ctl -l "'imx6-mipi-csi2':1 -> 'ipu1_csi0_mux':0[1]"
run media-ctl -l "'ipu1_csi0_mux':2 -> 'ipu1_csi0':0[1]"
run media-ctl -l "'ipu1_csi0':2 -> 'ipu1_csi0 capture':0[1]"
fmt="YUYV8_2X8/${w}x${h} field:none"
run media-ctl -V "'$cam':0 [fmt:$fmt]"
run media-ctl -V "'imx6-mipi-csi2':1 [fmt:$fmt]"
run media-ctl -V "'ipu1_csi0_mux':2 [fmt:$fmt]"
run media-ctl -V "'ipu1_csi0':2 [fmt:$fmt]"
dev=$(media-ctl -e 'ipu1_csi0 capture')
echo "nœud vidéo : $dev"
run v4l2-ctl -d "$dev" --set-fmt-video=width="$w",height="$h",pixelformat=YUYV
run v4l2-ctl -d "$dev" --stream-mmap --stream-count=5 --stream-skip=4 --stream-to="$out"
echo "image(s) YUYV ${w}x${h} dans $out ; conversion :"
echo "  ffmpeg -f rawvideo -pix_fmt yuyv422 -s ${w}x${h} -i $out -frames:v 1 /tmp/ov9740.png"
