/*
 * dvf99 — premiers pas avec la DVF9919 (base DECT + audio) du Gigaset Maxwell 10
 * sous Linux mainline, sur /dev/ttymxc2 (UART3, 115200 8N1, sans contrôle de flux).
 *
 *   dvf99 [-d TTY] [-l JOURNAL] console   console du U-Boot de la DVF99 (ligne à ligne)
 *   dvf99 [-d TTY] [-l JOURNAL] boot      démarre la pile cordless (« run bootm »)
 *   dvf99 [-d TTY] [-l JOURNAL] sniff     décode les trames CMBS reçues
 *   dvf99 [-d TTY] [-l JOURNAL] init      boot si besoin, puis Hello, SYS_START, version
 *   dvf99 -f CAPTURE sniff                décode un fichier brut (capture UART)
 *
 * Sous Android, CMBSServer pulse le reset de la DVF99, envoie « run bootm » à son
 * U-Boot, puis parle CMBS. Sous mainline, personne ne le fait : la DVF99 reste
 * dans son U-Boot (elle fournit quand même l'horloge du switch Ethernet).
 * Voir docs/dect-cmbs.md.
 *
 * Garde-fous : la console refuse les commandes U-Boot qui écrivent dans la flash
 * ou l'environnement du module (saveenv, upgrade, erase…) sauf avec -U.
 * Le reset (GPIO4_IO22) n'est jamais touché ici.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SYNC 0xDADADADAu
#define HDR_LEN 8
#define MAX_PARAMS 1024

static FILE *journal;
static int unsafe;

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void out(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	if (journal) {
		va_start(ap, fmt);
		vfprintf(journal, fmt, ap);
		va_end(ap);
		fflush(journal);
	}
}

static int open_tty(const char *path)
{
	int fd = open(path, O_RDWR | O_NOCTTY);
	if (fd < 0) { perror(path); exit(1); }
	struct termios t;
	if (tcgetattr(fd, &t) < 0) { perror("tcgetattr"); exit(1); }
	cfmakeraw(&t);
	cfsetispeed(&t, B115200);
	cfsetospeed(&t, B115200);
	t.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB);
	t.c_cflag |= CLOCAL | CREAD | CS8;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSANOW, &t) < 0) { perror("tcsetattr"); exit(1); }
	tcflush(fd, TCIOFLUSH);
	return fd;
}

/* ---------- décodage CMBS ------------------------------------------------ */

/*
 * Identifiants : événements CMBS (u16_EventID) et commandes de lien, codées
 * 0xFF00 | n (relevé par désassemblage de cmbs_int_cmd_Send). Seuls ceux qu'on
 * utilise ou qu'on a observés sont listés.
 */
#define CMD_HELLO		0xFF01
#define CMD_HELLO_RPLY		0xFF02
#define CMD_FLOW_NOK		0xFF03
#define CMD_FLOW_RESTART	0xFF04
#define CMD_RESET		0xFF05
#define EV_FW_VERSION_GET	23
#define EV_FW_VERSION_GET_RES	24
#define EV_SYS_START		25
#define EV_SYS_START_RES	26
#define EV_TARGET_UP		125

static const char *event_name(unsigned id)
{
	switch (id) {
	case CMD_HELLO:        return "CMBS_CMD_HELLO";
	case CMD_HELLO_RPLY:   return "CMBS_CMD_HELLO_RPLY";
	case CMD_FLOW_NOK:     return "CMBS_CMD_FLOW_NOK";
	case CMD_FLOW_RESTART: return "CMBS_CMD_FLOW_RESTART";
	case CMD_RESET:        return "CMBS_CMD_RESET";
	case 23:  return "CMBS_EV_DSR_FW_VERSION_GET";
	case 24:  return "CMBS_EV_DSR_FW_VERSION_GET_RES";
	case 25:  return "CMBS_EV_DSR_SYS_START";
	case 29:  return "CMBS_EV_DSR_SYS_STATUS";
	case 30:  return "CMBS_EV_DSR_SYS_LOG";
	case 16:  return "CMBS_EV_DSR_PARAM_SET_RES";
	case 26:  return "CMBS_EV_DSR_SYS_START_RES";
	case 125: return "CMBS_EV_DSR_TARGET_UP";
	case 239: return "CMBS_EV_DSR_AFE_ENDPOINT_ENABLE_RES";
	case 245: return "CMBS_EV_DSR_AFE_AUX_MEASUREMENT_RES";
	default:  return "?";
	}
}

static void hexdump(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i += 16) {
		out("      %04zx ", i);
		for (size_t j = i; j < i + 16; j++)
			if (j < n) out(" %02x", p[j]); else out("   ");
		out("  ");
		for (size_t j = i; j < i + 16 && j < n; j++)
			out("%c", p[j] >= 0x20 && p[j] < 0x7f ? p[j] : '.');
		out("\n");
	}
}

/*
 * Automate : texte libre (console U-Boot, messages) jusqu'au mot de synchro
 * DA DA DA DA, puis en-tête de 8 octets (u16 LE : longueur totale, n° de paquet,
 * événement, longueur des paramètres), puis les paramètres (IE).
 */
struct decoder {
	uint32_t window;
	int state;		/* 0 texte, 1 en-tête, 2 paramètres */
	uint8_t hdr[HDR_LEN];
	size_t got, need;
	uint8_t params[MAX_PARAMS];
	char text[256];
	size_t tlen;
	unsigned frames;
	unsigned last_ev;
	int prompt;		/* invite « DVF99 # » du U-Boot du module vue */
	double t0;
};

static void flush_text(struct decoder *d)
{
	if (!d->tlen) return;
	d->text[d->tlen] = 0;
	if (strstr(d->text, "DVF99 #")) d->prompt = 1;
	out("[%8.3f] texte : %s\n", now() - d->t0, d->text);
	d->tlen = 0;
}

static void feed(struct decoder *d, uint8_t c)
{
	switch (d->state) {
	case 0:
		d->window = (d->window << 8) | c;
		if (d->window == SYNC) {
			/* retirer de la ligne de texte les 3 premiers octets de synchro */
			d->tlen = d->tlen >= 3 ? d->tlen - 3 : 0;
			flush_text(d);
			d->state = 1; d->got = 0; d->window = 0;
			return;
		}
		if (c == '\n' || c == '\r') { flush_text(d); return; }
		if (d->tlen < sizeof d->text - 1)
			d->text[d->tlen++] = c >= 0x20 && c < 0x7f ? (char)c : '.';
		return;
	case 1:
		d->hdr[d->got++] = c;
		if (d->got < HDR_LEN) return;
		{
			unsigned total = d->hdr[0] | d->hdr[1] << 8;
			unsigned nr = d->hdr[2] | d->hdr[3] << 8;
			unsigned ev = d->hdr[4] | d->hdr[5] << 8;
			unsigned plen = d->hdr[6] | d->hdr[7] << 8;
			out("[%8.3f] CMBS  paquet %u  événement %u (0x%04x) %s  "
			    "longueur totale %u, paramètres %u\n",
			    now() - d->t0, nr, ev, ev, event_name(ev), total, plen);
			d->frames++;
			d->last_ev = ev;
			if (plen > MAX_PARAMS) {
				out("      longueur de paramètres incohérente, resynchronisation\n");
				d->state = 0; return;
			}
			d->need = plen; d->got = 0;
			d->state = plen ? 2 : 0;
		}
		return;
	case 2:
		d->params[d->got++] = c;
		if (d->got == d->need) {
			hexdump(d->params, d->need);
			d->state = 0;
		}
		return;
	}
}

/* ---------- modes ----------------------------------------------------------- */

static int read_for(int fd, struct decoder *d, double seconds)
{
	/* lit et décode pendant « seconds » ; renvoie 1 dès qu'une trame CMBS est vue */
	double end = now() + seconds;
	unsigned frames0 = d->frames;
	while (now() < end) {
		struct pollfd p = { fd, POLLIN, 0 };
		int ms = (int)((end - now()) * 1000);
		if (poll(&p, 1, ms > 0 ? ms : 0) <= 0) continue;
		uint8_t buf[256];
		ssize_t n = read(fd, buf, sizeof buf);
		for (ssize_t i = 0; i < n; i++)
			feed(d, buf[i]);
		if (d->frames > frames0) return 1;
	}
	flush_text(d);
	return 0;
}

/* envoie une trame CMBS : synchro, en-tête petit-boutiste (n° de paquet 0,
 * comme l'hôte DSPG), paramètres */
static int send_msg(int fd, struct decoder *d, unsigned ev, const uint8_t *p, unsigned n)
{
	uint8_t b[4 + HDR_LEN + MAX_PARAMS] = { 0xda, 0xda, 0xda, 0xda };
	unsigned total = HDR_LEN + n;
	b[4] = total & 0xff; b[5] = total >> 8;
	b[6] = 0; b[7] = 0;
	b[8] = ev & 0xff; b[9] = ev >> 8;
	b[10] = n & 0xff; b[11] = n >> 8;
	if (n) memcpy(b + 12, p, n);
	out("[%8.3f] envoi CMBS : événement %u (0x%04x) %s, %u octet(s) de paramètres\n",
	    now() - d->t0, ev, ev, event_name(ev), n);
	return write(fd, b, 4 + total) == (ssize_t)(4 + total) ? 0 : -1;
}

/* attend l'événement « ev » ; renvoie 1 s'il arrive avant « seconds » */
static int wait_event(int fd, struct decoder *d, unsigned ev, double seconds)
{
	double end = now() + seconds;
	while (now() < end)
		if (read_for(fd, d, end - now()) && d->last_ev == ev)
			return 1;
	return 0;
}

static int dangerous(const char *line)
{
	static const char *bad[] = { "saveenv", "upgrade", "erase", "protect", "env default",
		"destroyenv", "sf ", "nand ", "mmc write", "cp.b", "loady", "loadb", "loadx", NULL };
	for (int i = 0; bad[i]; i++)
		if (strstr(line, bad[i])) return 1;
	return 0;
}

static int mode_console(int fd, struct decoder *d)
{
	out("Console DVF99 : une commande par ligne, Ctrl-D pour quitter.%s\n",
	    unsafe ? " (garde-fous désactivés)" : "");
	char line[512];
	for (;;) {
		struct pollfd p[2] = { { fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
		if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; return 1; }
		if (p[0].revents & POLLIN) read_for(fd, d, 0.05);
		if (p[1].revents & (POLLIN | POLLHUP)) {
			if (!fgets(line, sizeof line, stdin)) break;
			line[strcspn(line, "\r\n")] = 0;
			if (!unsafe && dangerous(line)) {
				out("REFUSÉ : « %s » écrit dans la flash ou l'environnement du module (-U pour forcer)\n", line);
				continue;
			}
			if (journal) fprintf(journal, "[%8.3f] envoi : %s\n", now() - d->t0, line);
			dprintf(fd, "%s\r", line);
		}
	}
	return 0;
}

static int mode_boot(int fd, struct decoder *d)
{
	out("1) état de la DVF99\n");
	if (write(fd, "\r", 1) != 1) { perror("write"); return 1; }
	if (read_for(fd, d, 2.0) && d->frames) {
		out("=> trames CMBS reçues : la pile cordless tourne déjà\n");
		return 0;
	}
	out("2) envoi de « run bootm » (démarrage de la pile cordless)\n");
	if (journal) fprintf(journal, "[%8.3f] envoi : run bootm\n", now() - d->t0);
	dprintf(fd, "run bootm\r");
	if (read_for(fd, d, 20.0)) {
		out("=> trame CMBS reçue : pile cordless démarrée\n");
		return 0;
	}
	out("=> pas de trame CMBS en 20 s. La pile attend peut-être un « hello » de l'hôte,\n"
	    "   ou « run bootm » n'est pas la bonne commande : voir le texte ci-dessus.\n");
	return 2;
}

static int mode_init(int fd, struct decoder *d)
{
	out("1) état de la DVF99\n");
	if (write(fd, "\r", 1) != 1) { perror("write"); return 1; }
	read_for(fd, d, 2.0);
	if (d->prompt) {
		out("   U-Boot du module : envoi de « run bootm »\n");
		dprintf(fd, "run bootm\r");
		if (!wait_event(fd, d, EV_TARGET_UP, 20.0)) {
			out("=> pas de TARGET_UP en 20 s\n");
			return 2;
		}
	} else {
		out("   pas d'invite U-Boot : pile cordless supposée déjà démarrée\n");
	}

	out("2) Hello (sans configuration TDM)\n");
	send_msg(fd, d, CMD_HELLO, NULL, 0);
	if (!wait_event(fd, d, CMD_HELLO_RPLY, 10.0)) {
		out("=> pas de HELLO_RPLY en 10 s\n");
		return 3;
	}
	out("3) SYS_START\n");
	send_msg(fd, d, EV_SYS_START, NULL, 0);
	if (!wait_event(fd, d, EV_SYS_START_RES, 10.0))
		out("=> pas de SYS_START_RES en 10 s\n");
	out("4) version du firmware\n");
	send_msg(fd, d, EV_FW_VERSION_GET, NULL, 0);
	if (!wait_event(fd, d, EV_FW_VERSION_GET_RES, 5.0))
		out("=> pas de FW_VERSION_GET_RES en 5 s\n");
	out("5) écoute 10 s des événements spontanés\n");
	read_for(fd, d, 10.0);
	return 0;
}

static int mode_sniff(int fd, struct decoder *d)
{
	out("Décodage (Ctrl-C pour arrêter)\n");
	for (;;)
		if (read_for(fd, d, 3600.0) < 0) return 1;
}

static int sniff_file(const char *path, struct decoder *d)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return 1; }
	int c;
	while ((c = fgetc(f)) != EOF) feed(d, (uint8_t)c);
	flush_text(d);
	fclose(f);
	out("%u trame(s) CMBS\n", d->frames);
	return 0;
}

int main(int argc, char **argv)
{
	const char *tty = "/dev/ttymxc2", *file = NULL;
	int opt;
	while ((opt = getopt(argc, argv, "d:l:f:U")) != -1) {
		switch (opt) {
		case 'd': tty = optarg; break;
		case 'l': journal = fopen(optarg, "a");
			  if (!journal) { perror(optarg); return 1; } break;
		case 'f': file = optarg; break;
		case 'U': unsafe = 1; break;
		default: goto usage;
		}
	}
	if (optind != argc - 1) goto usage;
	struct decoder d = { .t0 = now() };
	const char *mode = argv[optind];
	if (file) {
		if (strcmp(mode, "sniff")) goto usage;
		return sniff_file(file, &d);
	}
	int fd = open_tty(tty);
	if (!strcmp(mode, "console")) return mode_console(fd, &d);
	if (!strcmp(mode, "boot")) return mode_boot(fd, &d);
	if (!strcmp(mode, "sniff")) return mode_sniff(fd, &d);
	if (!strcmp(mode, "init")) return mode_init(fd, &d);
usage:
	fprintf(stderr, "usage : dvf99 [-d TTY] [-l JOURNAL] [-U] console|boot|sniff|init\n"
			"        dvf99 -f CAPTURE sniff\n");
	return 2;
}
