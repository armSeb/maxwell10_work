#!/bin/sh
# Assemble goshim.bin (512 octets) et, si un zImage avec DTB appendé est donné,
# produit le fichier à charger en RAM puis à lancer avec `go`.
#   ./build.sh                      → goshim.bin seul
#   ./build.sh zImage-dtb go.bin    → goshim.bin + zImage-dtb dans go.bin
set -eu
here=$(dirname "$0")

if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
	arm-none-eabi-gcc -c "$here/goshim.S" -o "$here/goshim.o"
	arm-none-eabi-ld -Ttext=0 -e _start "$here/goshim.o" -o "$here/goshim.elf"
	arm-none-eabi-objcopy -O binary "$here/goshim.elf" "$here/goshim.bin"
else
	clang --target=armv7a-none-eabi -c "$here/goshim.S" -o "$here/goshim.o"
	ld.lld -Ttext=0 -e _start "$here/goshim.o" -o "$here/goshim.elf"
	llvm-objcopy -O binary "$here/goshim.elf" "$here/goshim.bin"
fi
# le code est position-indépendant ; l'édition de liens résout les bl internes
rm -f "$here/goshim.o" "$here/goshim.elf"

size=$(wc -c < "$here/goshim.bin")
[ "$size" -eq 512 ] || { echo "goshim.bin fait $size octets au lieu de 512" >&2; exit 1; }

if [ $# -eq 2 ]; then
	cat "$here/goshim.bin" "$1" > "$2"
	echo "$2 : $(wc -c < "$2") octets — dans U-Boot : loady <addr> puis go <addr>"
fi
