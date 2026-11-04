# Base DECT : DSPG DVF9919 et protocole CMBS

## Architecture

La **DVF9919** (famille DSPG DVF99) fait à la fois :

- la **base DECT** (combiné Maxwell, casques DECT) ;
- l'**AFE audio** : haut-parleurs (`amp_out0..3`), micros (entrées différentielles), combiné filaire RJ9
  (détection de présence par mesure de tension AUX) ;
- quelques GPIO / interruptions externes (berceau du combiné, courant de charge).

L'i.MX6 ne voit que :

- **UART3** (`/dev/ttymxc2`, 115200 8N1, sans contrôle de flux) : console U-Boot de la DVF99, puis protocole CMBS ;
- **TDM/PCM** entre la DVF99 et **SSI2** (`0x202c000`), via l'AUDMUX (port interne 2 ↔ port externe 4,
  pads SD2_DAT0..3 = AUD4_RXD / TXFS / TXD / TXC) ;
- **GPIO4_IO22** (gpio 118 du noyau vendeur) : reset de la DVF99, actif bas.

Conséquence : sans piloter la DVF99 en CMBS, **aucune sortie audio**, même sur les haut-parleurs.

## Logiciel vendeur (Android)

| Élément | Rôle |
|---|---|
| `/system/bin/CMBSServer` | service init `cmbs_server` (root), expose le service binder `CMBS_SERVICE` |
| `/system/lib/libcmbs_host.so` | pile CMBS host DSPG (couches `cfr_*`, `cmbs_int_*`, `cmbs_dsr/dee/dem/han…`, appli `appcmbs_*`) |
| `/system/lib/libCMBSClient.so` | client binder pour les applis (`com.maxwell.phone`…) |
| `/system/lib/libYModem.so` | `Dvf99_upgrade` : reset, boot, mise à jour YModem de la DVF99 |
| `cmbs_host` (outil) | appli de test DSPG `tcx_cmbs` **avec infos de debug** |
| `dvf99gpioreset.sh` | service `gpioreset_script` : pulse bas de 50 ms sur gpio 118 + relance des PHY du switch |
| `/vendor_gigaset/dect_configs/` (NOR, jffs2) | paramètres usine : `dect_ft_NNN.txt` = valeurs `CMBS_PARAM_*` (RFPI, calibration RF…) |
| `/data/cmbs/nvram.bin` | EEPROM émulée de la base (appairages) |
| `/fotatemp/firmware/cordless_noelf.bin` | image de secours de la pile cordless DVF99 |

Version de la pile observée : `Version 03.64 - Build 1a`.

⚠️ Les fichiers `dect_configs` et `nvram.bin` sont **propres à chaque appareil** (identité radio, calibration).
Ne pas les publier ; les sauvegarder avant toute expérimentation.

## Démarrage de la DVF99

D'après `libYModem.so` et `CMBSServer` :

1. reset : GPIO4_IO22 à 0 pendant 50 ms, puis 1 ;
2. la DVF99 démarre **son propre U-Boot**, console sur UART3 ;
3. l'hôte envoie `run bootm\r` ; la variable `bootm` du module fait
   `run chk_parts; cp.b css ${loadaddr} && css load ${loadaddr} …`
   (copie et démarrage de l'image **CSS** — Cordless Software Stack — depuis la flash du module) ;
4. en cas d'image invalide : `run upgrade`, attente de `Ready for binary (ymodem) download`,
   envoi de `cordless_noelf.bin` en YModem, `cp.b ${loadaddr} css`, `saveenv` ;
5. la pile cordless parle ensuite **CMBS** sur le même UART.

⚠️ Ne pas lancer `saveenv` / `upgrade` sur la console du module sans sauvegarde.

## Trames CMBS

```
DA DA DA DA                                   mot de synchro (0xDADADADA)
u16 u16_TotalLength                           \
u16 u16_PacketNr                               | ST_CMBS_SER_MSGHDR (8 octets)
u16 u16_EventID                                |
u16 u16_ParamLength                           /
u8  params[u16_ParamLength]                   liste d'IE (cfr_ie_*)
```

Taille maximale d'un message : 1024 octets (+ 4 de synchro). Couche de lien : Hello / Capabilities,
contrôle de flux (`FlowNOK` / `FlowRestart`), erreurs de checksum.

### Séquence de démarrage observée (logcat)

| ID | Événement |
|---|---|
| 26 | `CMBS_EV_DSR_SYS_START_RES` |
| 125 | `CMBS_EV_DSR_TARGET_UP` |
| 16 | `CMBS_EV_DSR_PARAM_SET_RES` |
| 239 | `CMBS_EV_DSR_AFE_ENDPOINT_ENABLE_RES` |
| 245 | `CMBS_EV_DSR_AFE_AUX_MEASUREMENT_RES` (détection RJ9) |

Configuration AFE au démarrage : `wideband = 1`, `input_diffin1 = 16`, `input_diffin2 = 16`,
`input_sig0 = 12`, `amp_out0 = 1`, `amp_out1 = 1`, `amp_out2 = 2`, `amp_out3 = 2`, TDM configuré.

## Définitions de types

L'outil `cmbs_host` du firmware contient le DWARF complet (54 unités de compilation) :
énumérations des événements (267), types d'IE (113), paramètres, AFE, codes retour, structures.
Ce header n'est **pas** publié ici ; on le régénère depuis son propre appareil :

```sh
adb pull <chemin>/cmbs_host
pip install pyelftools
python3 tools/dwarf2h.py cmbs_host cmbs_types.h
```

## Plan

1. sniffer les deux sens de l'UART3 (pistes EIM_D24 / EIM_D25 côté i.MX) pendant décroché, raccroché,
   appel interne, pose du combiné sur le socle ;
2. décodeur Python des trames (synchro, en-tête, IE) ;
3. démon `cmbsd` sous Linux : reset, `run bootm`, Hello, SYS_START, PARAM_SET depuis `dect_configs`,
   AFE, appairage, événements d'appel ;
4. audio : `fsl-ssi` + `audmux` + `simple-audio-card` en mainline, TDM esclave/maître à déterminer ;
5. intégration : baresip (SIP), Home Assistant (Assist, événements décroché/touches).

## U-Boot de la DVF99 (relevé sous mainline)

`dvf99 console` puis `printenv` (rien de propre à l'appareil) :

- `U-Boot dvf99 master 2012.04.01-00101-gb33b6ff (Aug 18 2014) maxwell-qspi`, invite `DVF99 # `,
  `bootdelay=2` mais l'autoboot ne fait que régler la console (`bootcmd` = `setenv stdin/stdout/stderr dvf_uart1`) :
  le module **attend l'hôte** ;
- flash du module : NOR `w25q16dv`, `mtdparts=256k(u-boot),256k(u-boot-env),-(css)` ;
- `bootm=run chk_parts; cp.b css 0x42000000 && css load 0x42000000 0x8e484` : copie de la pile CSS
  (0x8e484 octets) en RAM puis démarrage ;
- `upgrade`, `u-boot-upgrade`, `dfu_manifest_*` **effacent la flash du module** : à ne jamais lancer.

`dvf99 boot` → `CSS firmware successfully loaded` puis, environ 2,5 s après `run bootm`, une première trame
CMBS : paquet 0, événement 125 `CMBS_EV_DSR_TARGET_UP`, sans paramètre. En-tête confirmé en **petit-boutiste**,
`u16_TotalLength` = 8 + `u16_ParamLength`. La pile attend ensuite l'hôte.

## Couche de lien (relevée par désassemblage de `cmbs_host`)

- les **commandes de lien** passent par le même en-tête que les événements, avec `u16_EventID = 0xFF00 | n` :
  `HELLO` = 1, `HELLO_RPLY` = 2, `FLOW_NOK` = 3, `FLOW_RESTART` = 4, `RESET` = 5 ;
- l'hôte envoie toujours `u16_PacketNr = 0`, pas de somme de contrôle ;
- paramètres du Hello : vide, ou configuration TDM sur 6 octets — type (maître/esclave…), vitesse (PCM/IOM
  512…2048 kHz), synchro (courte/longue), masque de slots (u16), contrôle de flux ;
- `HELLO_RPLY` : deux u16 (version, build) puis un octet ;
- `cmbs_api_Init` = Hello puis attente de la réponse (10 s) ; `cmbs_dsr_sys_Start` = événement 25 sans paramètre.

## Premier échange réel (pmOS, 2026-11-04)

```
→ HELLO (0xFF01, vide)
← HELLO_RPLY : 64 03 1a 00 00        version 03.64, build 0x1a
→ SYS_START (25)
← SYS_START_RES (26) : IE RESPONSE (22) = 0
← PARAM_AREA_GET (76) : IE PARAMETER_AREA (26) zone 0 (EEPROM), offset 0, 0xF0 octets
```

Avec l'EEPROM émulée (`-e`, copie de `nvram.bin`, 8192 octets) : la DVF99 lit ses 0xF0 premiers octets,
reçoit SYS_START, **réécrit** aussitôt ces 0xF0 octets (PARAM_AREA_SET), puis poursuit sa lecture par blocs
de 0xF0 (offset 0xF0, …). Les octets lus et écrits sont propres à l'appareil (identité radio) : ne pas les publier.

Format d'un IE : `u16 type, u16 longueur, valeur`. Après SYS_START, la DVF99 **lit son EEPROM chez l'hôte** :
c'est le rôle de `/data/cmbs/nvram.bin` (appairages, identité). L'hôte répond par PARAM_AREA_GET_RES (77) :
IE PARAMETER_AREA (zone, offset u32, longueur u16, données) + IE RESPONSE ; les écritures arrivent en
PARAM_AREA_SET (78), réponse 79 avec IE RESPONSE (relevé dans `app_OnParamAreaGet/Set` de `cmbs_host`).

Récupérer l'EEPROM depuis pmOS, sans Android :

```sh
sudo mkdir -p /mnt/android-data && sudo mount -o ro /dev/mmcblk2p4 /mnt/android-data
sudo cp /mnt/android-data/cmbs/nvram.bin ~/nvram.orig.bin     # à sauvegarder hors de la tablette
cp ~/nvram.orig.bin ~/nvram.bin                                # copie de travail pour dvf99 -e
sudo umount /mnt/android-data
```

## Audio local : AFE

La DVF99 a un AFE : DAC/ADC, amplis `AMP_OUT0..3`, entrées micro/ligne. Événements et IE (valeur après
l'en-tête d'IE `type u16, longueur u16`) :

| Requête (réponse = +1) | IE |
|---|---|
| `AFE_CHANNEL_ALLOCATE` 247 | `AFE_RESOURCE_TYPE` 88 : u8, 0 = DAC/ADC, 1 = classe D → réponse : `RESPONSE`, `AFE_INSTANCE_NUM` 89, `MEDIACHANNEL` 10 (n° de canal) |
| `AFE_ENDPOINT_CONNECT` 236 | `AFE_ENDPOINT_CONNECT` 82 : u8 point « entrée » (ex. `AMP_OUT2` = 27), u8 sa source (ex. `AMPOUT_IN_DAC0` = 17) — voir l'énumération plus bas |
| `AFE_ENDPOINT_ENABLE` 238 / `DISABLE` 240 | `AFE_ENDPOINT` 83 : u8 |
| `AFE_ENDPOINT_GAIN` 242 | `AFE_ENDPOINT_GAIN` 84 : u8 point, s16 pas |
| `DEM_CHANNEL_START` 61 | `MEDIACHANNEL` 10 : u32 canal, u32 masque de slots TDM, u8 type (0 = IOM) ; `MEDIADESCRIPTOR` 12 : u8 codec, u8 n, n codecs (5 = PCM linéaire WB, 6 = NB) |
| `DEM_TONE_START` 67 / `STOP` 69 | `MEDIACHANNEL` 10 ; `TONE` 13 : u8 (0 = invitation à numéroter) |
| `AFE_CHANNEL_DEALLOCATE` 249 | `MEDIACHANNEL` 10 |
| `GPIO_ENABLE` 254 / `DISABLE` 256 | `GPIO_ID` 91 : u32 numéro, u8 banc (A = 0 … D = 3) |
| `GPIO_CONFIG_SET` 258 | `GPIO_ID` 91 ; `GPIO_MODE` 92 (1 = sortie) ; `GPIO_PULL_TYPE` 94 ; `GPIO_PULL_ENA` 95 ; `GPIO_VALUE` 93 |

### Énumération `E_CMBS_AFE_CHANNEL` du firmware de la Maxwell 10

⚠️ Les en-têtes CMBS 03.64 génériques (et les informations de débogage de `cmbs_host`) n'ont **pas**
`ADC_IN_DIGMIC1/2` : leur numérotation est **décalée de 2 à partir de 15**. Celle de la bibliothèque hôte
d'Android (chaînes de `app_GetAFEEndpointString`), qui fait foi pour ce firmware :

| Valeurs | Points |
|---|---|
| 0–2 | `SP_OUT`, `HS_SP_OUT`, `LINE0_OUT` |
| 3–8 | `MIC_IN`, `HS_MIC_IN`, `LINE1_IN`, `LINE0_IN`, `IN_SINGIN0`, `IN_SINGIN1` |
| 9–16 | `ADC_IN_DIFFIN0`, `ADC_IN_DIFFIN1`, `SINGIN0_MINUS_SINGIN1`, `VREF_OFFSET`, `MUTE_VREF`, `MUTE_FLOAT`, `ADC_IN_DIGMIC1`, `ADC_IN_DIGMIC2` |
| 17–24 | sources d'ampli : `DAC0`, `DAC1`, `DAC0_INV`, `DAC1_INV`, `SINGIN`, `DIFFIN0`, `DIFFIN1`, `MUTE` |
| 25–28 | `AMP_OUT0..3` |
| 29–32 | préamplis `AMP_SINGIN0/1`, `AMP_DIFFIN0/1` |
| 33–35 | `ADC0..2` |
| 36–37 | polarisation `MIC0`, `MIC1` |

Gains (inchangés) : entrées 0..31 ; sorties 32 = 0 dB, 33 = −6, 34 = −12, 35 = −18, 36 = −24 dB.

**Tous nos essais d'avant ce relevé visaient donc les mauvais points** (« AMP_OUT0 » = 23 était en fait
`AMPOUT_IN_DIFFIN1`, une source), et l'ampli du haut-parleur n'était pas alimenté (GPIO D10, ci-dessous) :
le silence s'explique.


Après SYS_START, la DVF99 relit **toute** son EEPROM par blocs de 0xF0 (plusieurs secondes) ; une requête AFE
envoyée pendant ce temps est refusée avec le code 5 (`CMBS_RC_ERROR_GENERAL`) : attendre la fin des lectures.

Premier essai (2026-11-04) : `AFE_CHANNEL_ALLOCATE` → OK, canal **0xF0**, instance 0 ; `AFE_ENDPOINT_CONNECT`
et `ENABLE` sur `AMP_OUT0..3` ← DAC0 → OK ; `DEM_TONE_START` sans canal démarré → **refus (1)** ;
`DISABLE`, `DEALLOCATE` → OK. La DVF99 émet aussi un second `TARGET_UP` à la fin de la relecture de l'EEPROM.
Avec `-s` : `DEM_CHANNEL_START` (canal 0xF0, slots 0x0003, PCM linéaire NB) → **OK**, mais `DEM_TONE_START`
toujours refusé : dans `cmbs_host`, les tonalités visent le canal d'un **appel DECT** (objet d'appel), pas un
canal AFE local. Le canal AFE démarré devrait en revanche transporter le flux TDM de l'i.MX vers le DAC0.

Liaison analogique micro → ampli (`-S 19/20/21`) : toutes les requêtes OK, **aucun son** au premier essai
(préampli, polarisation micro et gains non réglés à ce moment-là ; ajoutés ensuite). Pistes restantes :

- gains : sortie `CMBS_AFE_OUT_SING_GAIN_0DB` (32) sur `AMP_OUTx`, entrée +20 dB (16) sur le préampli ;
- les valeurs logcat `input_diffin1 = 16`, `input_sig0 = 12` correspondent à des **gains** (`E_CMBS_AFE_AMP_GAIN` :
  20 dB, 14 dB) ; `amp_out0/1 = 1`, `amp_out2/3 = 2` pourraient être des sources relatives (DAC1, DAC0 inversé),
  soit un haut-parleur en pont sur deux sorties ;
- un ampli externe commandé par un GPIO : `GPIO4_IO06` (KEY_COL0) est muxé en GPIO par le vendeur sans
  pilote associé — muxé aussi sous mainline (pkgrel 9) pour essayer `gpioset -c gpiochip3 6=1`.

Premier essai sans TDM : `dvf99 -e nvram.bin tone` — canal AFE, `AMP_OUT0..3 ← DAC0`, tonalité 5 s.
Le haut-parleur qui sonne identifie la bonne sortie (`-a N` pour n'en tester qu'une).

## Combiné DECT : premier signe de vie sous mainline

Avec la base démarrée par `dvf99` (EEPROM émulée), **décrocher puis raccrocher le combiné** produit :

```
← événement 274 (hors de l'énumération 03.64 : ajout du firmware Gigaset)
   IE 28 HANDSETS : 01 00                       combiné n° 1
   IE 32 DATA     : 07 00 81 05 05 01 02 00 27  décroché
   IE 32 DATA     : 07 00 81 05 05 01 02 01 27  raccroché
```

La liaison radio fonctionne donc, avec les appairages relus dans l'EEPROM émulée : le combiné joint la base
et lui signale son état. L'octet 7 de DATA porte l'état du crochet (0 décroché, 1 raccroché) ; le reste
(`81 05 05 01 02`, `27`) reste à interpréter.

Touche verte du combiné (prise de ligne) :

```
← CMBS_EV_DEE_CALL_ESTABLISH (46)
   IE 1  CALLINSTANCE    : 08 00 00 00                 instance d'appel 8
   IE 2  CALLERPARTY     : 0b 00 01 31                 appelant = combiné « 1 » (ASCII)
   IE 12 MEDIADESCRIPTOR : 05 07 05 03 04 06 02 01 07  codec préféré PCM linéaire WB, 7 codecs proposés
```

L'hôte doit répondre (CALL_PROGRESS / ANSWER / MEDIA_OFFER puis DEM_CHANNEL_START sur le canal de l'appel) :
c'est sur ce canal d'appel que les tonalités (`DEM_TONE_START`) ont un sens. Séquence exacte à relever
(logcat Android ou capture UART).

## Haut-parleur sous Android (logcat, 2026-11-04)

`CMBSServer` journalise les événements (sans leurs IE). Activation de la sortie « speaker » (Android
`AUDIO_DEVICE_OUT_SPEAKER`, 16 kHz, `bWideBand = 1`, carte ALSA 1 = DVF99 sur SSI2) :

| Étape | Événements (requête → réponse) |
|---|---|
| appareil 0 (« woofer ») | `AFE_CHANNEL_ALLOCATE` → canal **0xF0** ; 2 × `AFE_ENDPOINT_CONNECT` ; 2 × `AFE_ENDPOINT_ENABLE` ; 2 × `AFE_ENDPOINT_GAIN` ; `DEM_CHANNEL_START` ; **`GPIO_ENABLE` (254)** ; **`GPIO_CONFIG_SET` (258)** |
| appareil 1 (« **tweeter** ») | `AFE_CHANNEL_ALLOCATE` → canal **0xF2** ; `DEM_CHANNEL_START` (aucun point AFE connecté) |
| arrêt | désactivation des points, fermeture de 0xF0, « Disable Tweeter », fermeture de 0xF2 |

Conclusions :

- le haut-parleur est un ensemble **woofer + tweeter** : deux canaux, le tweeter probablement sur la
  ressource « classe D » (`AFE_RESOURCE_TYPE` = 1) ;
- l'ampli du woofer est activé par un **GPIO de la DVF99** (`GPIO_ENABLE` puis `GPIO_CONFIG_SET`) : **D10**
  (relevé ci-dessous) ;
- deux sorties AFE (cohérent avec `amp_out0 = amp_out1 = 1`) et deux gains ;
- au démarrage, `CMBSServer` fixe des `CMBS_PARAM_*` (puissances RF, `RXTUN`, `PREAM_NORM`, `AUXBGPROG_DIRECT`,
  `TEST_MODE`…) et `TDM configuration is: 1` ;
- un GPIO de la DVF99 est lu en boucle (`GPIO_CONFIG_GET`, ~2 Hz, valeur 0/1) après raccroché.

⚠️ Ce logcat contient le RFPI de la base : ne pas le publier.

### Séquence exacte (désassemblage de la bibliothèque hôte d'Android)

La bibliothèque décrit chaque « appareil » audio par une table de configuration ; pour le haut-parleur
(sortie 16 kHz, `wideband = 1`) :

| | Woofer (appareil 0) | Tweeter (appareil 1) |
|---|---|---|
| canal | `AFE_CHANNEL_ALLOCATE` ressource 0 (DAC/ADC) → 0xF0 | ressource **1 (classe D)** → 0xF2 |
| liaisons | `AMP_OUT2` (27) ← `DAC0` (17) ; `AMP_OUT3` (28) ← `DAC0_INV` (19) : **pont** | aucune |
| activation, gains | `ENABLE` 27, 28 ; gain 34 (−12 dB, réglage `amp_out2/3 = 2`) sur 27 et 28 | — |
| canal média | `DEM_CHANNEL_START` : `MEDIADESCRIPTOR` {5, 2, [5, 6]} puis `MEDIACHANNEL` {0xF0, slots **0x0F00**, 0} | slots **0x00F0** |
| ampli | `GPIO_ENABLE` {D, 10} ; `GPIO_CONFIG_SET` {D10, mode 1, pull type 1, pull off, valeur **1**} | — |

Masque de slots : `(wideband ? 0xF : 0x3) << n`, avec n = 8 (woofer) ou 4 (tweeter) ; codec 5 (PCM linéaire
16 kHz) ou 6 (8 kHz). Les autres appareils (combiné filaire, ligne…) utilisent le GPIO D11.

Reproduction sous mainline : `dvf99 -e nvram.bin speaker` (tonalité sur le woofer ; `-t -1` sans tonalité,
pour le flux TDM ; `-k` laisse le chemin ouvert ; `-S 22` : micro `DIFFIN0` → haut-parleur, sans TDM).

## Sous Linux mainline : premiers pas

État : la DVF99 est **hors reset** (gpio-hog sur GPIO4_IO22, pour l'horloge du switch) et reste dans
**son U-Boot** jusqu'à `dvf99 boot`, qui démarre bien la pile cordless (TARGET_UP reçu). Reste à écrire
l'hôte CMBS (Hello, SYS_START, paramètres, AFE) : pas encore de DECT ni d'audio.

L'UART3 est activé dans le DT (`/dev/ttymxc2`, pads EIM_D24 TX / EIM_D25 RX, comme le vendeur).
[tools/dvf99](../tools/dvf99/) (binaire ARM statique) :

| Commande | Effet |
|---|---|
| `dvf99 -l dvf99.log console` | console du U-Boot du module, ligne à ligne ; refuse `saveenv`, `upgrade`, `erase`, `sf`, `cp.b`… (sauf `-U`) |
| `dvf99 -l dvf99.log boot` | `\r` puis `run bootm` ; attend 20 s une trame CMBS (synchro `DA DA DA DA`) |
| `dvf99 -l dvf99.log -e nvram.bin init` | `run bootm` si besoin, **Hello** (`0xFF01`) puis **SYS_START** (25) ; avec `-e`, répond aux lectures/écritures de l'EEPROM émulée (PARAM_AREA_GET/SET) à partir d'une **copie** de `/data/cmbs/nvram.bin` |
| `dvf99 -l dvf99.log sniff` | décode en continu : texte, puis trames CMBS (n° de paquet, événement, IE en hexa) |
| `dvf99 -f capture.bin sniff` | décode un fichier brut (capture UART) |

Ordre conseillé :

1. sauvegarder `/vendor_gigaset/dect_configs/` (NOR) et `/data/cmbs/nvram.bin` (Android) — **propres à l'appareil, non publiés** ;
2. `dvf99 console` : noter l'invite, `printenv` (lecture seule), la version ;
3. `dvf99 boot` : voir si la pile démarre et émet spontanément des trames, ou attend l'hôte ;
4. **capturer l'échange réel sous Android** pour connaître la séquence hôte → DVF99 (hello, paramètres
   `CMBS_PARAM_*`, configuration AFE/TDM) : deux adaptateurs USB-série 3,3 V en écoute seule sur TX et RX de
   l'UART3 (masse commune, aucune liaison vers l'émission), `cat /dev/ttyUSB0 > hote.bin` et
   `cat /dev/ttyUSB1 > dvf99.bin` pendant un démarrage d'Android, puis `dvf99 -f … sniff` ;
5. rejouer cette séquence depuis Linux (démon `dvf99d` à écrire), puis décrire le lien audio TDM
   (SSI2 ↔ AUDMUX int 2 ↔ ext 4) dans le DT.

⚠️ Pendant `bootm`, l'horloge fournie au switch pourrait sauter (cf. workaround vendeur « AFM-307 » qui
relance les PHY) : faire ces essais par le Wi-Fi ou l'USB, et redémarrer si l'Ethernet tombe.


## Configuration TDM et carte ALSA (pkgrel 11)

Le `HELLO` de la bibliothèque hôte d'Android n'est pas vide : il porte la configuration TDM
(`tcx_TDMConfig(1)`, journalisée « TDM configuration is: 1 ») puis le contrôle de flux :

| Octet | Valeur | Sens |
|---|---|---|
| 0 | 0 | `CMBS_TDM_TYPE_MASTER` : la DVF99 fournit horloge et synchro |
| 1 | 3 | `CMBS_TDM_PCM_2048` : 2,048 MHz |
| 2 | 3 | `CMBS_TDM_SYNC_LONG` |
| 3–4 | `FF FF` | slots autorisés |
| 5 | 0 | `CMBS_FLOW_CTRL_NONE` |

Nos premiers essais envoyaient un `HELLO` vide : la DVF99 n'avait aucune configuration TDM. `dvf99` envoie
désormais celle d'Android (`-T 1` : DVF99 esclave, `-T -1` : ancien `HELLO` vide). Le `HELLO` n'est pris en
compte qu'au démarrage de la pile : redémarrer la tablette pour que `dvf99` refasse `run bootm`.

Trame : 2,048 MHz / 8 kHz = 256 bits = 32 octets = 16 mots de 16 bits. Les masques CMBS comptent en
octets : woofer `0x0F00` = octets 8..11 = mots 4 et 5 ; tweeter `0x00F0` = mots 2 et 3. En PCM linéaire
16 kHz, chaque paire de mots porte deux échantillons successifs.

DT : `simple-audio-card` « DVF99 » sur SSI2, format `left_j` + `bitclock-inversion`, SSI esclave, 16 slots de
16 bits, voies 2..5 actives ; AUDMUX port 2 ← port 4 ; codec factice `linux,spdif-dit`
(`CONFIG_SND_SOC_SPDIF`). Côté ALSA : 8 kHz, 4 voies, S16_LE — voies 0/1 = tweeter, 2/3 = woofer. Un flux
mono 16 kHz se joue tel quel en « stéréo 8 kHz » sur une paire.

Essai :

```sh
dvf99 -e nvram.bin -t -1 -k speaker &            # chemin haut-parleur ouvert, pas de tonalité
speaker-test -D hw:DVF99 -c 4 -r 8000 -F S16_LE -t sine -f 500
# un fichier : mono 16 kHz vers les voies du woofer
ffmpeg -i son.mp3 -ac 1 -ar 16000 -f s16le - | \
  sox -t raw -r 8000 -c 2 -e signed -b 16 - -t raw -c 4 - remix 0 0 1 2 | \
  aplay -D hw:DVF99 -t raw -f S16_LE -r 8000 -c 4
```

### Mise au point (2026-11-04, avec `tools/ssireg`)

- `ssireg rate` : ~8000 synchros de trame par seconde, aucun sous-débit → la DVF99 est bien maître.
- `ssireg fs` : synchro = **impulsion positive d'un bit** (et non « longue » malgré `CMBS_TDM_SYNC_LONG`).
- Réglage à chaud de `STCR` pendant une lecture : **0x280** (synchro non inversée, sans avance, données
  sur le front descendant de l'horloge) → musique **parfaite**. Les autres valeurs donnent souffle ou
  bruit « binaire ». Ordre des octets naturel (poids fort en premier). Le mode I2S esclave du SSI
  (`SCR` 0x105b) coupe le flux.
- Piège : un sinus identique sur toutes les voies masque les erreurs d'alignement (un échantillon
  « recollé » à cheval sur deux mots ressemble au bon) ; juger sur de la musique.

Si `aplay` échoue au bout de quelques secondes (« Input/output error »), la DVF99 ne fournit pas
d'horloge : essayer `-T 1` côté `dvf99` et un SSI maître dans le DT. Si le son est saturé ou haché, la
synchro est probablement décalée d'un bit : essayer `dsp_a` / `dsp_b` / `i2s` dans
`simple-audio-card,format`.

### Sortie « out of the box » (noyau pkgrel 12, device pkgrel 5)

- **Noyau** : patch `0003-ASoC-DVF9919-TDM-PCM-link` — composant ASoC `dspg,dvf9919-pcm` (aucun
  registre : il impose seulement 16 kHz, mono, S16_LE) et, dans `fsl_ssi`, le mode réseau gardé pour un
  flux mono quand une disposition TDM de plus de deux slots est définie. Le DT n'active que les mots 4
  et 5 : un échantillon 16 kHz sur deux dans chacun, exactement le format WB du woofer. ALSA, PipeWire et
  Phosh voient une sortie mono 16 kHz ordinaire.
- **Tweeter** : `dvf99` le démarre sur les **mêmes slots** que le woofer (`-W 0x0F00`, défaut) au lieu
  des slots 4..7 d'Android ; `-W 0x00F0` revient au schéma d'Android (tweeter muet sans flux dédié),
  `-W 0` le laisse éteint.
- **Paquet device** : `/usr/bin/dvf99`, `dvf99.service` (activé par preset), lanceur
  `/usr/libexec/dvf99-service` : `dvf99 -q -g 32 [-e /var/lib/dvf99/nvram.bin] service`. Mode `service` =
  init + chemin haut-parleur ouvert en permanence ; `-q` limite le journal aux événements spontanés
  (combiné, appels). Réglages dans `/etc/default/dvf99` (`DVF99_NVRAM`, `DVF99_GAIN`, `DVF99_OPTS`).

### Volume (2026-11-04)

- Paramètres CMBS 55/56 (`DAC1_VOL`/`DAC2_VOL`, `dvf99 param 55 56`) : valent `00 00` ; Android ne les
  utilise que comme **sidetone** du combiné filaire (`appsrv_AFESetSidetoneValue`, remis à 0 ensuite) —
  pas un volume de lecture, ne pas y toucher.
- Le « tweeter » d'Android est alloué sur la ressource **classe D** (`AFE_RESOURCE_TYPE` = 1, relevé dans
  `enableDevice`), pas DAC/ADC : c'est l'ampli intégré de la DVF99. `dvf99` l'alloue désormais ainsi.

## Appel du combiné (mode service, 2026-11-04)

Relevé sur la base réelle (touche verte du combiné, décroché automatique de `dvf99`) :

```
← CALL_ESTABLISH (46)     CALLINSTANCE n, CALLERPARTY « 1 », MEDIADESCRIPTOR {WB, 7 codecs}
→ CALL_PROGRESS (47)      CALLINSTANCE, MEDIADESCRIPTOR {5, 2, [5, 6]}, CALLPROGRESS 2 (proceeding)
← CALL_MEDIA_UPDATE (54)  CALLINSTANCE, MEDIACHANNEL {canal 0, 0, 0}
← CALL_STATE (45)         CALLSTATE 00 03 03 01 00 00
→ CALL_ANSWER (48)        CALLINSTANCE
→ DEM_CHANNEL_START (61)  MEDIACHANNEL {0, slots, 0}
← CALL_RELEASE (49)       au raccroché → DEM_CHANNEL_STOP (65) + CALL_RELEASECOMPLETE (50)
```

Le canal média de l'appel porte l'identifiant **0** (les canaux AFE locaux : 0xF0..). Avec les slots
0xF000 (mots 6 et 7, non émis par le SSI à ce stade), le combiné entend un souffle pendant une lecture :
le flux TDM lui parvient bien.

Un `dvf99` arrêté sans libérer ses canaux AFE laisse la ressource classe D prise (refus code 5 au
démarrage suivant) : le mode service libère désormais 0xF0..0xF3 au démarrage et ses canaux sur SIGTERM.

## Moteur audio : haut-parleur + combiné sous PulseAudio (noyau pkgrel 13, device pkgrel 7)

Constats sur la base réelle :

- le canal d'appel n'est accepté qu'au **slot 0, octets contigus** (`0x000F` → OK ; `0x0033`, `0xF000` →
  refus), comme Android (`appmedia_CallObjMediaStart` avec un décalage de slot 0) ;
- les canaux AFE locaux acceptent des slots non contigus (`0x3300`) ; woofer et classe D sur les mêmes
  slots passent après un SYS_RESET de la pile, mais le classe D sature les aigus à pleine échelle.

Un flux 16 kHz occupe **deux mots consécutifs** par trame 8 kHz : aucune voie d'un flux ALSA entrelacé
ne peut prendre cette forme. D'où la disposition d'Android, telle quelle :

| Mots TDM (octets) | Canal CMBS | Voies ALSA (8 kHz, 6 voies) | Tube PulseAudio (16 kHz mono) |
|---|---|---|---|
| 0-1 (`0x000F`) | appel du combiné | 0, 1 | `/run/dvf99/handset` → sortie « Combiné » ; capture → `/run/dvf99/mic` → entrée « Micro du combiné » |
| 2-3 (`0x00F0`) | tweeter, classe D | 2, 3 | copie du haut-parleur à `DVF99_TWEETER` % (50 par défaut) |
| 4-5 (`0x0F00`) | woofer, AMP_OUT2/3 | 4, 5 | `/run/dvf99/speaker` → sortie « Haut-parleur » (par défaut) |

- noyau : composant `dvf9919` en 8 kHz / 6 voies, DT : mots 0..5 en émission et réception ;
- `dvf99 -A` (paquet device, lié à tinyalsa) ouvre la carte, range / extrait les paires d'échantillons et
  sert les tubes ; l'arriéré de plus de 120 ms est jeté, les manques complétés de silence ;
- PulseAudio : `module-pipe-sink` ×2 et `module-pipe-source` (`/etc/pulse/default.pa.d/dvf99.pa`), la
  carte brute est ignorée (`PULSE_IGNORE`, règle udev `61-dvf99-pulseaudio.rules`).
