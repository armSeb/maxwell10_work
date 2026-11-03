/*
 * alive.S — charge utile « signe de vie » pour le Gigaset Maxwell 10.
 *
 * But : vérifier, SANS UART et SANS noyau Linux, que `fastboot boot`
 * exécute réellement le code placé dans le champ kernel du boot.img.
 *
 * Ce que fait le code (ARM, position-indépendant, aucune donnée en RAM) :
 *   - masque IRQ/FIQ ;
 *   - passe DISP0_DAT2 (ventilateur, GPIO4_IO23) et DISP0_DAT8
 *     (rétroéclairage, GPIO4_IO29) en GPIO (ALT5), en sortie ;
 *   - boucle : ventilateur + rétroéclairage ON ~1 s, OFF ~1 s ;
 *   - sert WDOG1 à chaque demi-période (si U-Boot l'a armé, pas de reset).
 *
 * Lecture du résultat :
 *   ventilo qui pulse / écran qui clignote indéfiniment → U-Boot saute bien
 *     dans notre code : le problème est dans le noyau / le DT ;
 *   rien, et retour sous Android après quelques secondes → U-Boot ne saute
 *     pas (ou pas où on croit) : revoir le boot.img (adresses, second, cmdline).
 *
 * La durée de la boucle d'attente n'est pas calibrée (caches et prédicteur
 * de branchement probablement coupés) : ajuster DELAY si c'est trop lent/rapide.
 */

	.syntax unified
	.arm
	.text
	.globl	_start

#define IOMUXC		0x020e0000
#define MUX_DISP0_DAT2	0x178		/* ALT5 = GPIO4_IO23 (ventilateur) */
#define MUX_DISP0_DAT8	0x190		/* ALT5 = GPIO4_IO29 (rétroéclairage) */
#define GPIO4		0x020a8000
#define GPIO_DR		0x00
#define GPIO_GDIR	0x04
#define WDOG1		0x020bc000
#define WDOG_WSR	0x02
#define PINS		((1 << 23) | (1 << 29))
#define DELAY		0x02000000

_start:
	cpsid	if

	ldr	r4, =IOMUXC
	mov	r0, #5
	str	r0, [r4, #MUX_DISP0_DAT2]
	str	r0, [r4, #MUX_DISP0_DAT8]

	ldr	r5, =GPIO4
	ldr	r6, =PINS
	ldr	r0, [r5, #GPIO_GDIR]
	orr	r0, r0, r6
	str	r0, [r5, #GPIO_GDIR]

	ldr	r7, =WDOG1
	ldr	r8, =0x5555
	ldr	r9, =0xaaaa

loop:
	ldr	r0, [r5, #GPIO_DR]
	eor	r0, r0, r6			/* bascule les deux sorties */
	str	r0, [r5, #GPIO_DR]

	strh	r8, [r7, #WDOG_WSR]		/* service du watchdog */
	strh	r9, [r7, #WDOG_WSR]

	ldr	r1, =DELAY
1:	subs	r1, r1, #1
	bne	1b
	b	loop

	.ltorg
