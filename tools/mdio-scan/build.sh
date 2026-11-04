#!/bin/sh
# Binaire ARM statique : adb/scp vers la tablette, puis ./mdio-scan en root.
set -eu
here=$(dirname "$0")
arm-linux-gnueabihf-gcc -static -Os -Wall -o "$here/mdio-scan" "$here/mdio-scan.c"
arm-linux-gnueabihf-strip "$here/mdio-scan"
echo "$here/mdio-scan prêt"
