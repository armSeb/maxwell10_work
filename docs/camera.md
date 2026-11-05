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

## Au démarrage (paquet `device-gigaset-maxwell10`)

Sur i.MX6, `/dev/videoN` n'est que la sortie d'une chaîne media-controller : sans liens ni formats,
le nœud refuse le flux et les applis affichent « impossible de diffuser le flux de la caméra ». La
règle udev `62-maxwell-camera.rules` lance `maxwell-camera.service` dès que le sous-périphérique du
capteur apparaît ; il pose la chaîne (`/usr/libexec/maxwell-camera`, taille
`MAXWELL_CAMERA_SIZE=1280x720|640x480` dans `/etc/default/maxwell-camera`). Le nœud de capture a
le nom stable `/dev/video-camera`. L'appli Appareil photo (PipeWire, nœud V4L2) affiche alors
l'image. PipeWire liste aussi les autres nœuds imx-media (IC prpenc/prpvf, IPU2, csc-scaler), non
câblés ; c'est le premier (`ipu1_csi0 capture`) qui est choisi par défaut.

Performances : la capture tient 30 i/s (v4l2-ctl, GStreamer). L'appli Snapshot plafonne vers
1 i/s avec ~90 % de CPU : le GC2000 (etnaviv) ne fournit qu'OpenGL ES 2.0, GTK4 exige GLES 3.0 et
retombe sur le rendu logiciel (`GSK_RENDERER=gl` n'y change rien). Limite de l'affichage GTK4, pas de
la caméra. Affichage direct quasi fluide :

```sh
gst-launch-1.0 v4l2src device=/dev/video-camera ! video/x-raw,format=UYVY,width=1280,height=720 \
  ! videoconvert ! waylandsink
```

libcamera ne gère pas l'IPU de l'i.MX6 : pas de pipeline libcamera, PipeWire passe par son
greffon V4L2.

## Essai

```sh
sudo apk add v4l-utils
dmesg | grep -i ov9740            # « OV9740 revision … » = le capteur répond sur l'I2C
sudo tools/camera/ov9740-capture.sh
```

Le capteur est réinitialisé à chaque démarrage du flux et repart d'une exposition nulle : les
premières images sont noires (Y ≈ 0–3, U/V = 0x80) le temps que l'AEC converge, environ 1 à 2 s. Le
script jette donc 60 images (`SKIP=…` pour changer) ; une application vidéo continue n'est pas gênée.

Chaîne : `ov9740 0-0010` → `imx6-mipi-csi2` (VC0) → `ipu1_csi0_mux` → `ipu1_csi0` → `ipu1_csi0 capture`.

Pistes si ça coince : `sensor not responding` (horloge ou PWDN/RESET : essayer l'autre polarité de
PWDN), `-ETIMEDOUT` dans imx6-mipi-csi2 (LP-11 / horloge de voie : registre MIPI 0x4800 = 0x44, horloge
continue), image décalée ou couleurs fausses (vérifier `UYVY8_2X8` sur les pads capteur / csi2 / mux, registre
0x4300 du capteur via `v4l2-dbg` ou i2ctransfer avec le pilote déchargé).
