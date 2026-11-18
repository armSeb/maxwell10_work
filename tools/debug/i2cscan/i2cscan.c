/*
 * i2cscan — i2cdetect minimal pour /dev/i2c-N (Android ou Linux sans i2c-tools).
 *   ./i2cscan N [N...]
 * Lecture d'un octet à chaque adresse (comme i2cdetect -r) ; UU = adresse prise
 * par un pilote noyau. Binaire statique, sans dépendance.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void scan(int bus)
{
	char path[32];
	snprintf(path, sizeof path, "/dev/i2c-%d", bus);
	int fd = open(path, O_RDWR);
	if (fd < 0) { perror(path); return; }
	printf("bus %d\n     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n", bus);
	for (int a = 0; a < 0x80; a++) {
		if (!(a & 15)) printf("%02x: ", a);
		if (a < 3 || a > 0x77) printf("   ");
		else if (ioctl(fd, I2C_SLAVE, a) < 0) printf(errno == EBUSY ? "UU " : "?? ");
		else {
			unsigned char b;
			printf(read(fd, &b, 1) == 1 ? "%02x " : "-- ", a);
		}
		if ((a & 15) == 15) printf("\n");
	}
	close(fd);
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage : %s N [N...]\n", argv[0]); return 1; }
	for (int i = 1; i < argc; i++) scan(atoi(argv[i]));
	return 0;
}
