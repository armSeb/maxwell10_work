# Base DECT et audio : DSPG DVF9919

*[English version](../en/audio-dect.md)*

La DSPG **DVF9919** est à la fois la **base DECT** (combiné sans fil) et le **frontal audio** (AFE) du
téléphone : DAC/ADC, amplis du haut-parleur, entrées micro. L'i.MX6 n'a pas de codec audio à lui, donc
**sans piloter la DVF9919, aucun son**, pas même sur le haut-parleur.

Ce document décrit comment la DVF9919 est pilotée sous Linux et ce que l'on sait de son protocole. Tout
a été relevé sur l'appareil (traces, comportement de la bibliothèque vendeur) ; aucun document DSPG
n'est publié.

- [Architecture](#architecture)
- [Ce qui tourne sur la tablette](#ce-qui-tourne-sur-la-tablette)
- [Utiliser l'audio](#utiliser-laudio)
- [Appairer un combiné](#appairer-un-combiné)
- [Robustesse](#robustesse)
- [Référence du protocole](#référence-du-protocole)
- [Micro de façade (question ouverte)](#micro-de-façade-question-ouverte)
- [Référence de `dvf99`](#référence-de-dvf99)

## Architecture

```
               UART3 /dev/ttymxc2 (115200 8N1)           ┌──────────────────────────┐
  i.MX6  ───────────────────────────────────────────────►│ DVF9919                  │
  dvf99         protocole CMBS (commande)                │  base DECT ◄──radio──► combiné
                                                         │  AFE : DAC0/1, ADC0..2,  │
  SSI2 ◄──AUDMUX int 2 ↔ ext 4──── TDM 2,048 MHz ────────│  AMP_OUT0..3, classe D   │──► woofer + tweeter
  (carte ALSA « DVF99 »)       (la DVF9919 est maître)   │  GPIO D10 (ampli woofer) │
                                                         └──────────────────────────┘
  GPIO4_IO22 ── reset (actif bas, maintenu relâché : il cadence aussi le switch Ethernet)
```

La DVF9919 démarre son propre U-Boot depuis sa propre flash. L'hôte envoie `run bootm`, qui lance la
pile sans fil (CSS) ; la pile parle ensuite **CMBS** sur le même UART. Les échantillons audio passent à
part, sur la liaison TDM. Chaque chemin audio (haut-parleur, appel du combiné) est un « canal média »
CMBS associé à des slots TDM.

## Ce qui tourne sur la tablette

| Composant | Rôle |
|---|---|
| patch noyau `0003-ASoC-DVF9919-TDM-PCM-link` | codec ASoC factice `dspg,dvf9919-pcm` : présente la liaison TDM comme la carte ALSA **DVF99**, 8 kHz, 6 voies, S16_LE (mots 0..5 de la trame) |
| device tree | `simple-audio-card` sur SSI2, format `left_j` + `bitclock-inversion`, la DVF9919 maître de l'horloge et de la synchro, 16 slots × 16 bits |
| `/usr/bin/dvf99` ([source](../../tools/dvf99/dvf99.c)) | hôte CMBS : démarre la pile de la DVF9919, émule son EEPROM, ouvre les chemins audio, répond au combiné, et fait tourner le moteur audio |
| `dvf99.service` → `/usr/libexec/dvf99-service` | lance `dvf99 … service` au démarrage ; importe l'EEPROM d'Android au premier démarrage |
| `dvf99-pulse.service` (unité utilisateur) | ajoute les sorties/entrées PulseAudio servies par `dvf99` à chaque session utilisateur (pas aux comptes système comme l'écran de connexion) |
| `61-dvf99-pulseaudio.rules` | cache à PulseAudio la carte brute à 6 voies |

### Le moteur audio

Un flux large bande 16 kHz occupe **deux mots de 16 bits consécutifs** par trame TDM à 8 kHz. Aucune
disposition de voies ALSA ne sait l'exprimer : `dvf99 -A` ouvre donc lui-même la carte ALSA (ioctl
directs) et sépare / rassemble les flux. Il échange des flux mono 16 kHz S16 avec PulseAudio par des
tubes nommés dans `/run/dvf99` :

| Mots TDM (masque CMBS) | Canal CMBS | Voies ALSA | Tube → périphérique PulseAudio |
|---|---|---|---|
| 0-1 (`0x000F`) | appel du combiné | 0, 1 | `handset` → sortie **Combiné** ; capture → `mic` → entrée **Micro du combiné** |
| 2-3 (`0x00F0`) | tweeter (classe D) | 2, 3 | copie du flux haut-parleur à `DVF99_TWEETER` % |
| 4-5 (`0x0F00`) | woofer (AMP_OUT2/3 en pont) | 4, 5 | `speaker` → sortie **Haut-parleur** (défaut) |

- Chaque tube de sortie a un tampon de gigue (démarre après 60 ms de données, jette l'excédent au-delà
  de 200 ms).
- **Veille du haut-parleur** : après `-Z` secondes (5 par défaut) sans échantillon non nul, `dvf99` coupe
  l'ampli du woofer (GPIO D10 de la DVF9919) et arrête le canal du tweeter, ce qui supprime le léger
  souffle au repos. Le premier son les réveille en 0,2 s environ ; pendant ce temps le son est gardé
  dans le tampon, les sons courts ne sont pas perdus.
- PulseAudio crée lui-même le tube `mic` (`module-pipe-source` refuse un fichier existant) : le
  répertoire `/run/dvf99` est en mode 1777 et `dvf99` ouvre le tube dès qu'il apparaît.

## Utiliser l'audio

Le haut-parleur est la sortie par défaut. Pour jouer dans le combiné DECT, choisir la sortie **Combiné**
dans les paramètres du son (ou `pactl set-default-sink dvf99_handset`). Le chemin du combiné n'est ouvert
que pendant un « appel » :

1. appuyer sur la **touche verte** du combiné ;
2. `dvf99` décroche automatiquement (désactivable par `DVF99_OPTS="-N"`) : l'écouteur et le micro sont
   reliés à la sortie **Combiné** et à l'entrée **Micro du combiné** ;
3. raccrocher pour fermer le chemin.

Décroché, raccroché et touches sont journalisés : `journalctl -u dvf99 -f`. Il n'y a pas encore de pile
téléphonique (SIP).

Réglages, dans `/etc/default/dvf99` (puis `sudo systemctl restart dvf99`, voir [Robustesse](#robustesse)) :

| Variable | Défaut | Sens |
|---|---|---|
| `DVF99_NVRAM` | `/var/lib/dvf99/nvram.bin` | EEPROM émulée de la base |
| `DVF99_GAIN` | `32` | gain de sortie du woofer : 32 = 0 dB, 33 = −6 dB … 36 = −24 dB |
| `DVF99_TWEETER` | `50` | niveau du tweeter en % du flux haut-parleur (les aigus saturent vers 100) |
| `DVF99_OPTS` | — | options supplémentaires : `-N` pas de décroché auto, `-Z N` délai de veille du haut-parleur (0 = jamais) |

Le volume logiciel est celui de PulseAudio.

## Appairer un combiné

L'appairage (et l'identité DECT de la base) sont dans l'EEPROM de la base, que la DVF9919 lit chez
l'hôte : `/var/lib/dvf99/nvram.bin`, importée d'Android (`/data/cmbs/nvram.bin`) au premier démarrage.
Un combiné appairé sous Android fonctionne donc directement.

Pour appairer de nouveau un combiné :

```sh
sudo systemctl stop dvf99
sudo dvf99 -e /var/lib/dvf99/nvram.bin register     # inscription ouverte 120 s
# mettre maintenant le combiné en mode inscription
```

puis couper le courant de la tablette. La base écrit le nouvel appairage dans `nvram.bin` : dès lors,
**ne plus restaurer une ancienne copie** du fichier (le combiné et la base n'auraient plus la même clé).

## Robustesse

Règles apprises à l'usage :

- **Préférer un démarrage à froid.** Relancer `dvf99` sur une pile déjà démarrée marche quelques fois,
  puis la base refuse des canaux ou ne cadence plus la liaison TDM. `dvf99 reset` (SYS_RESET CMBS) rend
  l'horloge mais **casse la liaison radio** avec le combiné : à éviter. Une coupure de courant rétablit
  toujours tout.
- **Second TARGET_UP.** Après `run bootm`, la pile lit son EEPROM, puis redémarre et s'annonce une
  seconde fois en oubliant le HELLO et le SYS_START déjà reçus. `dvf99` refait la poignée de main tant
  que de nouveaux TARGET_UP arrivent.
- **Attendre le combiné.** Ouvrir des canaux AFE juste après le démarrage empêche la base de retrouver
  le combiné. `dvf99` attend `HS_IN_RANGE`, ou n'importe quel événement du combiné, 30 s au plus (`-I`),
  avant de les ouvrir.
- **Canaux orphelins.** Un `dvf99` tué sans libérer ses canaux laisse l'ampli classe D alloué. Au
  démarrage, `dvf99` libère les canaux `0xF0`..`0xF3`, et il libère les siens sur SIGTERM.
- Le fichier d'EEPROM est réécrit par la base à chaque démarrage : garder aussi `nvram.android.bin`
  (la copie intacte) hors de la tablette.

## Référence du protocole

### Trames

```
DA DA DA DA        mot de synchro
u16 longueur totale = 8 + longueur des paramètres  ┐
u16 numéro de paquet (toujours 0 côté hôte)        │ en-tête petit-boutiste
u16 identifiant d'événement                        │
u16 longueur des paramètres                        ┘
paramètres         liste d'IE : u16 type, u16 longueur, valeur
```

Taille maximale d'un message : 1024 octets. Les commandes de la couche de lien utilisent l'identifiant
`0xFF00 | n` : HELLO 1, HELLO_RPLY 2, FLOW_NOK 3, FLOW_RESTART 4, RESET 5. Pas de somme de contrôle.

### Démarrage

```
(hôte)  run bootm\r                       sur la console U-Boot de la DVF9919
← TARGET_UP (125)                          ~2,5 s plus tard
→ HELLO (0xFF01)   00 03 03 FF FF 00       TDM : DVF9919 maître, PCM 2048 kHz, synchro longue, tous slots, sans contrôle de flux
← HELLO_RPLY       64 03 1a 00 00          pile version 03.64, build 0x1a
→ SYS_START (25)
← SYS_START_RES (26)
← PARAM_AREA_GET (76) … / PARAM_AREA_SET (78) …    la base lit/écrit son EEPROM par blocs de 0xF0 octets
← TARGET_UP (125)                          seconde fois : refaire HELLO + SYS_START
```

Les accès à l'EEPROM portent l'IE `PARAMETER_AREA` (26) : zone (0 = EEPROM), décalage u32, longueur u16,
données. L'hôte répond 77 / 79 avec les données et l'IE `RESPONSE` (22) = 0. Les requêtes AFE envoyées
pendant que la base lit encore son EEPROM sont refusées (code 5).

### Requêtes AFE

| Requête (réponse = id + 1) | IE |
|---|---|
| `AFE_CHANNEL_ALLOCATE` 247 | `AFE_RESOURCE_TYPE` 88 : u8, 0 = DAC/ADC, 1 = classe D → réponse : `MEDIACHANNEL` 10 (numéro de canal) |
| `AFE_ENDPOINT_CONNECT` 236 | `AFE_ENDPOINT_CONNECT` 82 : u8 destination, u8 source |
| `AFE_ENDPOINT_ENABLE` 238 / `DISABLE` 240 | `AFE_ENDPOINT` 83 : u8 |
| `AFE_ENDPOINT_GAIN` 242 | `AFE_ENDPOINT_GAIN` 84 : u8 point, s16 pas |
| `DEM_CHANNEL_START` 61 / `STOP` 65 | `MEDIADESCRIPTOR` 12 : u8 codec, u8 n, n codecs (5 = PCM linéaire 16 kHz, 6 = 8 kHz) ; `MEDIACHANNEL` 10 : u32 canal, u32 masque de slots TDM (en octets), u8 0 |
| `AFE_CHANNEL_DEALLOCATE` 249 | `MEDIACHANNEL` 10 |
| `GPIO_ENABLE` 254 / `DISABLE` 256 | `GPIO_ID` 91 : u32 numéro, u8 banc (A = 0 … D = 3) |
| `GPIO_CONFIG_SET` 258 | `GPIO_ID` 91, `GPIO_MODE` 92 (1 = sortie), `GPIO_PULL_TYPE` 94, `GPIO_PULL_ENA` 95, `GPIO_VALUE` 93 |
| `PARAM_GET` 13 / `PARAM_SET` 15 | `PARAMETER` 16 : u8 id, u8 type, u16 longueur, données |

Numérotation des points AFE **de ce firmware** (décalée de 2 à partir de 15 par rapport aux en-têtes
CMBS 03.64 génériques, qui n'ont pas les deux entrées micro numériques) :

| Valeurs | Points |
|---|---|
| 0–2 | `SP_OUT`, `HS_SP_OUT`, `LINE0_OUT` |
| 3–8 | `MIC_IN`, `HS_MIC_IN`, `LINE1_IN`, `LINE0_IN`, `IN_SINGIN0`, `IN_SINGIN1` |
| 9–16 | entrées d'ADC : `DIFFIN0`, `DIFFIN1`, `SINGIN0−SINGIN1`, `VREF_OFFSET`, `MUTE_VREF`, `MUTE_FLOAT`, `DIGMIC1`, `DIGMIC2` |
| 17–24 | sources d'ampli : `DAC0`, `DAC1`, `DAC0_INV`, `DAC1_INV`, `SINGIN`, `DIFFIN0`, `DIFFIN1`, `MUTE` |
| 25–28 | `AMP_OUT0..3` |
| 29–32 | préamplis `AMP_SINGIN0/1`, `AMP_DIFFIN0/1` |
| 33–35 | `ADC0..2` |
| 36–37 | polarisation micro `MIC0`, `MIC1` |

Gains : entrées 0..31 ; sorties 32 = 0 dB, 33 = −6, 34 = −12, 35 = −18, 36 = −24 dB. Les paramètres
55/56 (`DAC1_VOL`/`DAC2_VOL`) sont l'effet local du combiné filaire, pas un volume de lecture : ne pas
y toucher.

### Séquence du haut-parleur (comme Android)

| | Woofer | Tweeter |
|---|---|---|
| canal | `AFE_CHANNEL_ALLOCATE` ressource 0 (DAC/ADC) | ressource **1 (classe D)** |
| liaisons | `AMP_OUT2` (27) ← `DAC0` (17), `AMP_OUT3` (28) ← `DAC0_INV` (19) : en pont | aucune |
| activation, gain | activer 27 et 28, gain sur les deux | — |
| média | `DEM_CHANNEL_START` codec 5, slots `0x0F00` | slots `0x00F0` |
| ampli | `GPIO_ENABLE` D10, `GPIO_CONFIG_SET` D10 = 1 | — |

Les masques de slots comptent les **octets** de la trame de 32 octets (2,048 MHz / 8 kHz = 256 bits) :
`(large bande ? 0xF : 0x3) << n`.

### Appels depuis le combiné

```
← CALL_ESTABLISH (46)     CALLINSTANCE, CALLERPARTY « 1 », MEDIADESCRIPTOR
→ CALL_PROGRESS (47)      CALLINSTANCE, MEDIADESCRIPTOR {5, 2, [5, 6]}, CALLPROGRESS 2
← CALL_MEDIA_UPDATE (54)  MEDIACHANNEL {0, …}       le canal d'appel porte le numéro 0
→ CALL_ANSWER (48)        CALLINSTANCE
→ DEM_CHANNEL_START (61)  MEDIACHANNEL {0, 0x000F, 0}
← CALL_RELEASE (49)       au raccroché → DEM_CHANNEL_STOP (65), CALL_RELEASECOMPLETE (50)
```

Le canal d'appel n'est accepté qu'au **slot 0, octets contigus** (`0x000F` ; `0x0033` ou `0xF000` sont
refusés). L'état du combiné arrive aussi par l'événement **274** (extension Gigaset) : IE 28 numéro du
combiné, IE 32 données dont le 8ᵉ octet est l'état du crochet (0 décroché, 1 raccroché) ; les touches de
volume le produisent aussi.

### Format TDM

Mesuré avec [tools/debug/ssireg](../../tools/debug/ssireg/ssireg.c) : 8000 trames/s, synchro de trame en
**impulsion positive d'un bit** (malgré le réglage « synchro longue »), données valides sur le front
descendant : `STCR = 0x280` sur SSI2, ce que le DT exprime par `left_j` + `bitclock-inversion`.

## Micro de façade (question ouverte)

La vitre a une ouverture de micro, mais son routage est inconnu. La bibliothèque vendeur décrit six
« appareils » audio ; les appareils 2 à 5 relient un micro à l'ADC0 au slot 0 :

| Appareil | Slot | Liaisons | Activés | GPIO |
|---|---|---|---|---|
| 0 woofer | 8 | OUT2 ← DAC0, OUT3 ← DAC0_INV | OUT2, OUT3 | D10 |
| 1 tweeter | 4 | classe D | — | — |
| 2 | 0 | ADC0 ← DIGMIC1 (15) | — | — |
| 3 | 0 | OUT0 ← DAC0, OUT1 ← DAC0_INV, ADC0 ← SINGIN0−SINGIN1 (11) | OUT0, OUT1, AMP_SINGIN0/1 | D11 |
| 4 | 0 | idem, ADC0 ← DIFFIN0 (9) | OUT0, OUT1, AMP_DIFFIN0, ADC0 | D11 |
| 5 | 0 | idem, ADC0 ← DIFFIN1 (10) | OUT0, OUT1, AMP_DIFFIN1, ADC0 | D11 |

`dvf99 -A mictest` et `dvf99 -F SRC` essaient ces entrées (avec le GPIO D11, la polarisation micro et
+30 dB) : les canaux sont acceptés et des échantillons arrivent, mais aucun ne réagit à la voix. Étape
suivante : observer Android pendant un appel mains libres (capture CMBS sur l'UART3, ou journal vendeur).

## Référence de `dvf99`

Construction : `tools/dvf99/build.sh` (binaire ARM statique) ou le paquet de l'appareil. À lancer en
root ; arrêter d'abord `dvf99.service`, un seul programme peut utiliser l'UART.

| Commande | Effet |
|---|---|
| `dvf99 [-q] [-A] [-e NVRAM] service` | ce que lance le service : démarrage, chemin haut-parleur, décroché auto, moteur audio |
| `dvf99 [-e NVRAM] register` | ouvre l'inscription pendant 120 s |
| `dvf99 [-e NVRAM] init` | démarre la pile si besoin, HELLO, SYS_START, puis quitte |
| `dvf99 console` | console U-Boot de la DVF9919 ; les commandes qui écrivent dans sa flash sont refusées (sauf `-U`) |
| `dvf99 sniff` / `dvf99 -f FICHIER sniff` | décode les trames CMBS en direct ou depuis une capture UART brute |
| `dvf99 [-e NVRAM] speaker [-t TONALITÉ\|-1] [-k]` | ouvre le chemin haut-parleur, avec une tonalité éventuelle |
| `dvf99 [-e NVRAM] param N [=HEX] …` | lit / écrit des paramètres CMBS |
| `dvf99 -A -e NVRAM mictest` | recherche du micro de façade |

| Option | Sens |
|---|---|
| `-d TTY` | UART (défaut `/dev/ttymxc2`) |
| `-e NVRAM` | fichier d'EEPROM émulée (lu et **écrit** par la base) |
| `-q` | ne journalise que les événements utiles |
| `-A` | moteur audio (tubes dans `/run/dvf99`) ; `-M` : sans capture |
| `-g GAIN` | gain du woofer 32..36 |
| `-X PCT` | niveau du tweeter |
| `-Z S` | délai de veille du haut-parleur, 0 = jamais |
| `-N` | pas de décroché automatique |
| `-I S` | attente maximale du combiné avant d'ouvrir les canaux (défaut 30) |
| `-P` / `-W` / `-C SLOTS` | masques de slots du woofer / du tweeter / de l'appel |
| `-T 0\|1\|-1` | rôle TDM dans le HELLO (0 = DVF9919 maître, défaut) |
| `-l FICHIER` | fichier journal |

Ne jamais lancer `saveenv`, `upgrade` ni `u-boot-upgrade` sur la console de la DVF9919 : ils effacent ou
réécrivent la flash du module.

Pour la recherche, l'outil de test vendeur `cmbs_host` (sur la partition système d'Android) contient
toutes ses informations de débogage DWARF ; [tools/debug/dwarf2h.py](../../tools/debug/dwarf2h.py) en
tire un header C (`python3 tools/debug/dwarf2h.py cmbs_host cmbs_types.h`, nécessite `pyelftools`). Le
résultat est dérivé du code vendeur : le garder en local.
