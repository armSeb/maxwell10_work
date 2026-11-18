/*
 * i2c2probe — scanne le bus i2c2 de l'i.MX6Q (contrôleur 0x021a4000), désactivé dans
 * notre DT, en le pilotant à la main par /dev/mem : horloge, pinmux du DT vendeur
 * (EIM_EB2 = SCL, EIM_D16 = SDA, ALT6) puis contrôleur en mode maître à ~17 kHz.
 * Sert à trouver le capteur de lumière ISL29023 sans recompiler le DT.
 *
 *   ./i2c2probe            scan des adresses 0x03..0x77
 *   ./i2c2probe lux [ADDR] ISL29023 (défaut 0x44) : ALS continu, 16 bits, gamme
 *                          1000 lux, affiche la valeur toutes les 0,5 s (Ctrl-C)
 *
 * Ne pas lancer si un pilote Linux possède déjà i2c2 (nœud activé dans le DT).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CCM_BASE	0x020c4000
#define IOMUXC_BASE	0x020e0000
#define I2C2_BASE	0x021a4000

#define CCGR2		0x70	/* CG4 (bits 9-8) : i2c2_serial */

#define IADR 0x00
#define IFDR 0x04
#define I2CR 0x08
#define I2SR 0x0c
#define I2DR 0x10

#define I2CR_IEN  0x80
#define I2CR_MSTA 0x20
#define I2CR_MTX  0x10
#define I2CR_TXAK 0x08
#define I2CR_RSTA 0x04

#define I2SR_IBB  0x20
#define I2SR_IAL  0x10
#define I2SR_IIF  0x02
#define I2SR_RXAK 0x01

static volatile uint8_t *ccm, *mux, *i2c;

static uint32_t rd32(volatile uint8_t *b, unsigned o) { return *(volatile uint32_t *)(b + o); }
static void wr32(volatile uint8_t *b, unsigned o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static uint16_t ird(unsigned o) { return *(volatile uint16_t *)(i2c + o); }
static void iwr(unsigned o, uint16_t v) { *(volatile uint16_t *)(i2c + o) = v; }

static void *map(int fd, off_t base)
{
	void *p = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
	if (p == MAP_FAILED) { perror("mmap"); exit(1); }
	return p;
}

static void usleep_(long us) { struct timespec t = { us / 1000000, (us % 1000000) * 1000 }; nanosleep(&t, NULL); }

/* attend que (I2SR & mask) == val ; 0 si ok */
static int wait_sr(uint16_t mask, uint16_t val)
{
	for (int i = 0; i < 20000; i++) {
		if ((ird(I2SR) & mask) == val) return 0;
		usleep_(5);
	}
	return -1;
}

static void reset_ctrl(void)
{
	iwr(I2CR, 0);
	iwr(IFDR, 0x1f);	/* diviseur 3840 : ~17 kHz sur ipg 66 MHz */
	iwr(I2SR, 0);
	iwr(I2CR, I2CR_IEN);
	usleep_(100);
}

static void stop(void)
{
	iwr(I2CR, I2CR_IEN);
	wait_sr(I2SR_IBB, 0);
}

/* START (ou RESTART) + octet d'adresse ; 1 = ACK, 0 = NACK, -1 = erreur bus */
static int start_addr(uint8_t a8, int restart)
{
	if (!restart) {
		if (wait_sr(I2SR_IBB, 0)) { reset_ctrl(); if (wait_sr(I2SR_IBB, 0)) return -1; }
		iwr(I2CR, I2CR_IEN | I2CR_MSTA | I2CR_MTX);
		if (wait_sr(I2SR_IBB, I2SR_IBB)) return -1;
	} else {
		iwr(I2CR, I2CR_IEN | I2CR_MSTA | I2CR_MTX | I2CR_RSTA);
	}
	iwr(I2SR, 0);
	iwr(I2DR, a8);
	if (wait_sr(I2SR_IIF, I2SR_IIF)) return -1;
	uint16_t sr = ird(I2SR);
	iwr(I2SR, 0);
	if (sr & I2SR_IAL) return -1;
	return !(sr & I2SR_RXAK);
}

static int tx(uint8_t b)
{
	iwr(I2DR, b);
	if (wait_sr(I2SR_IIF, I2SR_IIF)) return -1;
	uint16_t sr = ird(I2SR);
	iwr(I2SR, 0);
	return (sr & I2SR_RXAK) ? -1 : 0;
}

static int write_reg(uint8_t a, uint8_t reg, uint8_t v)
{
	int r = start_addr(a << 1, 0);
	if (r == 1) r = tx(reg) || tx(v) ? -1 : 0; else r = -1;
	stop();
	return r;
}

static int read_regs(uint8_t a, uint8_t reg, uint8_t *buf, int n)
{
	if (start_addr(a << 1, 0) != 1 || tx(reg)) { stop(); return -1; }
	if (start_addr((a << 1) | 1, 1) != 1) { stop(); return -1; }
	/* passage en réception ; TXAK sur le dernier octet */
	iwr(I2CR, I2CR_IEN | I2CR_MSTA | (n == 1 ? I2CR_TXAK : 0));
	(void)ird(I2DR);	/* lecture factice : lance le premier octet */
	for (int i = 0; i < n; i++) {
		if (wait_sr(I2SR_IIF, I2SR_IIF)) { stop(); return -1; }
		iwr(I2SR, 0);
		if (i == n - 1) {
			iwr(I2CR, I2CR_IEN);	/* STOP avant de lire le dernier */
			wait_sr(I2SR_IBB, 0);
		} else if (i == n - 2) {
			iwr(I2CR, I2CR_IEN | I2CR_MSTA | I2CR_TXAK);
		}
		buf[i] = ird(I2DR);
	}
	return 0;
}

static void setup(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem (root ?)"); exit(1); }
	ccm = map(fd, CCM_BASE);
	mux = map(fd, IOMUXC_BASE);
	i2c = map(fd, I2C2_BASE);

	wr32(ccm, CCGR2, rd32(ccm, CCGR2) | (3u << 8));	/* horloge i2c2 */

	/* pinmux du DT vendeur (i2c2grp) : ALT6 + SION, pad 0x1b8b1, daisy 0 */
	wr32(mux, 0x08c, 0x16);	wr32(mux, 0x3a0, 0x1b8b1); wr32(mux, 0x8a0, 0);	/* EIM_EB2 : SCL */
	wr32(mux, 0x090, 0x16);	wr32(mux, 0x3a4, 0x1b8b1); wr32(mux, 0x8a4, 0);	/* EIM_D16 : SDA */

	reset_ctrl();
}

static void scan(void)
{
	int found = 0;
	printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
	for (int a = 0; a < 0x80; a++) {
		if (!(a & 15)) printf("%02x: ", a);
		if (a < 3 || a > 0x77) { printf("   "); }
		else {
			int r = start_addr(a << 1, 0);
			stop();
			if (r < 0) { printf("?? "); reset_ctrl(); }
			else if (r) { printf("%02x ", a); found++; }
			else printf("-- ");
		}
		if ((a & 15) == 15) printf("\n");
	}
	printf("%d périphérique(s)%s\n", found,
	       found ? "" : " ; « ?? » partout = bus bloqué ou pinmux faux");
}

static void lux(uint8_t a)
{
	/* CMD-II (0x01) : gamme 1000 lux, ADC 16 bits ; CMD-I (0x00) : ALS continu */
	if (write_reg(a, 0x01, 0x00) || write_reg(a, 0x00, 0xa0)) {
		fprintf(stderr, "pas de réponse en 0x%02x\n", a); exit(1);
	}
	usleep_(200000);
	for (;;) {
		uint8_t d[2];
		if (read_regs(a, 0x02, d, 2)) { printf("lecture échouée\n"); reset_ctrl(); }
		else {
			unsigned raw = d[0] | d[1] << 8;
			printf("brut %5u  ≈ %4u lux\n", raw, raw * 1000u / 65536u);
		}
		fflush(stdout);
		usleep_(500000);
	}
}

int main(int argc, char **argv)
{
	setup();
	if (argc > 1 && !strcmp(argv[1], "lux"))
		lux(argc > 2 ? strtoul(argv[2], NULL, 0) : 0x44);
	else
		scan();
	return 0;
}
