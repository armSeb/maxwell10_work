#!/bin/sh
set -eu
here=$(dirname "$0")
arm-linux-gnueabihf-gcc -static -Os -Wall -o "$here/memrw" "$here/memrw.c"
arm-linux-gnueabihf-strip "$here/memrw"
echo "$here/memrw prêt"
