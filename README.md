# rpi5video – VDR-Ausgabe-Plugin für Raspberry Pi 5

VDR 2.7.9 Ausgabe-Plugin für den Raspberry Pi 5.  
Hardware-Decoder über **FFmpeg + v4l2_request**, Ausgabe über **DRM Atomic**.

---

## Architektur

```
VDR (PES-Stream)
    │
    ▼
cRpi5Device          ← VDR-Device-Schnittstelle, PES-Routing
    ├── cAudioProcessor  ← FFmpeg-Decode + ALSA-Ausgabe (IEC61937/PCM)
    └── cRpi5Decoder     ← FFmpeg/v4l2_request Decode + Filter-Graph
            │
            ▼ Rpi5Frame (AV_PIX_FMT_DRM_PRIME)
        cRpi5Display     ← DRM-Atomic Page-Flip
```

### Wichtige Unterschiede zu VAAPI

| | VAAPI (x86) | v4l2_request (RPi5) |
|---|---|---|
| HW-Device-Typ | `AV_HWDEVICE_TYPE_VAAPI` | `AV_HWDEVICE_TYPE_V4L2REQUEST` |
| Ausgabeformat | `AV_PIX_FMT_VAAPI` → hwmap → DRM-PRIME | `AV_PIX_FMT_DRM_PRIME` direkt |
| Filter | `scale_vaapi`, `deinterlace_vaapi` | `hwdownload` + `bwdif` (CPU) |
| VPP | Ja (via VAEntrypointVideoProc) | Nein (Software-Filter) |
| Modifier | Tiling-Modifier | `DRM_FORMAT_MOD_LINEAR` |

---

## Voraussetzungen (Buildroot)

### Kernel
```
CONFIG_VIDEO_V4L2_REQUEST=y
CONFIG_VIDEO_CODEC_V4L2=y
CONFIG_VIDEO_HEVC_DECODER=y      # RPi5 HEVC
CONFIG_VIDEO_H264_DECODER=y      # RPi5 H.264
CONFIG_DRM_VC4=y                 # VideoCore-VII DRM
CONFIG_DRM_PANEL_SIMPLE=y
```

### Buildroot-Pakete
```
BR2_PACKAGE_FFMPEG=y
BR2_PACKAGE_FFMPEG_ENABLE_V4L2REQUEST=y   # wichtig!
BR2_PACKAGE_FFMPEG_ENABLE_LIBDRM=y
BR2_PACKAGE_ALSA_LIB=y
BR2_PACKAGE_LIBDRM=y
BR2_PACKAGE_VDR=y
```

### FFmpeg muss mit v4l2_request gebaut werden
```bash
./configure \
  --enable-v4l2-request \
  --enable-libdrm \
  --enable-alsa \
  --disable-vaapi          # kein VAAPI auf RPi5
```

---

## Bauen

```bash
# Im VDR-Plugin-Verzeichnis
export VDRDIR=/path/to/vdr/source
make

# Installieren
make install DESTDIR=/target
```

---

## Starten

```bash
vdr -P "rpi5video -d /dev/dri/card0 -a default -r 1920x1080@50"
```

### Kommandozeilen-Optionen

| Option | Beschreibung | Standard |
|--------|-------------|---------|
| `-d DEV` | DRM-Gerät | `/dev/dri/card0` |
| `-a DEV` | ALSA-Gerät | `default` |
| `-r WxH@R` | Auflösung | `1920x1080@50` |

---

## Setup-Menü (VDR)

| Einstellung | Beschreibung |
|-------------|-------------|
| Audio-Latenz (ms) | A/V-Sync-Korrektur, positiv = Video wartet |
| Deinterlacing | bwdif (Software) |
| Rauschfilter | hqdn3d (Software) |
| Schärfe-Filter | unsharp (Software) |

---

## V4L2-Geräte auf dem RPi5

Der VideoCore-VII-Decoder erscheint als:
```
/dev/video10  ← H.264 Decode
/dev/video11  ← HEVC Decode
/dev/video12  ← JPEG
```

Prüfen mit:
```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video10 --list-formats
```

---

## Bekannte Einschränkungen

- **Kein Hardware-Deinterlacing**: RPi5 V4L2-M2M hat kein VPP.  
  Software-bwdif wird verwendet (CPU-Last ca. +5 %).
- **Kein MPEG-2 Hardware-Decode** auf RPi5 (Lizenz).  
  FFmpeg-Software-Decode als Fallback.
- **Audio-Passthrough**: AC-3/E-AC-3 über HDMI wenn Receiver es unterstützt.

---

## Dateistruktur

```
rpi5video/
├── Makefile
├── common.h        ← Typen, Konstanten, AvErr()
├── config.h/.cpp   ← Plugin-Konfiguration
├── pes.h/.cpp      ← PES-Parsing, Codec-Erkennung
├── audio.h/.cpp    ← ALSA-Ausgabe (portiert von vaapivideo)
├── decoder.h/.cpp  ← V4L2-Request-Decoder + Filter
├── display.h/.cpp  ← DRM-Atomic-Ausgabe
├── device.h/.cpp   ← VDR-Device
└── plugin.cpp      ← Plugin-Einstiegspunkt
```
