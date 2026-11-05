# Caméra : OmniVision OV9740

*[English version](../en/camera.md)*

| | |
|---|---|
| Capteur | OV9740, 1/6", 1280×720, sortie YUV 4:2:2 (ISP intégré), **MIPI CSI-2, une voie** |
| Bus | i2c1 (`i2c-0` sous mainline) à 0x10 |
| Horloge | 24 MHz sur CCM_CLKO1 (pads GPIO_0 et CSI0_MCLK) : `IMX6QDL_CLK_CKO` ← CKO2 ← oscillateur |
| Commandes | power-down GPIO5_IO24 (actif haut), reset GPIO5_IO23 (actif bas), LED d'activité GPIO1_IO5 (active basse) |
| Alimentations | régulateurs fixes, toujours actifs |

Ce n'est **pas** une caméra USB : UVC ne s'applique pas. Le noyau mainline avait un pilote OV9740 pour
l'ancien framework soc_camera, retiré avec lui.

## Pilote

Le patch noyau [0004](../../pmos/linux-gigaset-maxwell10/0004-media-i2c-add-OmniVision-OV9740-driver.patch)
ajoute un pilote moderne de sous-périphérique V4L2 (`ov9740`, construit en module) :

- horloge, GPIO et point de terminaison CSI-2 tirés du device tree ;
- `pre_streamon` / `post_streamoff`, nécessaires à la poignée de main LP-11 du récepteur `imx6-mipi-csi2` ;
- contrôles `LINK_FREQ`, `PIXEL_RATE`, `HFLIP`, `VFLIP` ; `get_mbus_config` ;
- deux modes, **1280×720** et **640×480** (fenêtre centrée de 960×720, mise à l'échelle), code media bus
  `UYVY8_2X8`.

Les tables de registres viennent du pilote Freescale `ov9740_mipi` (GPL) du noyau Android de l'appareil.
Celles de l'ancien pilote mainline (de NVIDIA) donnaient une image verte sur ce module : le transport
fonctionnait (mire de test à 30 i/s) mais la chrominance restait nulle. Les différences importantes :
`0x4300 = 0x32` (sortie UYVY), les registres de temporisation MIPI, et le miroir horizontal activé par
défaut.

Le paramètre de module `link_freq_mhz` (304 par défaut, déduit de la PLL : 24 MHz / 3 × 76 = 608 Mbit/s)
se change sans reconstruire : `modprobe -r ov9740; modprobe ov9740 link_freq_mhz=…`.

## Chaîne media

Sur l'i.MX6, `/dev/videoN` n'est que la sortie d'une chaîne media-controller. Tant que les liens et les
formats des pads ne sont pas posés, le nœud de capture refuse le flux et les applications affichent
« impossible de diffuser le flux ».

```
ov9740 0-0010 ──► imx6-mipi-csi2 (VC0) ──► ipu1_csi0_mux ──► ipu1_csi0 ──► ipu1_csi0 capture
   UYVY8_2X8          UYVY8_2X8              UYVY8_2X8        AYUV32 (src)    YUYV, UYVY, NV12, YU12…
```

Le paquet de l'appareil la met en place tout seul : la règle udev `62-maxwell-camera.rules` lance
`maxwell-camera.service` dès que le sous-périphérique du capteur apparaît, et le nœud de capture reçoit
le nom stable **`/dev/video-camera`**. La taille se choisit dans `/etc/default/maxwell-camera` :

```sh
MAXWELL_CAMERA_SIZE=640x480     # 1280x720 par défaut
```

Le pad source du CSI de l'IPU doit être en `AYUV32` (il refuse `UYVY8_2X8`) ; l'IPU convertit dans le
format demandé par l'application.

## Utilisation

```sh
# affichage (chemin rapide : l'IPU sort directement du NV12)
gst-launch-1.0 v4l2src device=/dev/video-camera ! video/x-raw,format=NV12,width=1280,height=720 ! waylandsink
# si waylandsink refuse le NV12
gst-launch-1.0 v4l2src device=/dev/video-camera ! video/x-raw,format=UYVY,width=1280,height=720 ! videoconvert ! waylandsink
```

Pour une image fixe, [tools/camera/ov9740-capture.sh](../../tools/camera/ov9740-capture.sh) pose
lui-même la chaîne et enregistre des images YUYV brutes :

```sh
sudo tools/camera/ov9740-capture.sh 1280x720 /tmp/cam.yuv
ffmpeg -f rawvideo -pix_fmt yuyv422 -s 1280x720 -i /tmp/cam.yuv -frames:v 1 /tmp/cam.png
```

Le capteur repart d'une exposition nulle à chaque démarrage du flux : les premières images sont noires
(Y ≈ 0–3, U/V = 0x80) pendant 1 à 2 s, le temps que l'exposition automatique converge. Le script jette
60 images (`SKIP=…` pour changer) ; une vidéo continue n'est pas gênée.

libcamera ne gère pas l'IPU de l'i.MX6 : les applications passent par la prise en charge V4L2 de
PipeWire. PipeWire liste aussi les autres nœuds imx-media (IC prpenc/prpvf, IPU2, CSC scaler), non
câblés ; `ipu1_csi0 capture` est le premier et c'est lui qui est choisi par défaut.

## Performances

La capture tient 30 i/s (v4l2-ctl, GStreamer). L'application **Caméra** de GNOME (Snapshot) affiche
environ 1 image par seconde avec ~90 % de CPU : le GPU GC2000 n'offre qu'OpenGL ES 2.0, GTK4 exige
ES 3.0 et retombe sur le rendu logiciel (`GSK_RENDERER=gl` n'y change rien). C'est une limite de GTK4
sur ce GPU, pas de la caméra. La vidéo dans un navigateur (par exemple une carte d'interphone Home
Assistant) ou dans GStreamer est fluide.

## Dépannage

| Symptôme | Piste |
|---|---|
| pas de `OV9740 revision …` dans `dmesg` | horloge ou lignes power-down/reset ; le module s'appelle `ov9740` |
| `-ETIMEDOUT` dans imx6-mipi-csi2 | LP-11 / horloge de voie : registre MIPI 0x4800 du capteur |
| « impossible de diffuser le flux » dans les applications | `systemctl status maxwell-camera`, `media-ctl -p` |
| couleurs fausses | vérifier `UYVY8_2X8` sur les pads capteur, csi2 et mux, et le registre 0x4300 |
