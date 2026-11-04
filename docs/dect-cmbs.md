# Base DECT : DSPG DVF9919 et protocole CMBS

## Architecture

La **DVF9919** (famille DSPG DVF99) fait à la fois :

- la **base DECT** (combiné Maxwell, casques DECT) ;
- l'**AFE audio** : haut-parleurs (`amp_out0..3`), micros (entrées différentielles), combiné filaire RJ9
  (détection de présence par mesure de tension AUX) ;
- quelques GPIO / interruptions externes (berceau du combiné, courant de charge).

L'i.MX6 ne voit que :

- **UART3** (`/dev/ttymxc2`, 115200 8N1, sans contrôle de flux) : console U-Boot de la DVF99, puis protocole CMBS ;
- **TDM/PCM** entre la DVF99 et **SSI1**, via l'AUDMUX (port interne 2 ↔ port externe 4) ;
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
   (SSI1 ↔ AUDMUX int 2 ↔ ext 4) dans le DT.

⚠️ Pendant `bootm`, l'horloge fournie au switch pourrait sauter (cf. workaround vendeur « AFM-307 » qui
relance les PHY) : faire ces essais par le Wi-Fi ou l'USB, et redémarrer si l'Ethernet tombe.

