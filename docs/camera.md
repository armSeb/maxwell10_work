# Caméra OmniVision OV9740

| | |
|---|---|
| Capteur | OV9740, 1/6", 1280×720, sortie YUV422 (ISP intégré), **MIPI CSI-2 une voie** |
| Bus | i2c1 (`i2c-0` sous mainline) @ 0x10 |
| Horloge | 24 MHz sur CCM_CLKO1 (pads GPIO_0 et CSI0_MCLK), `IMX6QDL_CLK_CKO` ← CKO2 ← oscillateur |
| Commandes | PWDN GPIO5_IO24 (actif haut), RESET GPIO5_IO23 (actif bas), LED d'activité GPIO1_IO5 (active basse) |
| Alimentations | régulateurs fixes toujours actifs (1,8 V, 2,8 V) dans le DT vendeur |

Ce n'est **pas** une caméra USB : UVC ne s'applique pas. Le pilote mainline (soc_camera, NVIDIA 2011) a
été retiré avec soc_camera. Le patch noyau `0004` le porte vers un sous-périphérique V4L2 moderne
(media controller) : horloge / GPIO / point de terminaison CSI-2 tirés du DT,
`pre_streamon` / `post_streamoff` pour la poignée de main LP-11 du récepteur `imx6-mipi-csi2`,
contrôles LINK_FREQ, PIXEL_RATE, HFLIP, VFLIP. Il est construit en module : le paramètre
`link_freq_mhz` (304 par défaut, déduit de la PLL : 24 MHz /3 ×76 = 608 Mbit/s) peut être changé sans
rebuild (`modprobe -r ov9740; modprobe ov9740 link_freq_mhz=…`).

### Tables de registres

Les tables NVIDIA d'origine donnaient une image verte : transport CSI-2 correct (la mire de test passe
à 30 i/s) mais chrominance toujours nulle (`00 00 ff 00` quel que soit 0x5019 ou le registre de
format du CSI de l'IPU). Le pilote utilise donc les tables du pilote **Freescale `ov9740_mipi`
(GPL)** du noyau Android de l'appareil, extraites de l'image `boot.img` (noyau LZO, enregistrements
`{u16 reg; u8 val; u8 mask; u32 délai}`) :

| Mode | Fenêtre capteur | Remarque |
|---|---|---|
| 1280×720 | pleine | 283 registres |
| 640×480 | 960×720 centrée, mise à l'échelle | 288 registres |

Différences clés avec NVIDIA : **0x4300 = 0x32 (sortie UYVY)**, temporisations MIPI (0x4837, 0x3001,
0x3013, 0x301f, 0x3831), miroir horizontal par défaut (0x0101 = 0x01, l'image du module est inversée
sinon), pas de reset logiciel 0x0103 ni de 0x5019, réglages image (AWB, gamma, débruitage) propres au
module. Code media bus : `UYVY8_2X8` ; le pad source du CSI de l'IPU reste en `AYUV32`, la capture
ressort en YUYV.

## Essai

```sh
sudo apk add v4l-utils
dmesg | grep -i ov9740            # « OV9740 revision … » = le capteur répond sur l'I2C
sudo tools/camera/ov9740-capture.sh
```

Chaîne : `ov9740 0-0010` → `imx6-mipi-csi2` (VC0) → `ipu1_csi0_mux` → `ipu1_csi0` → `ipu1_csi0 capture`.

Pistes si ça coince : `sensor not responding` (horloge ou PWDN/RESET : essayer l'autre polarité de
PWDN), `-ETIMEDOUT` dans imx6-mipi-csi2 (LP-11 / horloge de voie : registre MIPI 0x4800 = 0x44, horloge
continue), image décalée ou couleurs fausses (vérifier `UYVY8_2X8` sur les pads capteur / csi2 / mux, registre
0x4300 du capteur via `v4l2-dbg` ou i2ctransfer avec le pilote déchargé).
