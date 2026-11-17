/* rootd : démon root pour l'Android d'origine du Gigaset Maxwell 10.
 *
 * Android 5.1 « user », SELinux enforcing : un su setuid lancé depuis adb shell
 * reste dans le domaine « shell », qui n'a pas le droit de changer d'uid. Les
 * scripts que init lance par /system/bin/sh tournent en revanche en root dans
 * le domaine « init_shell », non confiné (setuid, dac_override, sys_admin… mais
 * ni ptrace ni sockets TCP). android-root.sh fait lancer rootd par
 * /system/etc/macaddress.sh (service macaddr_script, à chaque boot).
 *
 * Rendez-vous par pseudo-terminal : la politique laisse init_shell créer des
 * /dev/pts/N et en changer le propriétaire, et shell les ouvrir. rootd garde
 * SLOTS terminaux « d'accueil » dont l'esclave appartient à 2000:0 (shell, groupe
 * root), mode 0600 : seuls root et l'utilisateur shell d'adb peuvent l'ouvrir.
 * Le client (su.c) en ouvre un, envoie sa requête (« i\n » shell interactif,
 * « c <commande>\n »), puis rootd relaie vers un shell root sur un second
 * pseudo-terminal. La session finit quand le client referme l'accueil.
 *
 * Construction (sous postmarketOS) : gcc -static -Os -o rootd rootd.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define SLOTS 3
#define AID_SHELL 2000

struct slot { int m, s; size_t n; char req[4096]; };

static void raw(int fd) { struct termios t; if (!tcgetattr(fd, &t)) { cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t); } }

/* nouveau terminal d'accueil ; l'esclave reste ouvert chez nous jusqu'à la
 * requête, pour que le maître ne voie pas de raccrochage en attendant */
static int mkslot(struct slot *x)
{
	char name[64];
	x->n = 0; x->s = -1;
	x->m = posix_openpt(O_RDWR | O_NOCTTY);
	if (x->m < 0) return -1;
	if (unlockpt(x->m) || ptsname_r(x->m, name, sizeof name) ||
	    (x->s = open(name, O_RDWR | O_NOCTTY)) < 0) { close(x->m); x->m = -1; return -1; }
	raw(x->s);
	if (chown(name, AID_SHELL, 0) || chmod(name, 0600)) { /* tant pis : root seul */ }
	fcntl(x->m, F_SETFD, FD_CLOEXEC); fcntl(x->s, F_SETFD, FD_CLOEXEC);
	return 0;
}

static void session(int c, const char *req, const char *pre, size_t npre)
{
	int cmd = req[0] == 'c' && req[1] == ' ';
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	char sl[64];
	if (m < 0 || unlockpt(m) || ptsname_r(m, sl, sizeof sl)) return;
	/* réglage du terminal avant que le client puisse écrire (sinon écho) */
	int t0 = open(sl, O_RDWR | O_NOCTTY);
	if (t0 >= 0 && cmd) {	/* sortie brute, pas d'écho ; ^D reste la fin d'entrée */
		struct termios tio;
		tcgetattr(t0, &tio);
		tio.c_oflag &= ~OPOST;
		tio.c_lflag &= ~(ECHO | ECHONL | ECHOE | ECHOK);
		tcsetattr(t0, TCSANOW, &tio);
	}
	pid_t pid = fork();
	if (pid < 0) return;
	if (!pid) {
		setsid();
		int t = open(sl, O_RDWR);
		if (t < 0) _exit(127);
		dup2(t, 0); dup2(t, 1); dup2(t, 2);
		if (t > 2) close(t);
		if (t0 > 2) close(t0);
		setenv("PATH", "/system/xbin:/system/bin:/sbin:/vendor/bin", 1);
		setenv("HOME", "/data/local/tmp", 1);
		if (chdir("/")) { /* sans importance */ }
		if (cmd) execl("/system/bin/sh", "sh", "-c", req + 2, (char *)NULL);
		else execl("/system/bin/sh", "sh", "-i", (char *)NULL);
		_exit(127);
	}
	if (t0 >= 0) close(t0);
	if (npre && write(m, pre, npre) < 0) { /* rien */ }
	struct pollfd p[2] = { { c, POLLIN, 0 }, { m, POLLIN, 0 } };
	char buf[4096];
	for (;;) {
		if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; break; }
		if (p[1].revents) {
			ssize_t r = read(m, buf, sizeof buf);
			if (r <= 0) break;		/* le shell est terminé */
			if (write(c, buf, r) != r) break;
		}
		if (p[0].revents) {
			ssize_t r = read(c, buf, sizeof buf);
			if (r <= 0) break;		/* le client a refermé l'accueil */
			if (write(m, buf, r) != r) break;
		}
	}
	kill(-pid, SIGHUP);
	waitpid(pid, NULL, 0);
}

int main(void)
{
	/* démon : détaché du service oneshot qui nous lance */
	if (fork()) return 0;
	setsid();
	if (fork()) _exit(0);
	int nul = open("/dev/null", O_RDWR);
	dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
	if (nul > 2) close(nul);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	static struct slot sl[SLOTS];
	for (int i = 0; i < SLOTS; i++) mkslot(&sl[i]);
	for (;;) {
		struct pollfd p[SLOTS];
		for (int i = 0; i < SLOTS; i++) {
			if (sl[i].m < 0) mkslot(&sl[i]);
			p[i].fd = sl[i].m; p[i].events = POLLIN; p[i].revents = 0;
		}
		if (poll(p, SLOTS, 5000) < 0) continue;
		for (int i = 0; i < SLOTS; i++) {
			struct slot *x = &sl[i];
			if (!p[i].revents || x->m < 0) continue;
			ssize_t r = read(x->m, x->req + x->n, sizeof x->req - 1 - x->n);
			if (r <= 0) { close(x->m); if (x->s >= 0) close(x->s); x->m = -1; continue; }
			x->n += r;
			char *nl = memchr(x->req, '\n', x->n);
			if (!nl) {
				if (x->n >= sizeof x->req - 1) { close(x->m); close(x->s); x->m = -1; }
				continue;
			}
			*nl = 0;
			close(x->s);		/* désormais, la fermeture du client se verra */
			if (!fork()) {
				signal(SIGCHLD, SIG_DFL);
				for (int j = 0; j < SLOTS; j++)
					if (j != i && sl[j].m >= 0) { close(sl[j].m); close(sl[j].s); }
				session(x->m, x->req, nl + 1, x->req + x->n - (nl + 1));
				_exit(0);
			}
			close(x->m);
			x->m = -1;		/* remplacé au prochain tour */
		}
	}
}
