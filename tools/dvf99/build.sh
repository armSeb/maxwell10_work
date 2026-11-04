#!/bin/sh
# Binaire ARM statique : scp vers la tablette, puis en root ./dvf99 …
# TINYALSA=/chemin/vers/tinyalsa (sources compilées : include/ et libtinyalsa-arm.a)
# ajoute le moteur audio (-A) ; le paquet device le compile avec tinyalsa-dev.
set -eu
here=$(dirname "$0")
if [ -n "${TINYALSA:-}" ]; then
	arm-linux-gnueabihf-gcc -static -Os -Wall -Wextra -DDVF99_AUDIO -I"$TINYALSA/include" \
		-o "$here/dvf99" "$here/dvf99.c" "$TINYALSA/libtinyalsa-arm.a" -lpthread -ldl 2>&1 | grep -v dlopen || true
else
	arm-linux-gnueabihf-gcc -static -Os -Wall -Wextra -o "$here/dvf99" "$here/dvf99.c"
fi
arm-linux-gnueabihf-strip "$here/dvf99"
echo "$here/dvf99 prêt"
