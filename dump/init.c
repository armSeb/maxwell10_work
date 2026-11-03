/*
 * init minimal pour le dump du Gigaset Maxwell 10.
 * - monte proc/sys/devtmpfs/configfs
 * - attend l'eMMC et la NOR
 * - crée un gadget USB composite : mass_storage (LUNs en lecture seule)
 *   + ACM (le dmesg est recopié en continu sur /dev/ttyGS0)
 * Aucune écriture sur l'eMMC ni sur la NOR.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define G "/cfg/usb_gadget/g1"

static void logk(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n, fd;

	n = snprintf(buf, sizeof(buf), "<3>maxwell-dump: ");
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	fd = open("/dev/kmsg", O_WRONLY);
	if (fd >= 0) {
		write(fd, buf, n);
		close(fd);
	}
}

static int wr(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		logk("open %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (write(fd, val, strlen(val)) < 0)
		logk("write %s=%s: %s\n", path, val, strerror(errno));
	close(fd);
	return 0;
}

static int wait_for(const char *path, int seconds)
{
	struct stat st;
	for (int i = 0; i < seconds * 10; i++) {
		if (stat(path, &st) == 0)
			return 0;
		usleep(100000);
	}
	return -1;
}

static int nlun;

static void add_lun(const char *dev)
{
	char p[256];

	if (wait_for(dev, 5)) {
		logk("%s absent, ignoré\n", dev);
		return;
	}
	snprintf(p, sizeof(p), G "/functions/mass_storage.0/lun.%d", nlun);
	if (nlun > 0)
		mkdir(p, 0755);
	snprintf(p, sizeof(p), G "/functions/mass_storage.0/lun.%d/ro", nlun);
	wr(p, "1");
	snprintf(p, sizeof(p), G "/functions/mass_storage.0/lun.%d/removable", nlun);
	wr(p, "0");
	snprintf(p, sizeof(p), G "/functions/mass_storage.0/lun.%d/inquiry_string", nlun);
	{
		char inq[64];
		snprintf(inq, sizeof(inq), "Maxwell %s", strrchr(dev, '/') + 1);
		wr(p, inq);
	}
	snprintf(p, sizeof(p), G "/functions/mass_storage.0/lun.%d/file", nlun);
	wr(p, dev);
	logk("LUN %d = %s\n", nlun, dev);
	nlun++;
}

static void kmsg_to_acm(void)
{
	char buf[1024];
	int in, out;
	ssize_t n;

	for (;;) {
		out = open("/dev/ttyGS0", O_WRONLY | O_NOCTTY);
		if (out < 0) {
			sleep(1);
			continue;
		}
		in = open("/dev/kmsg", O_RDONLY);
		if (in < 0) {
			close(out);
			sleep(1);
			continue;
		}
		while ((n = read(in, buf, sizeof(buf))) != 0) {
			if (n < 0) {
				if (errno == EPIPE)	/* messages écrasés */
					continue;
				break;
			}
			if (write(out, buf, n) < 0)
				break;
			write(out, "\r", 1);
		}
		close(in);
		close(out);
		sleep(1);
	}
}

int main(void)
{
	DIR *d;
	struct dirent *e;
	char udc[64] = "";

	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mkdir("/dev", 0755);
	mkdir("/cfg", 0755);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("configfs", "/cfg", "configfs", 0, NULL);

	logk("démarrage\n");

	if (wait_for("/dev/mmcblk2", 20))
		logk("ATTENTION : /dev/mmcblk2 introuvable\n");

	mkdir(G, 0755);
	wr(G "/idVendor", "0x1d6b");
	wr(G "/idProduct", "0x0104");
	wr(G "/bcdDevice", "0x0100");
	wr(G "/bcdUSB", "0x0200");
	mkdir(G "/strings/0x409", 0755);
	wr(G "/strings/0x409/manufacturer", "Gigaset (dump)");
	wr(G "/strings/0x409/product", "Maxwell 10 dump");
	wr(G "/strings/0x409/serialnumber", "maxwell-dump");
	mkdir(G "/configs/c.1", 0755);
	mkdir(G "/configs/c.1/strings/0x409", 0755);
	wr(G "/configs/c.1/strings/0x409/configuration", "dump");
	wr(G "/configs/c.1/MaxPower", "100");

	mkdir(G "/functions/mass_storage.0", 0755);
	wr(G "/functions/mass_storage.0/stall", "1");
	add_lun("/dev/mmcblk2");
	add_lun("/dev/mmcblk2boot0");
	add_lun("/dev/mmcblk2boot1");
	add_lun("/dev/mtdblock0");
	symlink(G "/functions/mass_storage.0", G "/configs/c.1/mass_storage.0");

	mkdir(G "/functions/acm.0", 0755);
	symlink(G "/functions/acm.0", G "/configs/c.1/acm.0");

	d = opendir("/sys/class/udc");
	if (d) {
		while ((e = readdir(d))) {
			if (e->d_name[0] != '.') {
				snprintf(udc, sizeof(udc), "%s", e->d_name);
				break;
			}
		}
		closedir(d);
	}
	if (udc[0]) {
		wr(G "/UDC", udc);
		logk("gadget lié à %s, %d LUN(s)\n", udc, nlun);
	} else {
		logk("ERREUR : aucun UDC\n");
	}

	if (fork() == 0)
		kmsg_to_acm();

	for (;;) {
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		sleep(10);
	}
	return 0;
}
