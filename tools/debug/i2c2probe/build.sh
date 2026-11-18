#!/bin/sh
# Binaire ARM statique ; sur la tablette, en root.
set -eu
here=$(dirname "$0")
if command -v arm-linux-gnueabihf-gcc >/dev/null; then
	arm-linux-gnueabihf-gcc -static -Os -Wall -Wextra -o "$here/i2c2probe" "$here/i2c2probe.c"
	arm-linux-gnueabihf-strip "$here/i2c2probe"
else
	python3 -m ziglang cc -target arm-linux-musleabihf -mcpu=cortex_a9 -static -Os -s \
		-Wall -Wextra -Wno-date-time -o "$here/i2c2probe" "$here/i2c2probe.c"
fi
echo "$here/i2c2probe prêt"
