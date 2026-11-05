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
(media controller) : mêmes tables de registres, horloge / GPIO / point de terminaison CSI-2 tirés du DT,
`pre_streamon` / `post_streamoff` pour la poignée de main LP-11 du récepteur `imx6-mipi-csi2`,
contrôles LINK_FREQ, PIXEL_RATE, HFLIP, VFLIP. Il est construit en module : le paramètre
`link_freq_mhz` (304 par défaut, déduit de la PLL : 24 MHz /3 ×76 = 608 Mbit/s) peut être changé sans
rebuild (`modprobe -r ov9740; modprobe ov9740 link_freq_mhz=…`).

## Essai

```sh
sudo apk add v4l-utils
dmesg | grep -i ov9740            # « OV9740 revision … » = le capteur répond sur l'I2C
sudo tools/camera/ov9740-capture.sh
```

Chaîne : `ov9740 0-0010` → `imx6-mipi-csi2` (VC0) → `ipu1_csi0_mux` → `ipu1_csi0` → `ipu1_csi0 capture`.

Pistes si ça coince : `sensor not responding` (horloge ou PWDN/RESET : essayer l'autre polarité de
PWDN), `-ETIMEDOUT` dans imx6-mipi-csi2 (LP-11 / horloge de voie : registre MIPI 0x4800 = 0x44, horloge
continue), image décalée ou couleurs fausses (ordre YUYV / UYVY, registre ISP 0x5019).
