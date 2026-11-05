/*
 * ssireg — lit/écrit les registres du SSI2 et de l'AUDMUX de l'i.MX6 par /dev/mem,
 * pour mettre au point la liaison TDM avec la DVF9919 sans recompiler le DT.
 *
 * ⚠️ N'utiliser que PENDANT une lecture/capture ALSA sur la carte DVF99 :
 * horloge du SSI2 coupée, un accès bloque le bus (la tablette se fige).
 *
 *   ./ssireg              état décodé (SCR, STCR, STCCR, masques, SISR, AUDMUX)
 *   ./ssireg rate         mesure 1 s : synchros de trame vues (TFS) et sous-débits
 *   ./ssireg set REG VAL  écrit un registre (stcr, srcr, stccr, stmsk, srmsk, scr)
 *   ./ssireg fs           mesure la synchro de trame venant de la DVF99 sur le pad
 *                         SD2_DAT1 (lu par GPIO1_IO14, SION posé le temps de la
 *                         mesure) : polarité et largeur en bits d'horloge
 *
 * Bits de STCR utiles : 0 TEFS (synchro 1 bit en avance), 1 TFSL (synchro d'1 bit
 * au lieu d'un mot), 2 TFSI (synchro inversée), 3 TSCKP (front d'horloge),
 * 4 TSHFD (LSB en premier). STMSK : 1 = slot ignoré.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define SSI2_BASE	0x0202c000
#define AUDMUX_BASE	0x021d8000

#define IOMUXC_BASE	0x020e0000
#define MUX_SD2_DAT1	0x04c		/* SW_MUX_CTL_PAD_SD2_DATA1 */
#define GPIO1_BASE	0x0209c000
#define GPIO_PSR	0x08
#define FS_BIT		14		/* SD2_DAT1 = GPIO1_IO14 */

static volatile uint32_t *ssi, *mux, *iomuxc, *gpio1;

static const struct { const char *name; unsigned off; } regs[] = {
	{ "scr", 0x10 }, { "sisr", 0x14 }, { "sier", 0x18 }, { "stcr", 0x1c },
	{ "srcr", 0x20 }, { "stccr", 0x24 }, { "srccr", 0x28 }, { "sfcsr", 0x2c },
	{ "stmsk", 0x48 }, { "srmsk", 0x4c },
};

static int reg_off(const char *n)
{
	for (unsigned i = 0; i < sizeof regs / sizeof *regs; i++)
		if (!strcasecmp(n, regs[i].name)) return regs[i].off;
	return -1;
}

static uint32_t r(unsigned off) { return ssi[off / 4]; }

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void xcr(const char *n, uint32_t v)
{
	printf("%-6s 0x%08x  synchro %s%s%s, horloge %s, %s en premier, %s, fifo0 %s%s\n", n, v,
	       v & 2 ? "1 bit" : "mot", v & 1 ? " en avance" : "", v & 4 ? " inversée" : "",
	       v & 8 ? "TSCKP=1" : "TSCKP=0", v & 0x10 ? "LSB" : "MSB",
	       v & 0x60 ? "horloges SORTIES (maître)" : "horloges entrantes (esclave)",
	       v & 0x80 ? "on" : "off", v & 0x200 ? ", TXBIT0" : "");
}

static void xccr(const char *n, uint32_t v)
{
	unsigned wl = (v >> 13) & 0xf, dc = ((v >> 8) & 0x1f) + 1;
	static const int wlb[16] = { 0, 0, 0, 8, 10, 12, 0, 16, 18, 20, 22, 24 };
	printf("%-6s 0x%08x  mot %d bits, %u slots/trame\n", n, v, wlb[wl], dc);
}

static void dump(void)
{
	uint32_t scr = r(0x10), sisr = r(0x14);
	printf("scr    0x%08x  %s%s%s%s%s mode %s\n", scr, scr & 1 ? "SSIEN " : "", scr & 2 ? "TE " : "",
	       scr & 4 ? "RE " : "", scr & 8 ? "NET " : "", scr & 0x10 ? "SYN " : "",
	       (const char *[]){ "normal", "I2S maître", "I2S esclave", "normal" }[(scr >> 5) & 3]);
	xcr("stcr", r(0x1c)); xcr("srcr", r(0x20));
	xccr("stccr", r(0x24)); xccr("srccr", r(0x28));
	printf("stmsk  0x%08x  slots émis :", r(0x48));
	for (int i = 0; i < 32; i++) if (!(r(0x48) >> i & 1)) printf(" %d", i);
	printf("\nsrmsk  0x%08x\n", r(0x4c));
	printf("sisr   0x%08x  %s%s%s%s\n", sisr, sisr & 0x80 ? "TFS " : "", sisr & 0x20 ? "TLS " : "",
	       sisr & 0x100 ? "SOUS-DÉBIT " : "", sisr & 0x400 ? "DÉBORDEMENT RX " : "");
	printf("sfcsr  0x%08x  fifo tx0 %u mots\n", r(0x2c), (r(0x2c) >> 8) & 0xf);
	for (int p = 2; p <= 4; p += 2)
		printf("audmux port %d : ptcr 0x%08x pdcr 0x%08x\n", p,
		       mux[(p - 1) * 2], mux[(p - 1) * 2 + 1]);
}

static void rate(void)
{
	unsigned fs = 0, ls = 0, ue = 0, polls = 0, pfs = 0, pls = 0;
	double end = now() + 1.0;
	while (now() < end) {
		for (int i = 0; i < 1000; i++) {
			uint32_t s = ssi[0x14 / 4];
			unsigned f = !!(s & 0x80), l = !!(s & 0x20);
			fs += f && !pfs; ls += l && !pls; ue += !!(s & 0x100);
			pfs = f; pls = l; polls++;
		}
	}
	printf("%u lectures/s ; fronts TFS %u/s, TLS %u/s ; sous-débit vu %u fois\n", polls, fs, ls, ue);
	printf("(trame attendue : 8000/s ; si les lectures sont trop lentes, valeur sous-estimée)\n");
}

/* La trame dure 256 coups d'horloge (2,048 MHz / 8 kHz) : la part du temps où
 * la synchro est haute donne sa largeur en bits. */
static void fs(void)
{
	uint32_t m = iomuxc[MUX_SD2_DAT1 / 4];
	iomuxc[MUX_SD2_DAT1 / 4] = m | 0x10;			/* SION */
	unsigned long n = 0, hi = 0, rise = 0, fall = 0;
	unsigned prev = 0, run = 0, runmin = ~0u, runmax = 0;
	double end = now() + 1.0;
	while (now() < end)
		for (int i = 0; i < 1000; i++) {
			unsigned v = gpio1[GPIO_PSR / 4] >> FS_BIT & 1;
			n++; hi += v;
			if (v && !prev) { rise++; run = 0; }
			if (v) run++;
			if (!v && prev) {
				fall++;
				if (run < runmin) runmin = run;
				if (run > runmax) runmax = run;
			}
			prev = v;
		}
	iomuxc[MUX_SD2_DAT1 / 4] = m;
	printf("%lu lectures, %lu fronts montants, %lu descendants en 1 s\n", n, rise, fall);
	if (!rise) { printf("pad figé à %u : pas de synchro lisible\n", prev); return; }
	double duty = (double)hi / n;
	printf("haute %.2f %% du temps → %.1f bits sur 256 (%s)\n", duty * 100, duty * 256,
	       duty < 0.5 ? "impulsion positive" : "impulsion négative, active basse");
	printf("impulsion : %u à %u lectures (%.2f µs par lecture)\n", runmin, runmax, 1e6 / n);
}

int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	ssi = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SSI2_BASE);
	mux = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, AUDMUX_BASE);
	iomuxc = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IOMUXC_BASE);
	gpio1 = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO1_BASE);
	if (ssi == MAP_FAILED || mux == MAP_FAILED || iomuxc == MAP_FAILED || gpio1 == MAP_FAILED) {
		perror("mmap"); return 1;
	}
	if (argc == 2 && !strcmp(argv[1], "fs")) { fs(); return 0; }
	if (argc == 1) { dump(); return 0; }
	if (!strcmp(argv[1], "rate")) { rate(); return 0; }
	if (!strcmp(argv[1], "set") && argc == 4) {
		int off = reg_off(argv[2]);
		if (off < 0) { fprintf(stderr, "registre inconnu\n"); return 2; }
		uint32_t v = strtoul(argv[3], NULL, 0);
		printf("%s : 0x%08x → 0x%08x\n", argv[2], r(off), v);
		ssi[off / 4] = v;
		printf("relu : 0x%08x\n", r(off));
		return 0;
	}
	fprintf(stderr, "usage : ssireg [rate | fs | set REG VAL]\n");
	return 2;
}
