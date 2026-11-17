/* rootd : démon root pour l'Android d'origine du Gigaset Maxwell 10.
 *
 * Android 5.1 « user », SELinux enforcing : un su setuid lancé depuis adb shell
 * reste dans le domaine « shell », qui n'a pas le droit de changer d'uid. Les
 * scripts que init lance par /system/bin/sh tournent en revanche en root dans
 * le domaine « init_shell », non confiné (capacités setuid, dac_override,
 * sys_admin… mais pas ptrace). android-root.sh fait lancer rootd par
 * /system/etc/macaddress.sh (service macaddr_script, démarré à chaque boot) ;
 * le su client (su.c) s'y connecte.
 *
 * Écoute sur 127.0.0.1:2323. N'accepte que les connexions dont le propriétaire
 * (lu dans /proc/net/tcp) est root ou l'utilisateur shell d'adb (2000) : les
 * applications, même avec la permission INTERNET, sont refusées.
 *
 * Requête : une ligne, « i\n » (shell interactif) ou « c <commande>\n ». La
 * session passe par un pseudo-terminal ; en mode commande il est mis en brut
 * pour que la sortie soit transmise telle quelle.
 *
 * Construction (sous postmarketOS) : gcc -static -Os -o rootd rootd.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define PORT 2323
#define AID_SHELL 2000

/* uid du propriétaire de la socket cliente, d'après /proc/net/tcp */
static int peer_uid(unsigned short port)
{
	FILE *f = fopen("/proc/net/tcp", "r");
	char l[512];
	int uid = -1;
	if (!f) return -1;
	while (fgets(l, sizeof l, f)) {
		unsigned lport, rport; int u;
		if (sscanf(l, " %*d: %*x:%x %*x:%x %*x %*x:%*x %*x:%*x %*x %d", &lport, &rport, &u) == 3 &&
		    lport == port && rport == PORT) { uid = u; break; }
	}
	fclose(f);
	return uid;
}

static void session(int s)
{
	char req[4096]; size_t n = 0;
	while (n < sizeof req - 1) {
		ssize_t r = read(s, req + n, 1);
		if (r <= 0) return;
		if (req[n] == '\n') break;
		n++;
	}
	req[n] = 0;
	int cmd = req[0] == 'c' && req[1] == ' ';

	int m = posix_openpt(O_RDWR | O_NOCTTY);
	if (m < 0 || grantpt(m) || unlockpt(m)) return;
	char *sl = ptsname(m);
	pid_t pid = fork();
	if (pid < 0) return;
	if (!pid) {
		setsid();
		int t = open(sl, O_RDWR);
		if (t < 0) _exit(127);
		if (cmd) { struct termios tio; tcgetattr(t, &tio); cfmakeraw(&tio); tcsetattr(t, TCSANOW, &tio); }
		dup2(t, 0); dup2(t, 1); dup2(t, 2);
		if (t > 2) close(t);
		close(m); close(s);
		setenv("PATH", "/system/xbin:/system/bin:/sbin:/vendor/bin", 1);
		setenv("HOME", "/data/local/tmp", 1);
		if (chdir("/")) { /* sans importance */ }
		if (cmd) execl("/system/bin/sh", "sh", "-c", req + 2, (char *)NULL);
		else execl("/system/bin/sh", "sh", "-i", (char *)NULL);
		_exit(127);
	}
	struct pollfd p[2] = { { s, POLLIN, 0 }, { m, POLLIN, 0 } };
	char buf[4096];
	for (;;) {
		if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; break; }
		if (p[1].revents) {
			ssize_t r = read(m, buf, sizeof buf);
			if (r <= 0) break;		/* EIO : le shell est terminé */
			if (write(s, buf, r) != r) break;
		}
		if (p[0].revents) {
			ssize_t r = read(s, buf, sizeof buf);
			if (r <= 0) {		/* fin de l'entrée du client */
				p[0].fd = -1;
				if (!cmd) { struct termios t; if (!tcgetattr(m, &t)) { char eof = t.c_cc[VEOF]; if (write(m, &eof, 1)) {} } }
				continue;
			}
			if (write(m, buf, r) != r) break;
		}
	}
	kill(pid, SIGHUP);
	waitpid(pid, NULL, 0);
}

int main(void)
{
	int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (ls < 0 || bind(ls, (void *)&a, sizeof a) || listen(ls, 4)) { perror("rootd"); return 1; }

	/* démon : détaché du service oneshot qui nous lance */
	if (fork()) return 0;
	setsid();
	if (fork()) _exit(0);
	int nul = open("/dev/null", O_RDWR);
	dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
	if (nul > 2) close(nul);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	for (;;) {
		struct sockaddr_in pa; socklen_t pl = sizeof pa;
		int s = accept(ls, (void *)&pa, &pl);
		if (s < 0) continue;
		int uid = peer_uid(ntohs(pa.sin_port));
		if (uid != 0 && uid != AID_SHELL) {
			static const char no[] = "rootd : refusé (seuls root et adb shell sont admis)\n";
			if (write(s, no, sizeof no - 1)) {}
		} else if (!fork()) {
			close(ls);
			signal(SIGCHLD, SIG_DFL);
			session(s);
			_exit(0);
		}
		close(s);
	}
}
