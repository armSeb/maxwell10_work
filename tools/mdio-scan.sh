#!/bin/sh
# mdio-scan.sh — lit directement le bus MDIO du FEC i.MX6 par /dev/mem (devmem),
# sans passer par Linux, pour savoir si le switch Marvell 88E6320 répond, et à
# quelles adresses. Lecture seule (trames MDIO de type READ uniquement).
#
# Sur la tablette, en root, eth0 levé (le FEC garde alors ses horloges actives) :
#   sh mdio-scan.sh            # registres 2 et 3 de chaque adresse 0..31
#   sh mdio-scan.sh 0x10 3     # une seule lecture : adresse 0x10, registre 3
#
# Lecture du résultat (88E6320) :
#   - mode mono-puce (adresse SMI 0) : registres de port aux adresses 0x10..0x16,
#     registre 3 = ID du switch, 0x115x (x = révision) ; globaux en 0x1b/0x1c ;
#   - mode multi-puce : le switch ne répond qu'à son adresse, par registres
#     indirects (0 = commande, 1 = données) : le DT doit alors donner cette adresse ;
#   - tout à 0xffff : personne ne répond (horloge, reset ou alimentation du switch).
set -eu

FEC=0x02188000
IEVENT=$((FEC + 0x004))
MMFR=$((FEC + 0x040))
MII_EVENT=0x00800000

command -v devmem >/dev/null || { echo "devmem introuvable (busybox)" >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo "à lancer en root" >&2; exit 1; }
grep -q up /sys/class/net/eth0/operstate 2>/dev/null || \
	echo "ATTENTION : eth0 n'est pas up, les horloges du FEC peuvent être coupées" >&2

mdio_read() {	# $1 = adresse, $2 = registre → valeur sur 16 bits
	devmem "$IEVENT" 32 "$MII_EVENT"			# acquitte un événement MII éventuel
	devmem "$MMFR" 32 $(( (1 << 30) | (2 << 28) | (($1 & 0x1f) << 23) | (($2 & 0x1f) << 18) | (2 << 16) ))
	i=0
	while [ $(( $(devmem "$IEVENT" 32) & MII_EVENT )) = 0 ]; do
		i=$((i + 1))
		[ "$i" -lt 1000 ] || { echo "timeout MDIO (adresse $1)" >&2; return 1; }
	done
	v=$(( $(devmem "$MMFR" 32) & 0xffff ))
	devmem "$IEVENT" 32 "$MII_EVENT"
	printf '0x%04x' "$v"
}

if [ $# -eq 2 ]; then
	mdio_read $(($1)) $(($2)); echo
	exit 0
fi

printf 'adr  reg2    reg3\n'
a=0
while [ "$a" -lt 32 ]; do
	r2=$(mdio_read "$a" 2); r3=$(mdio_read "$a" 3)
	if [ "$r2" != 0xffff ] || [ "$r3" != 0xffff ]; then
		printf '0x%02x %s  %s\n' "$a" "$r2" "$r3"
	fi
	a=$((a + 1))
done
echo "(adresses absentes = 0xffff sur les deux registres)"
