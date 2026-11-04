#!/bin/sh
# Binaire ARM statique ; sur la tablette, en root, pendant une lecture ALSA.
set -eu
here=$(dirname "$0")
arm-linux-gnueabihf-gcc -static -Os -Wall -Wextra -o "$here/ssireg" "$here/ssireg.c"
arm-linux-gnueabihf-strip "$here/ssireg"
echo "$here/ssireg prêt"
