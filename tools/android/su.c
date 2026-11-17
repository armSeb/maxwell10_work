/* su pour l'Android d'origine du Gigaset Maxwell 10 : client de rootd.
 *
 * Un su setuid ne peut pas marcher ici (build « user », SELinux enforcing : le
 * domaine shell n'a pas le droit de changer d'uid). Ce su se connecte à rootd
 * (rootd.c), lancé en root au démarrage, et relaie le terminal.
 *
 *   su                 shell root
 *   su -c "commande"   commande root (sortie brute, utilisable dans un tube)
 *   su prog args…      équivaut à su -c "prog args…"
 *
 * Construction (sous postmarketOS) : gcc -static -Os -o su su.c
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#define PORT 2323

static struct termios saved;
static int raw;
static void restore(void) { if (raw) tcsetattr(0, TCSANOW, &saved); }

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

	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (s < 0 || connect(s, (void *)&a, sizeof a)) {
		fprintf(stderr, "su : rootd injoignable (%s)\n", strerror(errno));
		return 1;
	}
	if (write(s, req, n) != n) return 1;

	if (req[0] == 'i' && isatty(0) && !tcgetattr(0, &saved)) {
		struct termios t = saved;
		cfmakeraw(&t);
		tcsetattr(0, TCSANOW, &t);
		raw = 1;
		atexit(restore);
	}
	struct pollfd p[2] = { { 0, POLLIN, 0 }, { s, POLLIN, 0 } };
	char buf[4096];
	for (;;) {
		if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; break; }
		if (p[1].revents) {
			ssize_t r = read(s, buf, sizeof buf);
			if (r <= 0) break;
			if (write(1, buf, r) != r) break;
		}
		if (p[0].revents) {
			ssize_t r = read(0, buf, sizeof buf);
			if (r <= 0) { shutdown(s, SHUT_WR); p[0].fd = -1; continue; }
			if (write(s, buf, r) != r) break;
		}
	}
	return 0;
}
