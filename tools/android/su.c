/* su minimal pour l'Android d'origine du Gigaset Maxwell 10.
 *
 * Installé setuid root dans /system/xbin par android-root.sh (depuis
 * postmarketOS). N'accepte que root et l'utilisateur « shell » d'adb (2000) :
 * les applications ne peuvent pas s'en servir.
 *
 *   su                 shell root
 *   su -c "commande"   commande via /system/bin/sh
 *   su prog args…      exécute prog
 *
 * Construction (sous postmarketOS) : gcc -static -Os -o su su.c
 */
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AID_SHELL 2000

int main(int argc, char **argv)
{
	uid_t u = getuid();
	if (u != 0 && u != AID_SHELL) {
		fprintf(stderr, "su : refusé (uid %d)\n", (int)u);
		return 1;
	}
	if (setgroups(0, NULL) || setgid(0) || setuid(0)) {
		fprintf(stderr, "su : %s\n", strerror(errno));
		return 1;
	}
	setenv("PATH", "/system/xbin:/system/bin:/sbin:/vendor/bin", 1);
	if (argc >= 3 && !strcmp(argv[1], "-c"))
		execl("/system/bin/sh", "sh", "-c", argv[2], (char *)NULL);
	else if (argc >= 2)
		execvp(argv[1], argv + 1);
	else
		execl("/system/bin/sh", "sh", (char *)NULL);
	fprintf(stderr, "su : exec : %s\n", strerror(errno));
	return 127;
}
