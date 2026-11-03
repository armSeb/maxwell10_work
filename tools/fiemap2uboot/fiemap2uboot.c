/*
 * fiemap2uboot — localise un fichier sur l'eMMC (ioctl FIEMAP, sans root)
 * et génère les commandes U-Boot pour le recharger en RAM par lecture brute
 * (`mmc read`), puis le vérifier (`crc32`) et le lancer (`go`).
 *
 * Pourquoi : U-Boot 2009.08 ne lit pas l'ext4 à extents (ext2load échoue sur
 * /data), mais `mmc read` lit n'importe quel secteur. Depuis Android, l'utilisateur
 * `shell` (adb, sans root) peut écrire dans /data/local/tmp et demander au noyau
 * où sont les blocs de son propre fichier : FIEMAP n'exige aucun privilège
 * (contrairement à FIBMAP).
 *
 * Utilisation (sur la tablette, via adb shell) :
 *   fiemap2uboot [-a ADDR] [-d DEV] [-n] [-i SOURCE] FICHIER
 *     -a ADDR    adresse RAM de chargement (défaut 0x12000000)
 *     -d DEV     numéro du périphérique MMC côté U-Boot (défaut 0, à vérifier)
 *     -n         syntaxe récente (`mmc dev N` puis `mmc read ADDR BLK CNT`)
 *     -i SOURCE  copie d'abord SOURCE vers FICHIER en préallouant (fallocate)
 *                pour obtenir un fichier contigu (une seule commande mmc read)
 *
 * Limites vérifiées : pas de dm-crypt / device-mapper (blocs chiffrés),
 * pas d'extent différé, inline, chiffré, non aligné ou non écrit, pas de trou.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <linux/fiemap.h>
#include <linux/fs.h>

#define MAX_EXT 512

static uint32_t crc_table[256];

static void crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}

static uint32_t crc32_fd(int fd, uint64_t *len)
{
	static unsigned char buf[65536];
	uint32_t c = 0xffffffffu;
	ssize_t n;
	*len = 0;
	if (lseek(fd, 0, SEEK_SET) < 0)
		return 0;
	while ((n = read(fd, buf, sizeof buf)) > 0) {
		for (ssize_t i = 0; i < n; i++)
			c = crc_table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
		*len += n;
	}
	return c ^ 0xffffffffu;
}

static int read_sys(unsigned maj, unsigned min, const char *attr, char *out, size_t sz)
{
	char path[128];
	snprintf(path, sizeof path, "/sys/dev/block/%u:%u/%s", maj, min, attr);
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, out, sz - 1);
	close(fd);
	if (n < 0)
		return -1;
	out[n] = 0;
	return 0;
}

static int copy_prealloc(const char *src, const char *dst)
{
	int in = open(src, O_RDONLY);
	if (in < 0) { perror(src); return -1; }
	struct stat st;
	fstat(in, &st);
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) { perror(dst); close(in); return -1; }
	/* préallocation d'un seul tenant : mballoc cherche une zone contiguë */
	if (st.st_size && fallocate(out, 0, 0, st.st_size) < 0)
		fprintf(stderr, "fallocate: %s (copie quand même)\n", strerror(errno));
	static char buf[65536];
	ssize_t n;
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, n) != n) { perror("write"); return -1; }
	fsync(out);
	close(out);
	close(in);
	return 0;
}

static void usage(void)
{
	fprintf(stderr, "usage: fiemap2uboot [-a ADDR] [-d DEV] [-n] [-i SOURCE] FICHIER\n");
	exit(2);
}

int main(int argc, char **argv)
{
	unsigned long addr = 0x12000000;
	unsigned dev = 0;
	int newsyntax = 0, opt;
	const char *src = NULL;

	while ((opt = getopt(argc, argv, "a:d:ni:")) != -1) {
		switch (opt) {
		case 'a': addr = strtoul(optarg, NULL, 0); break;
		case 'd': dev = strtoul(optarg, NULL, 0); break;
		case 'n': newsyntax = 1; break;
		case 'i': src = optarg; break;
		default: usage();
		}
	}
	if (optind != argc - 1)
		usage();
	const char *file = argv[optind];

	if (src && copy_prealloc(src, file) < 0)
		return 1;

	int fd = open(file, O_RDONLY);
	if (fd < 0) { perror(file); return 1; }
	struct stat st;
	fstat(fd, &st);
	if (!S_ISREG(st.st_mode)) { fprintf(stderr, "%s : pas un fichier régulier\n", file); return 1; }

	unsigned maj = major(st.st_dev), min = minor(st.st_dev);
	char buf[256], devname[64] = "?";
	if (read_sys(maj, min, "dm/name", buf, sizeof buf) == 0) {
		fprintf(stderr, "ERREUR : %s est sur un device-mapper (%s) — /data chiffré ? "
			"Les blocs bruts sur l'eMMC ne sont pas le contenu du fichier.\n", file, buf);
		return 1;
	}
	unsigned long long pstart = 0;
	if (read_sys(maj, min, "start", buf, sizeof buf) == 0)
		pstart = strtoull(buf, NULL, 10);
	else if (read_sys(maj, min, "size", buf, sizeof buf) == 0)
		fprintf(stderr, "note : %u:%u est un disque entier, décalage 0\n", maj, min);
	else {
		fprintf(stderr, "ERREUR : %u:%u introuvable dans /sys/dev/block\n", maj, min);
		return 1;
	}
	if (read_sys(maj, min, "uevent", buf, sizeof buf) == 0) {
		char *p = strstr(buf, "DEVNAME=");
		if (p) sscanf(p + 8, "%63s", devname);
	}

	/* FIEMAP avec synchronisation : force l'allocation des blocs différés */
	size_t sz = sizeof(struct fiemap) + MAX_EXT * sizeof(struct fiemap_extent);
	struct fiemap *fm = calloc(1, sz);
	fm->fm_start = 0;
	fm->fm_length = FIEMAP_MAX_OFFSET;
	fm->fm_flags = FIEMAP_FLAG_SYNC;
	fm->fm_extent_count = MAX_EXT;
	if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) { perror("FS_IOC_FIEMAP"); return 1; }
	if (!fm->fm_mapped_extents) { fprintf(stderr, "aucun extent (fichier vide ?)\n"); return 1; }
	struct fiemap_extent *last = &fm->fm_extents[fm->fm_mapped_extents - 1];
	if (!(last->fe_flags & FIEMAP_EXTENT_LAST)) {
		fprintf(stderr, "ERREUR : plus de %d extents, fichier trop fragmenté (essayer -i)\n", MAX_EXT);
		return 1;
	}

	const uint32_t bad = FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC |
		FIEMAP_EXTENT_ENCODED | FIEMAP_EXTENT_DATA_ENCRYPTED |
		FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE |
		FIEMAP_EXTENT_DATA_TAIL | FIEMAP_EXTENT_UNWRITTEN;
	uint64_t expect = 0;
	for (unsigned i = 0; i < fm->fm_mapped_extents; i++) {
		struct fiemap_extent *e = &fm->fm_extents[i];
		if (e->fe_flags & bad) {
			fprintf(stderr, "ERREUR : extent %u, drapeaux 0x%x inutilisables en lecture brute\n",
				i, e->fe_flags);
			return 1;
		}
		if (e->fe_logical != expect) {
			fprintf(stderr, "ERREUR : trou dans le fichier à l'offset 0x%llx\n",
				(unsigned long long)expect);
			return 1;
		}
		if ((e->fe_physical | e->fe_logical) & 511) {
			fprintf(stderr, "ERREUR : extent %u non aligné sur 512 octets\n", i);
			return 1;
		}
		expect = e->fe_logical + e->fe_length;
	}
	if (expect < (uint64_t)st.st_size) {
		fprintf(stderr, "ERREUR : extents incomplets\n");
		return 1;
	}

	crc_init();
	uint64_t len;
	uint32_t crc = crc32_fd(fd, &len);
	close(fd);

	printf("# %s : %llu octets, %u extent(s), CRC32 %08x\n", file,
	       (unsigned long long)len, fm->fm_mapped_extents, crc);
	printf("# partition /dev/block/%s, premier secteur %llu (0x%llx)\n",
	       devname, pstart, pstart);
	printf("#\n# 1) vérifier le numéro de périphérique : doit afficher ef53 (magic ext4)\n");
	if (newsyntax) {
		printf("mmc dev %u\n", dev);
		printf("mmc read 0x%lx 0x%llx 1\n", addr, pstart + 2);
	} else {
		printf("mmc read %u 0x%lx 0x%llx 1\n", dev, addr, pstart + 2);
	}
	printf("md.w 0x%lx 1\n", addr + 0x38);
	printf("#\n# 2) charger\n");
	for (unsigned i = 0; i < fm->fm_mapped_extents; i++) {
		struct fiemap_extent *e = &fm->fm_extents[i];
		uint64_t n = e->fe_length;
		if (e->fe_logical + n > len)	/* dernier extent : inutile de lire au-delà */
			n = len - e->fe_logical;
		unsigned long long blk = pstart + e->fe_physical / 512;
		unsigned long long cnt = (n + 511) / 512;
		unsigned long ra = addr + e->fe_logical;
		if (newsyntax)
			printf("mmc read 0x%lx 0x%llx 0x%llx\n", ra, blk, cnt);
		else
			printf("mmc read %u 0x%lx 0x%llx 0x%llx\n", dev, ra, blk, cnt);
	}
	printf("#\n# 3) vérifier (attendu : %08x)\n", crc);
	printf("crc32 0x%lx 0x%llx\n", addr, (unsigned long long)len);
	printf("#\n# 4) lancer\ngo 0x%lx\n", addr);
	return 0;
}
