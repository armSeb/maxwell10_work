#!/bin/sh
# Binaire ARM statique : scp vers la tablette, puis en root ./dvf99 …
set -eu
here=$(dirname "$0")
arm-linux-gnueabihf-gcc -static -Os -Wall -Wextra -o "$here/dvf99" "$here/dvf99.c" -lpthread
arm-linux-gnueabihf-strip "$here/dvf99"
echo "$here/dvf99 prêt"
