/*
 * dvf99 — premiers pas avec la DVF9919 (base DECT + audio) du Gigaset Maxwell 10
 * sous Linux mainline, sur /dev/ttymxc2 (UART3, 115200 8N1, sans contrôle de flux).
 *
 *   dvf99 [-d TTY] [-l JOURNAL] console   console du U-Boot de la DVF99 (ligne à ligne)
 *   dvf99 [-d TTY] [-l JOURNAL] boot      démarre la pile cordless (« run bootm »)
 *   dvf99 [-d TTY] [-l JOURNAL] sniff     décode les trames CMBS reçues
 *   dvf99 [-d TTY] [-l JOURNAL] [-e NVRAM] init
 *                                         boot si besoin, puis Hello et SYS_START
 *
 * -T 0|1|-1 : configuration TDM du HELLO (0 = DVF99 maître, comme Android ;
 * 1 = esclave ; -1 = HELLO vide, comportement des versions précédentes).
 *
 * -e NVRAM : EEPROM émulée de la base (copie de /data/cmbs/nvram.bin d'Android,
 * propre à l'appareil). Avec elle, dvf99 répond aux demandes PARAM_AREA_GET de
 * la DVF99 et enregistre ses PARAM_AREA_SET dans ce fichier (travailler sur une
 * COPIE, garder l'original de côté).
 *   dvf99 -f CAPTURE sniff                décode un fichier brut (capture UART)
 *   dvf99 [-e NVRAM] [-a 0..3] [-t N] [-s] tone
 *                                         essai audio : canal AFE, AMP_OUTx ← DAC0,
 *                                         tonalité N (0 = invitation à numéroter) 5 s ;
 *                                         -a : une seule sortie (défaut : les 4) ;
 *                                         -s : démarre aussi le canal média (TDM)
 *                                         -S N : source des amplis (E_CMBS_AFE_CHANNEL,
 *                                         17 = DAC0 par défaut ; 21 SINGIN, 22/23
 *                                         DIFFIN0/1 : liaison micro → ampli, sans tonalité)
 *   dvf99 [-e NVRAM] [-S SRC] [-g GAIN] [-t N|-1] [-w S] [-k] speaker
 *                                         séquence haut-parleur d'Android : woofer
 *                                         AMP_OUT2/3 en pont ← DAC0, slots 0x0F00 WB,
 *                                         tweeter slots 0x00F0, ampli via GPIO D10 ;
 *                                         -t -1 : pas de tonalité (son TDM) ;
 *                                         -g 32..36 : 0 à −24 dB (défaut 34 = −12) ;
 *                                         -w : durée ; -k : laisse le chemin ouvert ;
 *                                         -S 22 : micro (DIFFIN0) → HP, sans TDM
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
#include <sys/stat.h>
#include <unistd.h>

#define SYNC 0xDADADADAu
#define HDR_LEN 8
#define MAX_PARAMS 1024

static FILE *journal;
static int unsafe;
static int tdm_type = 0;	/* -T : 0 maître (Android), 1 esclave, -1 sans config */

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
#define EV_PARAM_AREA_GET	76
#define EV_PARAM_AREA_GET_RES	77
#define EV_PARAM_AREA_SET	78
#define EV_PARAM_AREA_SET_RES	79
#define IE_RESPONSE		22
#define IE_PARAMETER_AREA	26

static const char *ie_name(unsigned t)
{
	switch (t) {
	case 1:  return "CALLINSTANCE";
	case 10: return "MEDIACHANNEL";
	case 2:  return "CALLERPARTY";
	case 3:  return "CALLERNAME";
	case 4:  return "CALLEDPARTY";
	case 5:  return "CALLPROGRESS";
	case 8:  return "CALLRELEASE_REASON";
	case 9:  return "CALLSTATE";
	case 12: return "MEDIADESCRIPTOR";
	case 13: return "TONE";
	case 22: return "RESPONSE";
	case 26: return "PARAMETER_AREA";
	case 28: return "HANDSETS";
	case 32: return "DATA";
	case 82: return "AFE_ENDPOINT_CONNECT";
	case 83: return "AFE_ENDPOINT";
	case 84: return "AFE_ENDPOINT_GAIN";
	case 88: return "AFE_RESOURCE_TYPE";
	case 89: return "AFE_INSTANCE_NUM";
	case 91: return "GPIO_ID";
	case 92: return "GPIO_MODE";
	case 93: return "GPIO_VALUE";
	case 94: return "GPIO_PULL_TYPE";
	case 95: return "GPIO_PULL_ENA";
	case 96: return "GPIO_ENA";
	default: return NULL;
	}
}

static void out(const char *fmt, ...);

/* liste des IE d'une trame : type, nom, longueur, début de la valeur */
static void ie_print(const uint8_t *p, unsigned n)
{
	for (unsigned i = 0; i + 4 <= n; ) {
		unsigned t = p[i] | p[i + 1] << 8, l = p[i + 2] | p[i + 3] << 8;
		if (i + 4 + l > n) { out("      (IE tronqué)\n"); return; }
		const char *nm = ie_name(t);
		out("      IE %u%s%s%s, %u octet(s) :", t, nm ? " (" : "", nm ? nm : "", nm ? ")" : "", l);
		for (unsigned j = 0; j < l && j < 24; j++) out(" %02x", p[i + 4 + j]);
		out(l > 24 ? " …\n" : "\n");
		i += 4 + l;
	}
}

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
	case 45:  return "CMBS_EV_DCM_CALL_STATE";
	case 46:  return "CMBS_EV_DEE_CALL_ESTABLISH";
	case 47:  return "CMBS_EV_DEE_CALL_PROGRESS";
	case 48:  return "CMBS_EV_DEE_CALL_ANSWER";
	case 49:  return "CMBS_EV_DEE_CALL_RELEASE";
	case 50:  return "CMBS_EV_DEE_CALL_RELEASECOMPLETE";
	case 51:  return "CMBS_EV_DEE_CALL_INBANDINFO";
	case 52:  return "CMBS_EV_DEE_CALL_MEDIA_OFFER";
	case 53:  return "CMBS_EV_DEE_CALL_MEDIA_OFFER_RES";
	case 54:  return "CMBS_EV_DEE_CALL_MEDIA_UPDATE";
	case 61:  return "CMBS_EV_DEM_CHANNEL_START";
	case 62:  return "CMBS_EV_DEM_CHANNEL_START_RES";
	case 65:  return "CMBS_EV_DEM_CHANNEL_STOP";
	case 66:  return "CMBS_EV_DEM_CHANNEL_STOP_RES";
	case 67:  return "CMBS_EV_DEM_TONE_START";
	case 68:  return "CMBS_EV_DEM_TONE_START_RES";
	case 69:  return "CMBS_EV_DEM_TONE_STOP";
	case 70:  return "CMBS_EV_DEM_TONE_STOP_RES";
	case 236: return "CMBS_EV_DSR_AFE_ENDPOINT_CONNECT";
	case 237: return "CMBS_EV_DSR_AFE_ENDPOINT_CONNECT_RES";
	case 238: return "CMBS_EV_DSR_AFE_ENDPOINT_ENABLE";
	case 240: return "CMBS_EV_DSR_AFE_ENDPOINT_DISABLE";
	case 241: return "CMBS_EV_DSR_AFE_ENDPOINT_DISABLE_RES";
	case 242: return "CMBS_EV_DSR_AFE_ENDPOINT_GAIN";
	case 243: return "CMBS_EV_DSR_AFE_ENDPOINT_GAIN_RES";
	case 244: return "CMBS_EV_DSR_AFE_AUX_MEASUREMENT";
	case 246: return "CMBS_EV_DSR_AFE_AUX_MEASUREMENT_RESULT";
	case 247: return "CMBS_EV_DSR_AFE_CHANNEL_ALLOCATE";
	case 248: return "CMBS_EV_DSR_AFE_CHANNEL_ALLOCATE_RES";
	case 249: return "CMBS_EV_DSR_AFE_CHANNEL_DEALLOCATE";
	case 250: return "CMBS_EV_DSR_AFE_CHANNEL_DEALLOCATE_RES";
	case 254: return "CMBS_EV_DSR_GPIO_ENABLE";
	case 255: return "CMBS_EV_DSR_GPIO_ENABLE_RES";
	case 256: return "CMBS_EV_DSR_GPIO_DISABLE";
	case 257: return "CMBS_EV_DSR_GPIO_DISABLE_RES";
	case 258: return "CMBS_EV_DSR_GPIO_CONFIG_SET";
	case 259: return "CMBS_EV_DSR_GPIO_CONFIG_SET_RES";
	case 260: return "CMBS_EV_DSR_GPIO_CONFIG_GET";
	case 261: return "CMBS_EV_DSR_GPIO_CONFIG_GET_RES";
	case 274: return "(hors énumération 03.64 : données du combiné, firmware Gigaset)";
	case 7:   return "CMBS_EV_DSR_HS_REGISTERED";
	case 8:   return "CMBS_EV_DSR_HS_IN_RANGE";
	case 74:  return "CMBS_EV_DSR_PARAM_UPDATED";
	case 75:  return "CMBS_EV_DSR_PARAM_AREA_UPDATED";
	case 76:  return "CMBS_EV_DSR_PARAM_AREA_GET";
	case 77:  return "CMBS_EV_DSR_PARAM_AREA_GET_RES";
	case 78:  return "CMBS_EV_DSR_PARAM_AREA_SET";
	case 79:  return "CMBS_EV_DSR_PARAM_AREA_SET_RES";
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
	unsigned want;		/* événement attendu par wait_event */
	int want_seen;
	uint8_t wparams[MAX_PARAMS];	/* paramètres de l'événement attendu */
	unsigned wlen;
	uint8_t hello[8];	/* paramètres du dernier HELLO_RPLY */
	unsigned hello_len;
	int prompt;		/* invite « DVF99 # » du U-Boot du module vue */
	double t0;
	int fd;			/* -1 pour un fichier de capture */
	int eep_fd;		/* EEPROM émulée (-e), -1 sinon */
	uint8_t *eep;
	size_t eep_size;
};

static void on_frame(struct decoder *d, unsigned ev, const uint8_t *p, unsigned n);

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
			if (!plen) on_frame(d, ev, NULL, 0);
		}
		return;
	case 2:
		d->params[d->got++] = c;
		if (d->got == d->need) {
			hexdump(d->params, d->need);
			ie_print(d->params, d->need);
			d->state = 0;
			on_frame(d, d->last_ev, d->params, d->need);
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
	d->want = ev; d->want_seen = 0;
	while (now() < end && !d->want_seen)
		read_for(fd, d, end - now());
	d->want = 0;
	return d->want_seen;
}

static unsigned le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t *p, unsigned v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

/* cherche l'IE « type » ; renvoie sa valeur et sa longueur */
static const uint8_t *ie_find(const uint8_t *p, unsigned n, unsigned type, unsigned *len)
{
	for (unsigned i = 0; i + 4 <= n; ) {
		unsigned t = le16(p + i), l = le16(p + i + 2);
		if (i + 4 + l > n) break;
		if (t == type) { *len = l; return p + i + 4; }
		i += 4 + l;
	}
	return NULL;
}

/*
 * Réponses automatiques de l'hôte (relevées dans app_OnParamAreaGet/Set de
 * cmbs_host) : la DVF99 lit et écrit son EEPROM, émulée par l'hôte.
 *   GET_RES = IE PARAMETER_AREA (zone u8, offset u32, longueur u16, données)
 *             + IE RESPONSE (0 = OK, 1 = erreur)
 *   SET_RES = IE RESPONSE
 */
static void on_frame(struct decoder *d, unsigned ev, const uint8_t *p, unsigned n)
{
	if (d->want && ev == d->want) {
		d->wlen = n;
		if (n) memcpy(d->wparams, p, n);
		d->want_seen = 1;
	}
	if (ev == CMD_HELLO_RPLY) {
		d->hello_len = n < sizeof d->hello ? n : sizeof d->hello;
		memcpy(d->hello, p, d->hello_len);
	}
	if (d->fd < 0 || (ev != EV_PARAM_AREA_GET && ev != EV_PARAM_AREA_SET))
		return;
	unsigned l = 0;
	const uint8_t *a = ie_find(p, n, IE_PARAMETER_AREA, &l);
	if (!a || l < 7) { out("      PARAM_AREA sans IE exploitable, ignoré\n"); return; }
	unsigned area = a[0], len = le16(a + 5);
	uint32_t off = le32(a + 1);
	int ok = d->eep && area == 0 && (uint64_t)off + len <= d->eep_size;
	uint8_t r[16 + MAX_PARAMS];
	unsigned rn = 0;

	if (ev == EV_PARAM_AREA_GET) {
		unsigned dl = ok ? len : 0;
		if (12 + dl + 5 > sizeof r) { ok = 0; dl = 0; }
		put16(r, IE_PARAMETER_AREA); put16(r + 2, 7 + dl);
		r[4] = area; put32(r + 5, off); put16(r + 9, dl);
		if (dl) memcpy(r + 11, d->eep + off, dl);
		rn = 11 + dl;
		out("      → lecture EEPROM zone %u, offset 0x%x, %u octets : %s\n",
		    area, off, len, ok ? "fournie" : d->eep ? "hors limites" : "pas d'EEPROM (-e)");
	} else {
		if (ok && l >= 7 + len) {
			memcpy(d->eep + off, a + 7, len);
			if (pwrite(d->eep_fd, a + 7, len, off) != (ssize_t)len || fsync(d->eep_fd) < 0) {
				perror("écriture EEPROM"); ok = 0;
			}
		} else ok = 0;
		out("      → écriture EEPROM zone %u, offset 0x%x, %u octets : %s\n",
		    area, off, len, ok ? "enregistrée" : "refusée");
	}
	put16(r + rn, IE_RESPONSE); put16(r + rn + 2, 1); r[rn + 4] = ok ? 0 : 1;
	rn += 5;
	send_msg(d->fd, d, ev == EV_PARAM_AREA_GET ? EV_PARAM_AREA_GET_RES : EV_PARAM_AREA_SET_RES, r, rn);
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

static int do_init(int fd, struct decoder *d)
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

	if (tdm_type < 0) {
		out("2) Hello (sans configuration TDM)\n");
		send_msg(fd, d, CMD_HELLO, NULL, 0);
	} else {
		/* comme Android (tcx_TDMConfig) : ST_TDM_CONFIG {type, vitesse
		 * PCM_2048 = 3, synchro LONG = 3, slots 0xFFFF} puis contrôle de flux
		 * NONE. Type 0 = la DVF99 est maître de l'horloge TDM. */
		uint8_t h[6] = { tdm_type, 3, 3, 0xff, 0xff, 0 };
		out("2) Hello, TDM %s, PCM 2048 kHz, synchro longue\n", tdm_type ? "esclave" : "maître");
		send_msg(fd, d, CMD_HELLO, h, sizeof h);
	}
	if (!wait_event(fd, d, CMD_HELLO_RPLY, 10.0)) {
		out("=> pas de HELLO_RPLY en 10 s\n");
		return 3;
	}
	if (d->hello_len >= 4)
		out("   pile cordless version %x.%02x, build %x\n", d->hello[1], d->hello[0], le16(d->hello + 2));
	out("3) SYS_START\n");
	send_msg(fd, d, EV_SYS_START, NULL, 0);
	if (!wait_event(fd, d, EV_SYS_START_RES, 10.0))
		out("=> pas de SYS_START_RES en 10 s\n");
	/* La DVF99 relit toute son EEPROM (blocs de 0xF0) après SYS_START et refuse
	 * les requêtes AFE tant qu'elle n'a pas fini (erreur 5) : attendre 3 s de
	 * silence, 90 s au plus. */
	out("   attente de la fin de l'initialisation (3 s sans trame)\n");
	double end = now() + 90.0;
	unsigned quiet = 0;
	while (now() < end && quiet < 3) {
		unsigned f = d->frames;
		read_for(fd, d, 1.0);
		quiet = d->frames == f ? quiet + 1 : 0;
	}
	out("   initialisation %s\n", quiet >= 3 ? "terminée" : "toujours en cours après 90 s");
	return 0;
}

static int mode_init(int fd, struct decoder *d)
{
	int r = do_init(fd, d);
	if (r) return r;
	out("4) écoute 60 s (réponses automatiques EEPROM si -e)\n");
	read_for(fd, d, 60.0);
	return 0;
}

/* ---------- audio : essai de tonalité sur le haut-parleur ------------------ */

#define EV_DEM_TONE_START		67
#define EV_DEM_TONE_START_RES		68
#define EV_DEM_TONE_STOP		69
#define EV_DEM_TONE_STOP_RES		70
#define EV_DEM_CHANNEL_START		61
#define EV_DEM_CHANNEL_START_RES	62
#define EV_AFE_ENDPOINT_CONNECT		236
#define EV_AFE_ENDPOINT_ENABLE		238
#define EV_AFE_ENDPOINT_DISABLE		240
#define EV_AFE_ENDPOINT_GAIN		242
#define EV_AFE_CHANNEL_ALLOCATE		247
#define EV_AFE_CHANNEL_DEALLOCATE	249
#define IE_MEDIACHANNEL			10
#define IE_MEDIADESCRIPTOR		12
#define IE_TONE				13
#define IE_AFE_ENDPOINT_CONNECT		82
#define IE_AFE_ENDPOINT			83
#define IE_AFE_ENDPOINT_GAIN		84
#define IE_AFE_RESOURCE_TYPE		88
#define IE_AFE_INSTANCE_NUM		89
#define EV_GPIO_ENABLE			254
#define EV_GPIO_DISABLE			256
#define EV_GPIO_CONFIG_SET		258
#define IE_GPIO_ID			91
#define IE_GPIO_MODE			92
#define IE_GPIO_VALUE			93
#define IE_GPIO_PULL_TYPE		94
#define IE_GPIO_PULL_ENA		95

/* E_CMBS_AFE_CHANNEL tel que le firmware de la Maxwell 10 le numérote (relevé
 * dans la bibliothèque hôte d'Android). Attention : l'énumération des en-têtes
 * CMBS 03.64 génériques n'a pas DIGMIC1/2 et est décalée de 2 à partir de 15. */
#define AFE_ADC_IN_DIFFIN0		9
#define AFE_AMPOUT_IN_DAC0		17
#define AFE_AMPOUT_IN_DAC1		18
#define AFE_AMPOUT_IN_DAC0_INV		19
#define AFE_AMPOUT_IN_SINGIN		21
#define AFE_AMPOUT_IN_DIFFIN0		22
#define AFE_AMPOUT_IN_DIFFIN1		23
#define AFE_AMPOUT_IN_MUTE		24
#define AFE_AMP_OUT0			25
#define AFE_AMP_SINGIN0			29
#define AFE_AMP_DIFFIN0			31
#define AFE_AMP_DIFFIN1			32
#define AFE_MIC0			36
#define AFE_MIC1			37
#define AFE_OUT_GAIN_0DB		32	/* puis 33 = −6 dB … 36 = −24 dB */

/* ampli du haut-parleur : GPIO D10 de la DVF99 (banc D = 3) */
#define SPK_GPIO_BANK			3
#define SPK_GPIO			10

static unsigned ie_add(uint8_t *b, unsigned at, unsigned type, const uint8_t *v, unsigned n)
{
	put16(b + at, type); put16(b + at + 2, n);
	if (n) memcpy(b + at + 4, v, n);
	return at + 4 + n;
}

static unsigned ie_media_channel(uint8_t *b, unsigned at, uint32_t id, uint32_t param)
{
	uint8_t v[9];
	put32(v, id); put32(v + 4, param); v[8] = 0;	/* CMBS_MEDIA_TYPE_AUDIO_IOM */
	return ie_add(b, at, IE_MEDIACHANNEL, v, sizeof v);
}

/* envoie une requête, attend « ev + 1 », affiche l'IE RESPONSE ; 0 = OK */
static int request(int fd, struct decoder *d, unsigned ev, const uint8_t *p, unsigned n, const char *what)
{
	send_msg(fd, d, ev, p, n);
	d->want = ev + 1; d->want_seen = 0;
	double end = now() + 5.0;
	while (now() < end && !d->want_seen)
		read_for(fd, d, end - now());
	d->want = 0;
	if (!d->want_seen) { out("   %s : pas de réponse\n", what); return -1; }
	unsigned l;
	const uint8_t *r = ie_find(d->wparams, d->wlen, IE_RESPONSE, &l);
	int rc = r && l ? r[0] : -2;
	/* la valeur suit E_CMBS_RESPONSE (0 OK, 1 erreur) ou E_CMBS_RC (5 erreur
	 * générale…) selon la commande : on n'interprète que 0 */
	if (rc == -2) out("   %s : réponse sans IE RESPONSE\n", what);
	else if (rc == 0) out("   %s : OK\n", what);
	else out("   %s : REFUS (code %d)\n", what, rc);
	return rc;
}

static void set_gain(int fd, struct decoder *d, int ep, int steps)
{
	uint8_t b[16], v[3];
	v[0] = ep; v[1] = steps & 0xff; v[2] = (steps >> 8) & 0xff;	/* s16 petit-boutiste */
	unsigned n = ie_add(b, 0, IE_AFE_ENDPOINT_GAIN, v, 3);
	char what[48];
	snprintf(what, sizeof what, "AFE_ENDPOINT_GAIN %d = %d", ep, steps);
	request(fd, d, EV_AFE_ENDPOINT_GAIN, b, n, what);
}

static int mode_tone(int fd, struct decoder *d, int amp, int tone, int start_channel, int src)
{
	if (do_init(fd, d)) return 1;
	uint8_t b[64]; unsigned n; uint8_t v[4];

	out("5) allocation d'un canal AFE (ressource DAC/ADC)\n");
	v[0] = 0;
	n = ie_add(b, 0, IE_AFE_RESOURCE_TYPE, v, 1);
	if (request(fd, d, EV_AFE_CHANNEL_ALLOCATE, b, n, "AFE_CHANNEL_ALLOCATE")) return 4;
	unsigned l; uint32_t chan = 0;
	const uint8_t *mc = ie_find(d->wparams, d->wlen, IE_MEDIACHANNEL, &l);
	if (mc && l >= 4) chan = le32(mc);
	const uint8_t *in = ie_find(d->wparams, d->wlen, IE_AFE_INSTANCE_NUM, &l);
	out("   canal %u, instance %d\n", chan, in && l ? in[0] : -1);

	int first = amp < 0 ? 0 : amp, last = amp < 0 ? 3 : amp;
	for (int a = first; a <= last; a++) {
		out("6) AMP_OUT%d ← source %d, activation\n", a, src);
		v[0] = AFE_AMP_OUT0 + a; v[1] = src;
		n = ie_add(b, 0, IE_AFE_ENDPOINT_CONNECT, v, 2);
		request(fd, d, EV_AFE_ENDPOINT_CONNECT, b, n, "AFE_ENDPOINT_CONNECT");
		n = ie_add(b, 0, IE_AFE_ENDPOINT, v, 1);
		request(fd, d, EV_AFE_ENDPOINT_ENABLE, b, n, "AFE_ENDPOINT_ENABLE");
		/* gain de sortie : CMBS_AFE_OUT_SING_GAIN_0DB (32), le maximum */
		set_gain(fd, d, AFE_AMP_OUT0 + a, AFE_OUT_GAIN_0DB);
	}

	/* entrée analogique : il faut aussi activer son préampli et la
	 * polarisation micro, et lui donner du gain */
	int inamp = src == AFE_AMPOUT_IN_SINGIN ? AFE_AMP_SINGIN0 :
		    src == AFE_AMPOUT_IN_DIFFIN0 ? AFE_AMP_DIFFIN0 :
		    src == AFE_AMPOUT_IN_DIFFIN1 ? AFE_AMP_DIFFIN1 : -1;
	if (inamp >= 0) {
		int mic = src == AFE_AMPOUT_IN_DIFFIN1 ? AFE_MIC1 : AFE_MIC0;
		out("6b) préampli %d et polarisation micro %d, gain d'entrée +20 dB\n", inamp, mic);
		v[0] = inamp;
		n = ie_add(b, 0, IE_AFE_ENDPOINT, v, 1);
		request(fd, d, EV_AFE_ENDPOINT_ENABLE, b, n, "AFE_ENDPOINT_ENABLE (préampli)");
		v[0] = mic;
		n = ie_add(b, 0, IE_AFE_ENDPOINT, v, 1);
		request(fd, d, EV_AFE_ENDPOINT_ENABLE, b, n, "AFE_ENDPOINT_ENABLE (micro)");
		set_gain(fd, d, inamp, 16);					/* CMBS_AFE_IN_GAIN_20DB */
	}

	if (src < AFE_AMPOUT_IN_DAC0 || src > 20) {	/* pas un DAC */
		/* liaison analogique directe (entrée → ampli) : pas de canal média ni de
		 * tonalité, on maintient 10 s pour écouter */
		out("7) liaison analogique maintenue 10 s : parler dans le micro / écouter\n");
		read_for(fd, d, 10.0);
		goto release;
	}

	if (start_channel) {
		out("7) démarrage du canal (PCM linéaire NB, slots 0x0003)\n");
		uint8_t desc[4] = { 6, 2, 6, 5 };	/* codec, nombre, liste */
		n = ie_media_channel(b, 0, chan, 0x0003);
		n = ie_add(b, n, IE_MEDIADESCRIPTOR, desc, sizeof desc);
		request(fd, d, EV_DEM_CHANNEL_START, b, n, "DEM_CHANNEL_START");
	}

	out("8) tonalité %d pendant 5 s : écoute le haut-parleur\n", tone);
	v[0] = tone;
	n = ie_media_channel(b, 0, chan, 0);
	n = ie_add(b, n, IE_TONE, v, 1);
	request(fd, d, EV_DEM_TONE_START, b, n, "DEM_TONE_START");
	read_for(fd, d, 5.0);
	n = ie_media_channel(b, 0, chan, 0);
	request(fd, d, EV_DEM_TONE_STOP, b, n, "DEM_TONE_STOP");

release:
	out("9) libération\n");
	for (int a = first; a <= last; a++) {
		v[0] = AFE_AMP_OUT0 + a;
		n = ie_add(b, 0, IE_AFE_ENDPOINT, v, 1);
		request(fd, d, EV_AFE_ENDPOINT_DISABLE, b, n, "AFE_ENDPOINT_DISABLE");
	}
	n = ie_media_channel(b, 0, chan, 0);
	request(fd, d, EV_AFE_CHANNEL_DEALLOCATE, b, n, "AFE_CHANNEL_DEALLOCATE");
	return 0;
}

static unsigned ie_gpio_id(uint8_t *b, unsigned at)
{
	uint8_t v[5];
	put32(v, SPK_GPIO); v[4] = SPK_GPIO_BANK;	/* ST_GPIO_ID : u32 numéro, u8 banc */
	return ie_add(b, at, IE_GPIO_ID, v, sizeof v);
}

static unsigned ie_u8(uint8_t *b, unsigned at, unsigned type, unsigned val)
{
	uint8_t v = val;
	return ie_add(b, at, type, &v, 1);
}

static int afe_alloc(int fd, struct decoder *d, uint32_t *chan)
{
	uint8_t b[16];
	unsigned n = ie_u8(b, 0, IE_AFE_RESOURCE_TYPE, 0);	/* DAC/ADC */
	if (request(fd, d, EV_AFE_CHANNEL_ALLOCATE, b, n, "AFE_CHANNEL_ALLOCATE")) return -1;
	unsigned l;
	const uint8_t *mc = ie_find(d->wparams, d->wlen, IE_MEDIACHANNEL, &l);
	*chan = mc && l >= 4 ? le32(mc) : 0;
	out("   canal 0x%x\n", *chan);
	return 0;
}

static void afe_ep(int fd, struct decoder *d, unsigned ev, int ep, const char *what)
{
	uint8_t b[16];
	unsigned n = ie_u8(b, 0, IE_AFE_ENDPOINT, ep);
	char w[64];
	snprintf(w, sizeof w, "%s %d", what, ep);
	request(fd, d, ev, b, n, w);
}

static void afe_connect(int fd, struct decoder *d, int dst, int src)
{
	uint8_t b[16], v[2] = { dst, src };
	unsigned n = ie_add(b, 0, IE_AFE_ENDPOINT_CONNECT, v, 2);
	char w[64];
	snprintf(w, sizeof w, "AFE_ENDPOINT_CONNECT %d ← %d", dst, src);
	request(fd, d, EV_AFE_ENDPOINT_CONNECT, b, n, w);
}

/* DEM_CHANNEL_START comme la bibliothèque d'Android : descripteur média (codec
 * choisi + liste {PCM linéaire WB 5, NB 6}) puis canal + masque de slots TDM */
static void channel_start(int fd, struct decoder *d, uint32_t chan, uint32_t slots, int wb)
{
	uint8_t b[32], desc[4] = { wb ? 5 : 6, 2, 5, 6 };
	unsigned n = ie_add(b, 0, IE_MEDIADESCRIPTOR, desc, sizeof desc);
	n = ie_media_channel(b, n, chan, slots);
	char w[64];
	snprintf(w, sizeof w, "DEM_CHANNEL_START 0x%x slots 0x%04x", chan, slots);
	request(fd, d, EV_DEM_CHANNEL_START, b, n, w);
}

static void spk_amp(int fd, struct decoder *d, int on)
{
	uint8_t b[48]; unsigned n;
	if (on) {
		n = ie_gpio_id(b, 0);
		request(fd, d, EV_GPIO_ENABLE, b, n, "GPIO_ENABLE D10");
	}
	n = ie_gpio_id(b, 0);
	n = ie_u8(b, n, IE_GPIO_MODE, 1);		/* sortie */
	n = ie_u8(b, n, IE_GPIO_PULL_TYPE, 1);
	n = ie_u8(b, n, IE_GPIO_PULL_ENA, 0);
	n = ie_u8(b, n, IE_GPIO_VALUE, on);
	request(fd, d, EV_GPIO_CONFIG_SET, b, n, on ? "GPIO_CONFIG_SET D10 = 1 (ampli HP)" : "GPIO_CONFIG_SET D10 = 0");
	if (!on) {
		n = ie_gpio_id(b, 0);
		request(fd, d, EV_GPIO_DISABLE, b, n, "GPIO_DISABLE D10");
	}
}

/* Séquence « haut-parleur » de la bibliothèque hôte d'Android (woofer + tweeter) :
 * woofer = AMP_OUT2/AMP_OUT3 en pont (DAC0 / DAC0 inversé), slots TDM 8..11 ;
 * tweeter = second canal, slots 4..7, sans extrémité ; ampli sur GPIO D10.
 * -S autre que DAC0 : liaison analogique (ex. 22 = DIFFIN0 → micro dans le HP). */
static int mode_speaker(int fd, struct decoder *d, int src, int gain, int tone, int hold, int keep)
{
	if (do_init(fd, d)) return 1;
	int dac = src == AFE_AMPOUT_IN_DAC0;
	int o2 = AFE_AMP_OUT0 + 2, o3 = AFE_AMP_OUT0 + 3;
	uint32_t woofer, tweeter = 0;

	out("5) canal woofer\n");
	if (afe_alloc(fd, d, &woofer)) return 4;
	afe_connect(fd, d, o2, src);
	afe_connect(fd, d, o3, dac ? AFE_AMPOUT_IN_DAC0_INV : AFE_AMPOUT_IN_MUTE);
	afe_ep(fd, d, EV_AFE_ENDPOINT_ENABLE, o2, "AFE_ENDPOINT_ENABLE");
	afe_ep(fd, d, EV_AFE_ENDPOINT_ENABLE, o3, "AFE_ENDPOINT_ENABLE");
	set_gain(fd, d, o2, gain);
	set_gain(fd, d, o3, gain);

	int inamp = src == AFE_AMPOUT_IN_SINGIN ? AFE_AMP_SINGIN0 :
		    src == AFE_AMPOUT_IN_DIFFIN0 ? AFE_AMP_DIFFIN0 :
		    src == AFE_AMPOUT_IN_DIFFIN1 ? AFE_AMP_DIFFIN1 : -1;
	if (inamp >= 0) {
		int mic = src == AFE_AMPOUT_IN_DIFFIN1 ? AFE_MIC1 : AFE_MIC0;
		out("5b) préampli %d, polarisation micro %d, gain +20 dB\n", inamp, mic);
		afe_ep(fd, d, EV_AFE_ENDPOINT_ENABLE, inamp, "AFE_ENDPOINT_ENABLE (préampli)");
		afe_ep(fd, d, EV_AFE_ENDPOINT_ENABLE, mic, "AFE_ENDPOINT_ENABLE (micro)");
		set_gain(fd, d, inamp, 16);
	}
	if (dac) channel_start(fd, d, woofer, 0x0F00, 1);

	if (dac) {
		out("6) canal tweeter\n");
		if (!afe_alloc(fd, d, &tweeter))
			channel_start(fd, d, tweeter, 0x00F0, 1);
	}

	out("7) ampli du haut-parleur (GPIO D10 de la DVF99)\n");
	spk_amp(fd, d, 1);

	if (dac && tone >= 0) {
		uint8_t b[32]; unsigned n;
		out("8) tonalité %d pendant %d s\n", tone, hold);
		n = ie_media_channel(b, 0, woofer, 0);
		n = ie_u8(b, n, IE_TONE, tone);
		request(fd, d, EV_DEM_TONE_START, b, n, "DEM_TONE_START");
		read_for(fd, d, hold);
		n = ie_media_channel(b, 0, woofer, 0);
		request(fd, d, EV_DEM_TONE_STOP, b, n, "DEM_TONE_STOP");
	} else {
		out("8) chemin ouvert pendant %d s%s\n", hold,
		    dac ? " (TDM : jouer du son sur SSI2 côté i.MX)" : " : parler dans le micro");
		read_for(fd, d, hold);
	}
	if (keep) {
		out("9) -k : chemin laissé ouvert (Ctrl-C pour quitter, la DVF99 garde l'état)\n");
		for (;;) if (read_for(fd, d, 3600.0) < 0) return 1;
	}

	out("9) libération\n");
	spk_amp(fd, d, 0);
	afe_ep(fd, d, EV_AFE_ENDPOINT_DISABLE, o2, "AFE_ENDPOINT_DISABLE");
	afe_ep(fd, d, EV_AFE_ENDPOINT_DISABLE, o3, "AFE_ENDPOINT_DISABLE");
	if (inamp >= 0) afe_ep(fd, d, EV_AFE_ENDPOINT_DISABLE, inamp, "AFE_ENDPOINT_DISABLE");
	uint8_t b[16]; unsigned n;
	n = ie_media_channel(b, 0, woofer, 0);
	request(fd, d, EV_AFE_CHANNEL_DEALLOCATE, b, n, "AFE_CHANNEL_DEALLOCATE (woofer)");
	if (tweeter) {
		n = ie_media_channel(b, 0, tweeter, 0);
		request(fd, d, EV_AFE_CHANNEL_DEALLOCATE, b, n, "AFE_CHANNEL_DEALLOCATE (tweeter)");
	}
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
	const char *tty = "/dev/ttymxc2", *file = NULL, *eep = NULL;
	int amp = -1, tone = 0, start_channel = 0, src = AFE_AMPOUT_IN_DAC0;
	int gain = 34, hold = 5, keep = 0;
	int opt;
	while ((opt = getopt(argc, argv, "d:l:f:e:Ua:t:sS:g:w:kT:")) != -1) {
		switch (opt) {
		case 'd': tty = optarg; break;
		case 'l': journal = fopen(optarg, "a");
			  if (!journal) { perror(optarg); return 1; } break;
		case 'f': file = optarg; break;
		case 'e': eep = optarg; break;
		case 'U': unsafe = 1; break;
		case 'a': amp = atoi(optarg); break;
		case 't': tone = atoi(optarg); break;
		case 's': start_channel = 1; break;
		case 'S': src = atoi(optarg); break;
		case 'g': gain = atoi(optarg); break;
		case 'w': hold = atoi(optarg); break;
		case 'k': keep = 1; break;
		case 'T': tdm_type = atoi(optarg); break;
		default: goto usage;
		}
	}
	if (optind != argc - 1) goto usage;
	struct decoder d = { .t0 = now(), .fd = -1, .eep_fd = -1 };
	if (eep) {
		d.eep_fd = open(eep, O_RDWR);
		struct stat st;
		if (d.eep_fd < 0 || fstat(d.eep_fd, &st) < 0) { perror(eep); return 1; }
		d.eep_size = st.st_size;
		d.eep = malloc(d.eep_size ? d.eep_size : 1);
		if (!d.eep || pread(d.eep_fd, d.eep, d.eep_size, 0) != (ssize_t)d.eep_size) {
			perror(eep); return 1;
		}
		out("EEPROM émulée : %s, %zu octets\n", eep, d.eep_size);
	}
	const char *mode = argv[optind];
	if (file) {
		if (strcmp(mode, "sniff")) goto usage;
		return sniff_file(file, &d);
	}
	int fd = open_tty(tty);
	d.fd = fd;
	if (!strcmp(mode, "console")) return mode_console(fd, &d);
	if (!strcmp(mode, "boot")) return mode_boot(fd, &d);
	if (!strcmp(mode, "sniff")) return mode_sniff(fd, &d);
	if (!strcmp(mode, "init")) return mode_init(fd, &d);
	if (!strcmp(mode, "tone")) return mode_tone(fd, &d, amp, tone, start_channel, src);
	if (!strcmp(mode, "speaker")) return mode_speaker(fd, &d, src, gain, tone, hold, keep);
usage:
	fprintf(stderr, "usage : dvf99 [-d TTY] [-l JOURNAL] [-e NVRAM] [-U] console|boot|sniff|init\n"
			"        dvf99 [-e NVRAM] [-a AMP 0..3] [-t TONALITÉ] [-s] [-S SOURCE] tone\n"
			"        dvf99 [-e NVRAM] [-S SOURCE] [-g GAIN] [-t TONALITÉ|-1] [-w S] [-k] speaker\n"
			"        dvf99 -f CAPTURE sniff\n");
	return 2;
}
