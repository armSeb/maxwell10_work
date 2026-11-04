/*
 * mdio-scan — lit directement le bus MDIO du FEC i.MX6 par /dev/mem, sans
 * passer par Linux, pour savoir si le switch Marvell 88E6320 répond, et à
 * quelles adresses. Lecture seule : uniquement des trames MDIO de type READ.
 * (busybox d'Alpine n'a pas devmem, d'où ce petit binaire statique.)
 *
 * Sur la tablette, en root, eth0 levé (le FEC garde alors ses horloges) :
 *   ./mdio-scan              registres 2 et 3 de chaque adresse 0..31
 *   ./mdio-scan 0x10 3       une lecture : adresse 0x10, registre 3
 *
 * Lecture du résultat (88E6320) :
 *   - mode mono-puce (adresse SMI 0) : registres de port aux adresses 0x10..0x16,
 *     registre 3 = ID du switch 0x115x (x = révision) ; globaux en 0x1b/0x1c ;
 *   - mode multi-puce : le switch ne répond qu'à son adresse (registres
 *     indirects 0 = commande, 1 = données) : le DT doit alors donner cette adresse ;
 *   - rien (tout à 0xffff) : personne ne répond (horloge, reset, alimentation).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define FEC_BASE	0x02188000
#define IEVENT		(0x004 / 4)
#define MMFR		(0x040 / 4)
#define MII_SPEED	(0x044 / 4)
#define MII_EVENT	0x00800000u

static volatile uint32_t *fec;

static int mdio_read(unsigned addr, unsigned reg)
{
	struct timespec ts = { 0, 20000 };
	fec[IEVENT] = MII_EVENT;
	fec[MMFR] = (1u << 30) | (2u << 28) | ((addr & 0x1f) << 23) |
		    ((reg & 0x1f) << 18) | (2u << 16);
	for (int i = 0; !(fec[IEVENT] & MII_EVENT); i++) {
		if (i > 5000)
			return -1;
		nanosleep(&ts, NULL);
	}
	int v = fec[MMFR] & 0xffff;
	fec[IEVENT] = MII_EVENT;
	return v;
}

int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem (root ?)"); return 1; }
	fec = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, FEC_BASE);
	if (fec == MAP_FAILED) { perror("mmap"); return 1; }

	if (!(fec[MII_SPEED] & 0x7e)) {
		fprintf(stderr, "MII_SPEED = 0 : horloge MDIO non configurée (eth0 down ?)\n");
		return 1;
	}
	if (argc == 3) {
		int v = mdio_read(strtoul(argv[1], NULL, 0), strtoul(argv[2], NULL, 0));
		if (v < 0) { fprintf(stderr, "timeout MDIO\n"); return 1; }
		printf("0x%04x\n", v);
		return 0;
	}
	printf("MII_SPEED = 0x%08x\nadr  reg2    reg3\n", fec[MII_SPEED]);
	int seen = 0;
	for (unsigned a = 0; a < 32; a++) {
		int r2 = mdio_read(a, 2), r3 = mdio_read(a, 3);
		if (r2 < 0 || r3 < 0) { printf("0x%02x timeout\n", a); continue; }
		if (r2 != 0xffff || r3 != 0xffff) {
			printf("0x%02x 0x%04x  0x%04x\n", a, r2, r3);
			seen++;
		}
	}
	printf("%d adresse(s) répondent (les autres lisent 0xffff)\n", seen);
	return 0;
}
