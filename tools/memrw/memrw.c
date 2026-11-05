/*
 * memrw — lecture/écriture d'un registre 32 bits par /dev/mem (busybox d'Alpine
 * n'a pas devmem). Pour la mise au point ; un registre dont le bloc n'a pas son
 * horloge fige la tablette : ne lire que des blocs actifs.
 *   memrw ADRESSE            lit
 *   memrw ADRESSE VALEUR     écrit puis relit
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage : memrw ADRESSE [VALEUR]\n"); return 2; }
	unsigned long a = strtoul(argv[1], NULL, 0);
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	long pg = sysconf(_SC_PAGESIZE);
	volatile uint8_t *m = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, a & ~(pg - 1));
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	volatile uint32_t *r = (volatile uint32_t *)(m + (a & (pg - 1)));
	if (argc > 2) {
		uint32_t v = strtoul(argv[2], NULL, 0);
		printf("0x%08lx : 0x%08x → 0x%08x\n", a, *r, v);
		*r = v;
	}
	printf("0x%08lx = 0x%08x\n", a, *r);
	return 0;
}
