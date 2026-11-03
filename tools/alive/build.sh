#!/bin/sh
# Construit alive-boot.img (charge utile « signe de vie », voir alive.S).
# Usage : ./build.sh  puis  fastboot boot alive-boot.img
set -eu
cd "$(dirname "$0")"

if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
	arm-none-eabi-gcc -c alive.S -o alive.o
	arm-none-eabi-objcopy -O binary alive.o alive.bin
else
	clang --target=armv7a-none-eabi -c alive.S -o alive.o
	llvm-objcopy -O binary alive.o alive.bin
fi

: > empty.ramdisk
python3 ../mkbootimg_v0.py --kernel alive.bin --ramdisk empty.ramdisk -o alive-boot.img
rm -f alive.o empty.ramdisk
