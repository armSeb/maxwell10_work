# Camera: OmniVision OV9740

*[Version française](../fr/camera.md)*

| | |
|---|---|
| Sensor | OV9740, 1/6", 1280×720, YUV 4:2:2 output (on-chip ISP), **MIPI CSI-2, one lane** |
| Bus | i2c1 (`i2c-0` under mainline) at 0x10 |
| Clock | 24 MHz on CCM_CLKO1 (pads GPIO_0 and CSI0_MCLK): `IMX6QDL_CLK_CKO` ← CKO2 ← oscillator |
| Control | power-down GPIO5_IO24 (active high), reset GPIO5_IO23 (active low), activity LED GPIO1_IO5 (active low) |
| Supplies | fixed, always-on regulators |

It is **not** a USB camera: UVC does not apply. Mainline had an OV9740 driver for the old soc_camera
framework, removed with it.

## Driver

Kernel patch [0004](../../pmos/linux-gigaset-maxwell10/0004-media-i2c-add-OmniVision-OV9740-driver.patch)
adds a modern V4L2 subdevice driver (`ov9740`, built as a module):

- clock, GPIOs and CSI-2 endpoint from the device tree;
- `pre_streamon` / `post_streamoff`, needed for the LP-11 handshake of the `imx6-mipi-csi2` receiver;
- controls `LINK_FREQ`, `PIXEL_RATE`, `HFLIP`, `VFLIP`; `get_mbus_config`;
- the green activity LED (GPIO1_IO5, `leds` phandle of the sensor node): on while streaming, and not
  controllable from user space, like a V4L2 privacy LED (Linux 6.12 cannot resolve those from the
  device tree yet, so the driver drives it);
- two modes, **1280×720** and **640×480** (centred 960×720 window, scaled), media bus code `UYVY8_2X8`.

The register tables come from the GPL Freescale `ov9740_mipi` driver of the device's Android kernel.
The tables of the old mainline driver (from NVIDIA) gave a green picture on this module: the transport
worked (test pattern at 30 fps) but the chroma was always zero. The important differences are
`0x4300 = 0x32` (UYVY output), the MIPI timing registers, and horizontal mirroring on by default.

The module parameter `link_freq_mhz` (default 304, from the PLL: 24 MHz / 3 × 76 = 608 Mbit/s) can be
changed without rebuilding: `modprobe -r ov9740; modprobe ov9740 link_freq_mhz=…`.

## Media pipeline

On the i.MX6, `/dev/videoN` is the end of a media-controller pipeline. Until the links and pad formats
are set, the capture node refuses to stream and applications report "unable to stream".

```
ov9740 0-0010 ──► imx6-mipi-csi2 (VC0) ──► ipu1_csi0_mux ──► ipu1_csi0 ──► ipu1_csi0 capture
   UYVY8_2X8          UYVY8_2X8              UYVY8_2X8        AYUV32 (src)    YUYV, UYVY, NV12, YU12…
```

The device package sets it up automatically: the udev rule `62-maxwell-camera.rules` starts
`maxwell-camera.service` as soon as the sensor subdevice appears, and the capture node gets the stable
name **`/dev/video-camera`**. The size is chosen in `/etc/default/maxwell-camera`:

```sh
MAXWELL_CAMERA_SIZE=640x480     # default 1280x720
```

The IPU CSI source pad must be `AYUV32` (it rejects `UYVY8_2X8`); the IPU converts to the format the
application asks for.

## Use

```sh
# display (fast path: the IPU outputs NV12 directly)
gst-launch-1.0 v4l2src device=/dev/video-camera ! video/x-raw,format=NV12,width=1280,height=720 ! waylandsink
# if waylandsink refuses NV12
gst-launch-1.0 v4l2src device=/dev/video-camera ! video/x-raw,format=UYVY,width=1280,height=720 ! videoconvert ! waylandsink
```

To grab a still image, [tools/camera/ov9740-capture.sh](../../tools/camera/ov9740-capture.sh) sets the
pipeline itself and saves raw YUYV frames:

```sh
sudo tools/camera/ov9740-capture.sh 1280x720 /tmp/cam.yuv
ffmpeg -f rawvideo -pix_fmt yuyv422 -s 1280x720 -i /tmp/cam.yuv -frames:v 1 /tmp/cam.png
```

The sensor restarts from zero exposure each time streaming starts: the first frames are black
(Y ≈ 0–3, U/V = 0x80) for 1–2 s while auto-exposure converges. The script skips 60 frames
(`SKIP=…` to change); continuous video is not affected.

libcamera does not support the i.MX6 IPU: applications use the camera through PipeWire's V4L2 support.
PipeWire also lists the other imx-media nodes (IC prpenc/prpvf, IPU2, CSC scaler), which are not
connected; `ipu1_csi0 capture` is the first one and is picked by default.

## Performance

Capture runs at 30 fps (v4l2-ctl, GStreamer). The GNOME **Camera** application (Snapshot) shows about
1 frame per second with ~90 % CPU: the GC2000 GPU only offers OpenGL ES 2.0, GTK4 needs ES 3.0 and falls
back to software rendering (`GSK_RENDERER=gl` changes nothing). This is a limit of GTK4 on this GPU, not
of the camera. Video in a browser (for example a Home Assistant intercom card) or in GStreamer is fluid.

## Troubleshooting

| Symptom | Lead |
|---|---|
| no `OV9740 revision …` in `dmesg` | clock or power-down/reset lines; the module is `ov9740` |
| `-ETIMEDOUT` from imx6-mipi-csi2 | LP-11 / lane clock: MIPI register 0x4800 of the sensor |
| "unable to stream" in applications | `systemctl status maxwell-camera`, `media-ctl -p` |
| wrong colours | check `UYVY8_2X8` on the sensor, csi2 and mux pads, and register 0x4300 |
