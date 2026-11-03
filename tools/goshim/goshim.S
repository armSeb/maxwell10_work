/*
 * goshim.S — cale pour lancer un zImage Linux avec la commande `go` d'U-Boot,
 * qui ne passe pas par l'authentification HAB (contrairement à booti/bootm).
 *
 * `go` saute à l'adresse sans préparer le CPU comme le ferait bootm :
 * MMU, caches et L2 peuvent être actifs, et r0/r1/r2 ne suivent pas la convention
 * de boot ARM. Cette cale, placée juste devant le zImage (fichier unique, cale de 512 octets,
 * goshim.bin + zImage-dtb, voir build.sh) :
 *   1. masque IRQ/FIQ ;
 *   2. nettoie et invalide tout le cache de données L1 (set/way) ;
 *   3. nettoie et invalide le L2 PL310 (0x00a02000) s'il est actif, puis le coupe ;
 *   4. coupe MMU, D-cache, I-cache, prédicteur ; invalide I-cache, TLB, BTB ;
 *   5. saute au zImage avec r0 = 0, r1 = ~0 (pas de machine ID), r2 = 0
 *      (le DTB est appendé au zImage, CONFIG_ARM_APPENDED_DTB).
 *
 * Hypothèse : U-Boot mappe la RAM à plat (VA = PA). Sinon le saut après coupure
 * de la MMU part dans le vide.
 *
 * Utilisation dans U-Boot (exemple, adresse à adapter) :
 *     loady 0x12000000            (ou tftpboot / fatload)
 *     go 0x12000000
 */

	.syntax unified
	.arm
	.text
	.globl	_start

#define PL310		0x00a02000
#define PL310_CTRL	0x100
#define PL310_SYNC	0x730
#define PL310_CINV_WAY	0x7fc

_start:
	cpsid	if

	bl	v7_flush_dcache_all

	/* L2 PL310 : clean+invalidate par way si actif, puis désactivation */
	ldr	r4, =PL310
	ldr	r0, [r4, #PL310_CTRL]
	tst	r0, #1
	beq	1f
	ldr	r0, =0xffff
	str	r0, [r4, #PL310_CINV_WAY]
2:	ldr	r0, [r4, #PL310_CINV_WAY]
	cmp	r0, #0
	bne	2b
	mov	r0, #0
	str	r0, [r4, #PL310_SYNC]
3:	ldr	r0, [r4, #PL310_SYNC]
	tst	r0, #1
	bne	3b
	mov	r0, #0
	str	r0, [r4, #PL310_CTRL]
	dsb
1:
	/* SCTLR : coupe M (bit 0), C (bit 2), Z (bit 11), I (bit 12) */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #0x0005
	bic	r0, r0, #0x1800
	mcr	p15, 0, r0, c1, c0, 0
	isb

	bl	v7_flush_dcache_all		/* lignes éventuellement remplies entre-temps */

	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0		/* ICIALLU */
	mcr	p15, 0, r0, c7, c5, 6		/* BPIALL */
	mcr	p15, 0, r0, c8, c7, 0		/* TLBIALL */
	dsb
	isb

	mov	r0, #0
	mvn	r1, #0
	mov	r2, #0
	b	zimage

/*
 * Clean + invalidate de toutes les lignes de cache de données jusqu'au
 * point de cohérence (LoC), par set/way. Routine v7 classique.
 * Utilise r0-r3, r5, r7, r9-r11 ; préserve lr.
 */
v7_flush_dcache_all:
	dmb
	mrc	p15, 1, r0, c0, c0, 1		/* CLIDR */
	ands	r3, r0, #0x7000000
	mov	r3, r3, lsr #23			/* LoC * 2 */
	beq	finished
	mov	r10, #0				/* niveau courant * 2 */
loop1:
	add	r2, r10, r10, lsr #1		/* niveau * 3 */
	mov	r1, r0, lsr r2
	and	r1, r1, #7			/* type de cache à ce niveau */
	cmp	r1, #2
	blt	skip				/* pas de D-cache */
	mcr	p15, 2, r10, c0, c0, 0		/* CSSELR */
	isb
	mrc	p15, 1, r1, c0, c0, 0		/* CCSIDR */
	and	r2, r1, #7
	add	r2, r2, #4			/* log2(taille de ligne) */
	ldr	r4, =0x3ff
	ands	r4, r4, r1, lsr #3		/* nb de ways - 1 */
	clz	r5, r4				/* position du champ way */
	ldr	r7, =0x7fff
	ands	r7, r7, r1, lsr #13		/* nb de sets - 1 */
loop2:
	mov	r9, r4
loop3:
	orr	r11, r10, r9, lsl r5
	orr	r11, r11, r7, lsl r2
	mcr	p15, 0, r11, c7, c14, 2		/* DCCISW */
	subs	r9, r9, #1
	bge	loop3
	subs	r7, r7, #1
	bge	loop2
skip:
	add	r10, r10, #2
	cmp	r3, r10
	bgt	loop1
finished:
	mov	r10, #0
	mcr	p15, 2, r10, c0, c0, 0
	dsb
	isb
	bx	lr

	.ltorg

	/* le zImage commence à l'offset 0x200 du fichier */
	.org	0x200
zimage:
