# DECT base and audio: DSPG DVF9919

*[Version française](../fr/audio-dect.md)*

The DSPG **DVF9919** is at the same time the **DECT base** (cordless handset) and the **audio front end**
(AFE) of the phone: DAC/ADC, speaker amplifiers, microphone inputs. The i.MX6 has no audio codec of its
own, so **without driving the DVF9919 there is no sound at all**, not even on the speaker.

This document describes how the DVF9919 is driven under Linux and what is known of its protocol.
Everything here was found on the device (traces, behaviour of the vendor library); no DSPG material is
published.

- [Architecture](#architecture)
- [What runs on the tablet](#what-runs-on-the-tablet)
- [Using the audio](#using-the-audio)
- [Pairing a handset](#pairing-a-handset)
- [Robustness](#robustness)
- [Protocol reference](#protocol-reference)
- [Front microphone (open issue)](#front-microphone-open-issue)
- [`dvf99` reference](#dvf99-reference)

## Architecture

```
               UART3 /dev/ttymxc2 (115200 8N1)           ┌──────────────────────────┐
  i.MX6  ───────────────────────────────────────────────►│ DVF9919                  │
  dvf99         CMBS protocol (control)                  │  DECT base ◄──radio──► handset
                                                         │  AFE: DAC0/1, ADC0..2,   │
  SSI2 ◄──AUDMUX int 2 ↔ ext 4──── TDM 2.048 MHz ────────│  AMP_OUT0..3, class D    │──► woofer + tweeter
  (ALSA card "DVF99")            (DVF9919 is master)     │  GPIO D10 (woofer amp)   │
                                                         └──────────────────────────┘
  GPIO4_IO22 ── reset (active low, kept released: it also clocks the Ethernet switch)
```

The DVF9919 boots its own U-Boot from its own flash. The host sends `run bootm`, which starts the
cordless stack (CSS); the stack then speaks **CMBS** on the same UART. Audio samples travel separately
on the TDM link. Each audio path (speaker, handset call) is a CMBS "media channel" bound to a set of
TDM slots.

## What runs on the tablet

| Component | Role |
|---|---|
| kernel patch `0003-ASoC-DVF9919-TDM-PCM-link` | dummy ASoC codec `dspg,dvf9919-pcm`: exposes the TDM link as ALSA card **DVF99**, 8 kHz, 6 channels, S16_LE (words 0..5 of the frame) |
| device tree | `simple-audio-card` on SSI2, format `left_j` + `bitclock-inversion`, DVF9919 is clock and frame master, 16 slots × 16 bit |
| `/usr/bin/dvf99` ([source](../../tools/dvf99/dvf99.c)) | CMBS host: boots the DVF9919 stack, emulates its EEPROM, opens the audio paths, answers the handset, and runs the audio engine |
| `dvf99.service` → `/usr/libexec/dvf99-service` | starts `dvf99 … service` at boot; imports the EEPROM from Android on first boot |
| `dvf99-pulse.service` (user unit) | adds the PulseAudio outputs/inputs fed by `dvf99` to each user session (not to system users such as the login greeter) |
| `61-dvf99-pulseaudio.rules` | hides the raw 6-channel card from PulseAudio |

### The audio engine

A 16 kHz wideband stream occupies **two consecutive 16-bit words** per 8 kHz TDM frame. No ALSA channel
layout can express that, so `dvf99 -A` opens the ALSA card itself (raw ioctls) and splits/merges the
streams. It exchanges 16 kHz mono S16 streams with PulseAudio through named pipes in `/run/dvf99`:

| TDM words (CMBS mask) | CMBS channel | ALSA channels | Pipe → PulseAudio device |
|---|---|---|---|
| 0-1 (`0x000F`) | handset call, otherwise front microphone | 0, 1 | `handset` → output **Combiné**; capture → `mic` → **Micro du combiné** during a call, otherwise → `front` → **Micro de façade** (16 kHz) |
| 2-3 (`0x00F0`) | tweeters (class D) | 2, 3 | `tweeter` → output **Tweeters**, at `DVF99_TWEETER` % |
| 4-5 (`0x0F00`) | woofer (AMP_OUT2) | 4, 5 | `woofer` → output **Woofer** |

The default output **Haut-parleurs** is a `module-combine-sink` of **Woofer** and **Tweeters**, so the
volumes of these two outputs in the mixer set the bass/treble balance.

Wiring found with `dvf99 outtest`:

- the **two small speakers** at the bottom are wired together on the **class D**: they play the same
  signal, stereo is impossible without a hardware change;
- the **woofer** is loud with AMP_OUT2 alone or AMP_OUT3 alone, but very faint with the
  OUT2 ← DAC0 / OUT3 ← DAC0_INV bridge Android uses: both outputs seem summed before the woofer
  amplifier, and the inversion cancels out. `dvf99` therefore only uses AMP_OUT2 (`-B 1`: OUT2 + OUT3
  in phase, `-B 2`: Android's bridge);
- AMP_OUT0/1 (with or without GPIO D11): no sound on the speakers.

- Each output pipe has a jitter buffer (starts after 60 ms of data, drops anything beyond 200 ms).
- **Speaker sleep**: after `-Z` seconds (default 5) without a non-zero sample, `dvf99` switches the woofer
  amplifier off (DVF9919 GPIO D10) and stops the tweeter channel, which removes the faint hiss at rest.
  The first sound wakes them in about 0.2 s; meanwhile the sound is held in the buffer, so short sounds
  are not lost.
- PulseAudio creates the `mic` pipe itself (`module-pipe-source` refuses an existing file): the
  directory `/run/dvf99` is mode 1777 and `dvf99` opens the pipe when it appears.

## Using the audio

The speaker is the default output. To play to the DECT handset, pick the **Combiné** output in the sound
settings (or `pactl set-default-sink dvf99_handset`). The handset path is only open during a "call":

1. press the **green key** of the handset;
2. `dvf99` answers automatically (disable with `DVF99_OPTS="-N"`): the earpiece and microphone are now
   connected to the **Combiné** output and **Micro du combiné** input;
3. hang up to close the path.

Hook and key events are logged: `journalctl -u dvf99 -f`. There is no telephony stack (SIP) yet.

Settings, in `/etc/default/dvf99` (then `sudo systemctl restart dvf99`, see [Robustness](#robustness)):

| Variable | Default | Meaning |
|---|---|---|
| `DVF99_NVRAM` | `/var/lib/dvf99/nvram.bin` | emulated EEPROM of the base |
| `DVF99_GAIN` | `32` | woofer output gain: 32 = 0 dB, 33 = −6 dB … 36 = −24 dB |
| `DVF99_TWEETER` | `50` | tweeter level in % of the speaker stream (treble clips near 100) |
| `DVF99_OPTS` | — | extra options: `-N` no auto-answer, `-Z N` speaker sleep delay (0 = never) |

Software volume is PulseAudio's.

## Pairing a handset

The pairing (and the DECT identity of the base) live in the base EEPROM, which the DVF9919 reads from the
host: `/var/lib/dvf99/nvram.bin`, imported from Android (`/data/cmbs/nvram.bin`) on first boot. A handset
paired under Android therefore works directly.

To pair a handset again:

```sh
sudo systemctl stop dvf99
sudo dvf99 -e /var/lib/dvf99/nvram.bin register     # registration open for 120 s
# put the handset in registration mode now
```

then power cycle the tablet. The base writes the new pairing into `nvram.bin`: from then on, **do not
restore an older copy** of the file (the handset and the base would no longer share the same key).

## Robustness

Rules learned the hard way:

- **Prefer a cold boot.** Restarting `dvf99` on an already running stack works a few times, then the base
  starts refusing channels or stops clocking the TDM link. `dvf99 reset` (CMBS SYS_RESET) brings the
  clock back but **breaks the radio link** with the handset: avoid it. A power cycle always recovers.
- **Second TARGET_UP.** After `run bootm` the stack reads its EEPROM, then restarts and announces itself a
  second time, forgetting the HELLO and SYS_START it already received. `dvf99` repeats the handshake as
  long as new TARGET_UP messages arrive.
- **Wait for the handset.** Opening AFE channels right after the start-up prevents the base from finding
  the handset. `dvf99` waits for `HS_IN_RANGE`, or any handset event, at most 30 s (`-I`), then opens them.
- **Stale channels.** A `dvf99` killed without releasing its channels leaves the class D amplifier
  allocated. At start-up `dvf99` releases channels `0xF0`..`0xF3`, and it releases its own on SIGTERM.
- The EEPROM file is rewritten by the base at every start: keep `nvram.android.bin` (the pristine copy)
  off the tablet as well.

## Protocol reference

### Framing

```
DA DA DA DA        sync word
u16 total length   = 8 + parameter length          ┐
u16 packet number  (always 0 from the host)        │ little-endian header
u16 event id                                       │
u16 parameter length                               ┘
parameters         list of IEs: u16 type, u16 length, value
```

Maximum message size 1024 bytes. Link-layer commands use event id `0xFF00 | n`: HELLO 1, HELLO_RPLY 2,
FLOW_NOK 3, FLOW_RESTART 4, RESET 5. No checksum.

### Start-up

```
(host)  run bootm\r                       on the DVF9919 U-Boot console
← TARGET_UP (125)                          ~2.5 s later
→ HELLO (0xFF01)   00 03 03 FF FF 00       TDM: DVF9919 master, PCM 2048 kHz, long sync, all slots, no flow control
← HELLO_RPLY       64 03 1a 00 00          stack version 03.64, build 0x1a
→ SYS_START (25)
← SYS_START_RES (26)
← PARAM_AREA_GET (76) … / PARAM_AREA_SET (78) …    the base reads/writes its EEPROM, 0xF0 bytes at a time
← TARGET_UP (125)                          second time: redo HELLO + SYS_START
```

EEPROM accesses carry IE `PARAMETER_AREA` (26): zone (0 = EEPROM), u32 offset, u16 length, data. The host
answers 77 / 79 with the data and IE `RESPONSE` (22) = 0. AFE requests sent while the base still reads
its EEPROM are refused (code 5).

### AFE requests

| Request (reply = id + 1) | IEs |
|---|---|
| `AFE_CHANNEL_ALLOCATE` 247 | `AFE_RESOURCE_TYPE` 88: u8, 0 = DAC/ADC, 1 = class D → reply: `MEDIACHANNEL` 10 (channel number) |
| `AFE_ENDPOINT_CONNECT` 236 | `AFE_ENDPOINT_CONNECT` 82: u8 destination, u8 source |
| `AFE_ENDPOINT_ENABLE` 238 / `DISABLE` 240 | `AFE_ENDPOINT` 83: u8 |
| `AFE_ENDPOINT_GAIN` 242 | `AFE_ENDPOINT_GAIN` 84: u8 endpoint, s16 steps |
| `DEM_CHANNEL_START` 61 / `STOP` 65 | `MEDIADESCRIPTOR` 12: u8 codec, u8 n, n codecs (5 = linear PCM 16 kHz, 6 = 8 kHz); `MEDIACHANNEL` 10: u32 channel, u32 TDM slot mask (bytes), u8 0 |
| `AFE_CHANNEL_DEALLOCATE` 249 | `MEDIACHANNEL` 10 |
| `GPIO_ENABLE` 254 / `DISABLE` 256 | `GPIO_ID` 91: u32 number, u8 bank (A = 0 … D = 3) |
| `GPIO_CONFIG_SET` 258 | `GPIO_ID` 91, `GPIO_MODE` 92 (1 = output), `GPIO_PULL_TYPE` 94, `GPIO_PULL_ENA` 95, `GPIO_VALUE` 93 |
| `PARAM_GET` 13 / `PARAM_SET` 15 | `PARAMETER` 16: u8 id, u8 type, u16 length, data |

AFE endpoint numbering **of this firmware** (it is shifted by 2 from 15 upwards compared with generic
CMBS 03.64 headers, which lack the two digital microphone inputs):

| Values | Endpoints |
|---|---|
| 0–2 | `SP_OUT`, `HS_SP_OUT`, `LINE0_OUT` |
| 3–8 | `MIC_IN`, `HS_MIC_IN`, `LINE1_IN`, `LINE0_IN`, `IN_SINGIN0`, `IN_SINGIN1` |
| 9–16 | ADC inputs: `DIFFIN0`, `DIFFIN1`, `SINGIN0−SINGIN1`, `VREF_OFFSET`, `MUTE_VREF`, `MUTE_FLOAT`, `DIGMIC1`, `DIGMIC2` |
| 17–24 | amplifier sources: `DAC0`, `DAC1`, `DAC0_INV`, `DAC1_INV`, `SINGIN`, `DIFFIN0`, `DIFFIN1`, `MUTE` |
| 25–28 | `AMP_OUT0..3` |
| 29–32 | preamplifiers `AMP_SINGIN0/1`, `AMP_DIFFIN0/1` |
| 33–35 | `ADC0..2` |
| 36–37 | microphone bias `MIC0`, `MIC1` |

Gains: inputs 0..31; outputs 32 = 0 dB, 33 = −6, 34 = −12, 35 = −18, 36 = −24 dB. Parameters 55/56
(`DAC1_VOL`/`DAC2_VOL`) are the wired-handset sidetone, not a playback volume: leave them alone.

### Speaker sequence (as done by Android)

| | Woofer | Tweeter |
|---|---|---|
| channel | `AFE_CHANNEL_ALLOCATE` resource 0 (DAC/ADC) | resource **1 (class D)** |
| routing | `AMP_OUT2` (27) ← `DAC0` (17), `AMP_OUT3` (28) ← `DAC0_INV` (19): bridged | none |
| enable, gain | enable 27 and 28, gain on both | — |
| media | `DEM_CHANNEL_START` codec 5, slots `0x0F00` | slots `0x00F0` |
| amplifier | `GPIO_ENABLE` D10, `GPIO_CONFIG_SET` D10 = 1 | — |

Slot masks count **bytes** of the 32-byte frame (2.048 MHz / 8 kHz = 256 bits): `(wideband ? 0xF : 0x3) << n`.

### Calls from the handset

```
← CALL_ESTABLISH (46)     CALLINSTANCE, CALLERPARTY "1", MEDIADESCRIPTOR
→ CALL_PROGRESS (47)      CALLINSTANCE, MEDIADESCRIPTOR {5, 2, [5, 6]}, CALLPROGRESS 2
← CALL_MEDIA_UPDATE (54)  MEDIACHANNEL {0, …}       the call channel is number 0
→ CALL_ANSWER (48)        CALLINSTANCE
→ DEM_CHANNEL_START (61)  MEDIACHANNEL {0, 0x000F, 0}
← CALL_RELEASE (49)       on hang-up → DEM_CHANNEL_STOP (65), CALL_RELEASECOMPLETE (50)
```

The call channel is only accepted at **slot 0 with contiguous bytes** (`0x000F`; `0x0033` or `0xF000` are
refused). Handset state also arrives as event **274** (Gigaset extension): IE 28 handset number, IE 32
data whose 8th byte is the hook state (0 off-hook, 1 on-hook); the volume keys produce it too.

### Android's CMBS trace

`CMBSServer` (the Android daemon that owns `/dev/ttymxc2`) writes every CMBS exchange, decoded, to
`/data/cmbs/cmbs_log.txt` (current boot) and `cmbs_temp.txt` (previous one); a `mUartDump` option also
exists for raw dumps. What it shows:

- at start-up, once and for good: `PARAM_SET DHSG ENABLE = 1`, then `AFE_ENDPOINT_ENABLE AMP_DIFFIN0` (31)
  and `AFE_ENDPOINT_ENABLE MIC1` (37, microphone bias), then an `AFE_AUX_MEASUREMENT` on DCIN02 (RJ9 port);
- for playback: DAC channel `f0` with OUT2 ← DAC0, OUT3 ← DAC0_INV, both at −12 dB, `0x0F00`, then GPIO
  bank 3 pin 10 enabled and configured as output set; class D channel `f2` on `0x00F0`; codec
  `PCM_LINEAR_WB`; on stop, GPIO disabled first.

- **front microphone recording** (recorder app): DAC channel `f0` and class D `f2` started first as for
  playback, then `AFE_CHANNEL_ALLOCATE` (resource 0) → `f1`, instance 1; `AFE_ENDPOINT_CONNECT 15 ← 34`
  (the trace names them ADC1 ← ADC_IN_DIGMIC1); `AFE_ENDPOINT_ENABLE 39`; `DEM_CHANNEL_START f1`,
  `PCM_LINEAR_WB`, slots `0x000F`; on stop `AFE_ENDPOINT_DISABLE 38`, `DEM_CHANNEL_STOP`, `DEALLOCATE`.
  These are raw on-wire values: our earlier attempts (33 ← 15, enable 35) came from a wrong reading of the
  HAL's table.

`mictest` now replays the start-up part (AMP_DIFFIN0 + MIC1), then this recording sequence and a few variants.
**Result: the front microphone answers** (`15 ← 34`, enable 39, also with 39 + 38, and with source 35):
TDM word 0 goes from 256 in silence to 5000-8000 when speaking. Word 1 stays at 0 and every value is a
multiple of 256 (silence = `0xFF00`): one useful byte per frame, which looks like 8 kHz µ-law rather than
the linear wideband PCM that was asked for. `mictest` now also dumps words 0-3 while speaking to
`/tmp/mictest-N.raw` (S16, 4 channels, 8000 frames/s) and tries the narrowband codec.

The dumps settled it: **signed linear PCM, 8 kHz, only the most significant byte** (values spread around
`0x00`/`0xFF`, speech spectrum), whatever codec is requested, and the same with source 34 or 35. The
service now opens the microphone itself (`front` pipe → PulseAudio input **Micro de façade**, the default
one): `AMP_DIFFIN0` + `MIC1` at start-up, then the recording sequence above on the second channel.
**With the speaker channels open, as in the service, the microphone comes like the handset: linear 16-bit
PCM, 16 kHz on words 0-1** (only `mictest`, without them, got the high byte of word 0); `-Y N` is a digital
gain (default 1, 0 = off). During a handset call it closes the microphone channel, which uses the same
slots, and reopens it afterwards.

### Handset battery

Event 274 (Gigaset data) carries, in its IE 32, `81 05 05 01 02 S N`: **S** = 1 when the handset sits on its
cradle (on hook, hence charging), 0 when lifted; **N** = battery level in % (seen rising from 86 to 93 % on the
cradle over a few hours). `dvf99` copies them into the `dvf99-handset` battery of kernel patch 0006
(`capacity`, `status` Charging/Full/Discharging, `present`), which UPower and Phosh show as a battery; the
always-online `dvf99-mains` supply keeps the system from acting on a low handset battery.

### TDM format

Measured with [tools/debug/ssireg](../../tools/debug/ssireg/ssireg.c): 8000 frames/s, frame sync is a
**one-bit positive pulse** (despite the "long sync" setting), data valid on the falling edge: SSI2
`STCR = 0x280`, which the device tree expresses as `left_j` + `bitclock-inversion`.

## Front microphone (open issue)

The glass has a microphone opening, but its routing is unknown. The vendor library describes six audio
"devices"; devices 2 to 5 route a microphone to ADC0 at slot 0:

| Device | Slot | Routing | Enabled | GPIO |
|---|---|---|---|---|
| 0 woofer | 8 | OUT2 ← DAC0, OUT3 ← DAC0_INV | OUT2, OUT3 | D10 |
| 1 tweeter | 4 | class D | — | — |
| 2 | 0 | ADC0 ← DIGMIC1 (15) | — | — |
| 3 | 0 | OUT0 ← DAC0, OUT1 ← DAC0_INV, ADC0 ← SINGIN0−SINGIN1 (11) | OUT0, OUT1, AMP_SINGIN0/1 | D11 |
| 4 | 0 | same, ADC0 ← DIFFIN0 (9) | OUT0, OUT1, AMP_DIFFIN0, ADC0 | D11 |
| 5 | 0 | same, ADC0 ← DIFFIN1 (10) | OUT0, OUT1, AMP_DIFFIN1, ADC0 | D11 |

`dvf99 -A mictest` and `dvf99 -F SRC` try these inputs (with GPIO D11, microphone bias and +30 dB):
channels are accepted and samples arrive, but none reacts to voice. The next step is to observe Android
during a hands-free call (CMBS capture on UART3, or the vendor log).

What Android's ALSA card `dvf9919-audio` reports (`/proc/asound/dvf9919audio/pcm0*/sub0/hw_params`):

| Stream | Format | Meaning |
|---|---|---|
| playback | 3 channels, 16 kHz, period 160 | the same 3 pairs of TDM words as ours (call, tweeters, woofer): no stereo |
| capture, front microphone recording | **6 channels**, 16 kHz, period 80 | **12 TDM words**: the receive side goes past the 6 words our card read |

Since then the `DVF99` card runs 12 TDM words in both directions (`fsl_ssi` in synchronous mode needs
the same channel count for playback and capture; `dvf99` sends zeros in words 6-11), and `mictest` prints the peaks of every word, flagging those that react to voice. Result (kernel #23, the
five `mictest` routings): **no word 0..11 reacts**; word 0 stays at a constant 256, the others at 0 or 1. The
microphone samples do not reach the TDM bus at all, something is missing on the DVF9919 side.

## `dvf99` reference

Build: `tools/dvf99/build.sh` (static ARM binary) or the device package. Run as root; stop
`dvf99.service` first, since only one program can use the UART.

| Command | Effect |
|---|---|
| `dvf99 [-q] [-A] [-e NVRAM] service` | what the service runs: start-up, speaker path, auto-answer, audio engine |
| `dvf99 [-e NVRAM] register` | open registration for 120 s |
| `dvf99 [-e NVRAM] init` | boot the stack if needed, HELLO, SYS_START, then exit |
| `dvf99 console` | DVF9919 U-Boot console; commands that write its flash are refused (unless `-U`) |
| `dvf99 sniff` / `dvf99 -f FILE sniff` | decode CMBS frames live or from a raw UART capture |
| `dvf99 [-e NVRAM] speaker [-t TONE\|-1] [-k]` | open the speaker path, optionally with a tone |
| `dvf99 [-e NVRAM] param N [=HEX] …` | read / write CMBS parameters |
| `dvf99 -A -e NVRAM mictest` | front microphone search |
| `dvf99 -e NVRAM [-g GAIN] outtest` | plays a 1 kHz sine over TDM on each possible output in turn (class D, AMP_OUT0..3 with GPIO D10/D11), then tries two class D channels at once: which physical speaker is wired to what |

| Option | Meaning |
|---|---|
| `-d TTY` | UART (default `/dev/ttymxc2`) |
| `-e NVRAM` | emulated EEPROM file (read and **written** by the base) |
| `-q` | log only useful events |
| `-A` | audio engine (pipes in `/run/dvf99`); `-M`: no capture |
| `-g GAIN` | woofer gain 32..36 |
| `-X PCT` | tweeter level |
| `-Z S` | speaker sleep delay, 0 = never |
| `-N` | no auto-answer |
| `-I S` | maximum wait for the handset before opening channels (default 30) |
| `-P` / `-W` / `-C SLOTS` | woofer / tweeter / call slot masks |
| `-T 0\|1\|-1` | TDM role in HELLO (0 = DVF9919 master, default) |
| `-l FILE` | log file |

Never run `saveenv`, `upgrade` or `u-boot-upgrade` on the DVF9919 console: they erase or rewrite the
module flash.

For research, the vendor test tool `cmbs_host` (on the Android system partition) carries full DWARF debug
information; [tools/debug/dwarf2h.py](../../tools/debug/dwarf2h.py) turns it into a C header
(`python3 tools/debug/dwarf2h.py cmbs_host cmbs_types.h`, needs `pyelftools`). The result is derived from
vendor code: keep it local.
