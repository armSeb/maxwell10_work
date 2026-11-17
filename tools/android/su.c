/* su pour l'Android d'origine du Gigaset Maxwell 10 : client de rootd.
 *
 * Un su setuid ne peut pas marcher ici (build « user », SELinux enforcing : le
 * domaine shell n'a pas le droit de changer d'uid). Ce su ouvre un terminal
 * d'accueil de rootd (rootd.c, lancé en root au démarrage) : un /dev/pts/N
 * appartenant à 2000:0 en mode 0600, puis relaie le terminal.
 *
 *   su                 shell root
 *   su -c "commande"   commande root (sortie brute, utilisable dans un tube)
 *   su prog args…      équivaut à su -c "prog args…"
 *
 * Construction (sous postmarketOS) : gcc -static -Os -o su su.c
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define AID_SHELL 2000

static struct termios saved;
static int rawmode;
static void restore(void) { if (rawmode) tcsetattr(0, TCSANOW, &saved); }

static int mine(dev_t d)
{
	struct stat st;
	for (int fd = 0; fd < 3; fd++)
		if (!fstat(fd, &st) && S_ISCHR(st.st_mode) && st.st_rdev == d) return 1;
	return 0;
}

/* un terminal d'accueil libre de rootd. shell peut traverser /dev/pts et
 * ouvrir un terminal par son nom, mais pas lister le dossier : on essaie les
 * numéros un par un */
static int rendezvous(void)
{
	int fd = -1, miss = 0;
	for (int i = 0; fd < 0 && i < 1024 && miss < 64; i++) {
		char p[32]; struct stat st;
		snprintf(p, sizeof p, "/dev/pts/%d", i);
		if (stat(p, &st)) { miss++; continue; }
		miss = 0;
		if (st.st_uid != AID_SHELL || st.st_gid != 0 ||
		    (st.st_mode & 07777) != 0600 || mine(st.st_rdev)) continue;
		fd = open(p, O_RDWR | O_NOCTTY);
		if (fd >= 0 && ioctl(fd, TIOCEXCL) < 0) { close(fd); fd = -1; }
	}
	return fd;
}

int main(int argc, char **argv)
{
	char req[4096];
	int n;
	if (argc >= 3 && !strcmp(argv[1], "-c"))
		n = snprintf(req, sizeof req, "c %s\n", argv[2]);
	else if (argc >= 2) {
		n = snprintf(req, sizeof req, "c");
		for (int i = 1; i < argc && n < (int)sizeof req; i++)
			n += snprintf(req + n, sizeof req - n, " %s", argv[i]);
		if (n < (int)sizeof req) n += snprintf(req + n, sizeof req - n, "\n");
	} else
		n = snprintf(req, sizeof req, "i\n");
	if (n >= (int)sizeof req) { fprintf(stderr, "su : commande trop longue\n"); return 1; }
	if (getuid() == 0) {		/* déjà root : pas besoin de rootd */
		if (req[0] == 'c') { req[n - 1] = 0; execl("/system/bin/sh", "sh", "-c", req + 2, (char *)NULL); }
		else execl("/system/bin/sh", "sh", (char *)NULL);
		return 127;
	}

	int c = rendezvous();
	if (c < 0) { fprintf(stderr, "su : rootd injoignable (aucun terminal d'accueil libre dans /dev/pts)\n"); return 1; }
	struct termios t;
	if (!tcgetattr(c, &t)) { cfmakeraw(&t); tcsetattr(c, TCSANOW, &t); }
	if (write(c, req, n) != n) return 1;

	if (req[0] == 'i' && isatty(0) && !tcgetattr(0, &saved)) {
		struct termios r = saved;
		cfmakeraw(&r);
		tcsetattr(0, TCSANOW, &r);
		rawmode = 1;
		atexit(restore);
	}
	struct pollfd p[2] = { { 0, POLLIN, 0 }, { c, POLLIN, 0 } };
	char buf[4096];
	for (;;) {
		if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; break; }
		if (p[1].revents) {
			ssize_t r = read(c, buf, sizeof buf);
			if (r <= 0) break;		/* rootd a fermé : fin de session */
			if (write(1, buf, r) != r) break;
		}
		if (p[0].revents) {
			ssize_t r = read(0, buf, sizeof buf);
			if (r <= 0) {		/* fin de notre entrée : ^D au shell */
				char eof = 4;
				if (write(c, &eof, 1)) {}
				p[0].fd = -1;
				continue;
			}
			if (write(c, buf, r) != r) break;
		}
	}
	return 0;
}
