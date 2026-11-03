#!/bin/sh
# Binaire ARM statique pour Android (adb shell, sans root).
set -eu
here=$(dirname "$0")
arm-linux-gnueabihf-gcc -static -Os -Wall -o "$here/fiemap2uboot" "$here/fiemap2uboot.c"
arm-linux-gnueabihf-strip "$here/fiemap2uboot"
echo "$here/fiemap2uboot prêt — adb push $here/fiemap2uboot /data/local/tmp/"
