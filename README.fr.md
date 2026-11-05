# Gigaset Maxwell 10 — Linux mainline et postmarketOS

*[English version](README.md)*

Le **Gigaset Maxwell 10** est un téléphone de bureau Android 10,1" (IP + DECT) construit autour d'un
NXP i.MX6 Quad. Ce projet y fait tourner un **noyau Linux mainline** et **postmarketOS** depuis une
**carte microSD**, sans modifier l'eMMC interne : Android reste intact et redémarre dès qu'on retire
la carte. Usages typiques : tableau de bord mural Home Assistant, petit terminal tactile, combiné
DECT piloté depuis Linux.

> postmarketOS a été renommé **Nura** en 2026. Les outils (`pmbootstrap`, `pmaports`) gardent leur
> nom, et cette documentation aussi.

## État

| Fonction | État | Remarques |
|---|---|---|
| Démarrage depuis la microSD (`go` d'U-Boot, HAB intact) | ✅ | [chaîne de démarrage](docs/fr/boot.md) |
| Écran LVDS 1280×800, tactile | ✅ | KMS (imx-drm), FT5x06 |
| GPU (Vivante GC2000, etnaviv) | ✅ | OpenGL ES 2.0 seulement (voir la note GTK4 dans [camera.md](docs/fr/camera.md#performances)) |
| Fréquence CPU variable, ventilateur | ✅ | ventilateur piloté par la zone thermique du noyau |
| Ethernet (2 ports, switch Marvell 88E6320) | ✅ | ports `lan` et `pc` |
| Wi-Fi (Atheros AR6003) | ✅ | firmware non libre nécessaire |
| Bluetooth (Atheros AR3002) | ✅ | recherche, appairage, A2DP |
| USB OTG (réseau USB) | ✅ | `ssh user@172.16.42.1` |
| USB hôte (2 ports type A) | 🧪 | |
| Haut-parleur (DVF9919 woofer + tweeter) | ✅ | sortie PulseAudio, mise en veille au repos |
| Combiné DECT : écouteur, micro, décroché | ✅ | chemin audio seulement, pas encore de pile téléphonique |
| Caméra (OmniVision OV9740) | ✅ | 1280×720 / 640×480, 30 i/s |
| Micro de façade | ❌ | routage inconnu |
| HDMI, capteur de lumière | ⏳ | pas fait |

✅ fonctionne · 🧪 écrit, pas entièrement vérifié · ❌ ne fonctionne pas · ⏳ pas commencé

## Pour commencer

Il faut un PC sous Linux, une carte microSD (SDHC, 32 Go maximum, de préférence sans UHS-I) et un
adaptateur USB-série 3,3 V pour régler U-Boot une seule fois. La procédure complète est dans
**[docs/fr/install.md](docs/fr/install.md)**. En résumé :

```sh
git clone https://github.com/armSeb/maxwell10_work && cd maxwell10_work
pmbootstrap init                        # une fois, n'importe quel appareil : crée la config et le clone pmaports
pmos/install-sdcard.sh /dev/sdX         # construit l'image, corrige la table de partitions, écrit la carte
```

puis régler une fois l'environnement U-Boot par la console série.

## Documentation

| Document | Contenu |
|---|---|
| [install.md](docs/fr/install.md) | construction, écriture de la carte, réglage d'U-Boot, premier démarrage, mises à jour, dépannage |
| [hardware.md](docs/fr/hardware.md) | inventaire matériel, bus, GPIO, état des pilotes mainline |
| [boot.md](docs/fr/boot.md) | secure boot (HAB), démarrage d'un noyau non signé par `go` + goshim, pièges d'U-Boot |
| [audio-dect.md](docs/fr/audio-dect.md) | DSPG DVF9919 : protocole CMBS, chemins audio, démon `dvf99`, combiné DECT |
| [camera.md](docs/fr/camera.md) | pilote OV9740, chaîne imx-media, applications |
| [development.md](docs/fr/development.md) | organisation du dépôt, paquets, patches et config du noyau, outils de mise au point |

## Ce que ce dépôt ne contient pas

- aucun binaire ni firmware Gigaset ou DSPG, ni header dérivé de leurs sources ;
- aucune donnée propre à un appareil (numéros de série, adresses MAC, RFPI DECT, calibration RF,
  contenu d'EEPROM).

Tout ce qui est propre à l'appareil est lu sur votre propre appareil. Par exemple, l'EEPROM de la base
DECT est copiée depuis la partition de données d'Android au premier démarrage.

## Avertissement

Projet communautaire non officiel, sans lien avec Gigaset. À utiliser à vos risques. La procédure
n'écrit jamais sur l'eMMC ; la seule modification persistante est l'environnement d'U-Boot dans la
flash NOR SPI, que l'on peut rétablir (voir [install.md](docs/fr/install.md#revenir-à-android)).
