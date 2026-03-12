// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file device.cpp
 * @brief VDR-Device-Integration, PES-Routing und Lebenszyklus
 */

#include "device.h"
#include "audio.h"
#include "common.h"
#include "config.h"
#include "decoder.h"
#include "display.h"
#include "pes.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}
#pragma GCC diagnostic pop

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <vdr/device.h>
#include <vdr/thread.h>
#include <vdr/tools.h>

// ============================================================================
// === KONSTRUKTOR / DESTRUKTOR ===
// ============================================================================

cRpi5Device::cRpi5Device() {
    isyslog("rpi5video/device: erstellt");
    SetDescription("RPi5 Video Device");
    SetVideoFormat(true);
}

cRpi5Device::~cRpi5Device() noexcept {
    if (IsPrimaryDevice()) cDevice::MakePrimaryDevice(false);
    DetachAllReceivers();
    if (audioProcessor || decoder || display) Stop();
    ReleaseHardware();
    dsyslog("rpi5video/device: zerstört");
}

// ============================================================================
// === VDR-DEVICE-SCHNITTSTELLE ===
// ============================================================================

[[nodiscard]] auto cRpi5Device::CanReplay() const -> bool {
    return (initState.load(std::memory_order_relaxed) == 2) && decoder && decoder->IsReady();
}
[[nodiscard]] auto cRpi5Device::HasDecoder()  const -> bool { return decoder && decoder->IsReady(); }
[[nodiscard]] auto cRpi5Device::HasIBPTrickSpeed() -> bool  { return true; }
[[nodiscard]] auto cRpi5Device::DeviceType()  const -> cString { return "RPi5"; }
[[nodiscard]] auto cRpi5Device::Ready()             -> bool  { return initState.load(std::memory_order_acquire) == 2; }

[[nodiscard]] auto cRpi5Device::GetSTC() -> int64_t {
    if (!decoder) [[unlikely]] return -1;
    const int64_t pts = decoder->GetLastPts();
    return (pts == AV_NOPTS_VALUE) ? -1 : pts;
}

auto cRpi5Device::GetOsdSize(int &Width, int &Height, double &PixelAspect) -> void {
    if (!display) [[unlikely]] {
        Width  = static_cast<int>(rpi5Config.display.GetWidth());
        Height = static_cast<int>(rpi5Config.display.GetHeight());
        PixelAspect = static_cast<double>(Width) / Height;
        return;
    }
    if (osdWidth > 0 && osdHeight > 0) [[likely]] {
        Width = osdWidth; Height = osdHeight;
        PixelAspect = display->GetAspectRatio();
        return;
    }
    if (display->IsInitialized()) {
        osdWidth  = static_cast<int>(display->GetOutputWidth());
        osdHeight = static_cast<int>(display->GetOutputHeight());
    } else {
        osdWidth  = static_cast<int>(rpi5Config.display.GetWidth());
        osdHeight = static_cast<int>(rpi5Config.display.GetHeight());
    }
    Width = osdWidth; Height = osdHeight;
    PixelAspect = display->GetAspectRatio();
}

auto cRpi5Device::GetVideoSize(int &Width, int &Height, double &VideoAspect) -> void {
    if (!HasDecoder()) [[unlikely]] { Width = Height = 0; VideoAspect = 1.0; return; }
    Width       = decoder->GetStreamWidth();
    Height      = decoder->GetStreamHeight();
    VideoAspect = decoder->GetStreamAspect();
    if (Width == 0 || Height == 0) VideoAspect = 1.0;
}

auto cRpi5Device::MakePrimaryDevice(bool On) -> void {
    cDevice::MakePrimaryDevice(On);
    if (On && IsPrimaryDevice())
        isyslog("rpi5video/device: als primäres Gerät aktiviert");
}

// ============================================================================
// === WIEDERGABE ===
// ============================================================================

[[nodiscard]] auto cRpi5Device::SetPlayMode(ePlayMode PlayMode) -> bool {
    static constexpr const char *kModeNames[] = {
        "pmNone","pmAudioVideo","pmAudioOnly","pmAudioOnlyBlack","pmVideoOnly","pmExtern"
    };
    const auto idx = static_cast<unsigned>(PlayMode);
    dsyslog("rpi5video/device: SetPlayMode(%s)",
            idx < std::size(kModeNames) ? kModeNames[idx] : "unbekannt");

    paused.store(false, std::memory_order_relaxed);

    switch (PlayMode) {
        case pmNone:
            previousVideoCodec = videoCodecId.load(std::memory_order_relaxed);
            videoCodecId.store(AV_CODEC_ID_NONE, std::memory_order_relaxed);
            audioCodecId.store(AV_CODEC_ID_NONE, std::memory_order_relaxed);
            liveMode = false;
            trickSpeed.store(0, std::memory_order_release);
            if (decoder) decoder->SetTrickSpeed(0);
            prevAudioStreamId.store(0xFF, std::memory_order_relaxed);
            codecHysteresis = AV_CODEC_ID_NONE;
            codecHysteresisCount = 0;
            videoCodecCandidate = AV_CODEC_ID_NONE;
            videoCodecCandidateCount = 0;
            Clear();
            break;
        case pmAudioVideo:
        case pmAudioOnly:
        case pmAudioOnlyBlack:
        case pmVideoOnly:
            Clear();
            break;
        default:
            break;
    }
    return true;
}

auto cRpi5Device::Clear() -> void {
    cDevice::Clear();
    if (display)        display->BeginStreamSwitch();
    if (decoder)        decoder->Clear();
    if (display)        display->EndStreamSwitch();
    if (audioProcessor) audioProcessor->Clear();
}

auto cRpi5Device::Play() -> void {
    cDevice::Play();
    if (trickSpeed.load(std::memory_order_relaxed) != 0) {
        trickSpeed.store(0, std::memory_order_release);
        if (decoder) decoder->SetTrickSpeed(0);
    }
    paused.store(false, std::memory_order_relaxed);
}

auto cRpi5Device::Freeze() -> void {
    cDevice::Freeze();
    paused.store(true, std::memory_order_relaxed);
    if (decoder)        decoder->DrainQueue();
    if (audioProcessor) audioProcessor->Clear();
}

auto cRpi5Device::Mute() -> void {
    cDevice::Mute();
    if (audioProcessor) audioProcessor->Clear();
}

auto cRpi5Device::TrickSpeed(int Speed, bool Forward) -> void {
    const bool isFast = !paused.load(std::memory_order_relaxed);
    trickSpeed.store(Speed, std::memory_order_release);
    if (decoder)        decoder->SetTrickSpeed(Speed, Forward, isFast);
    if (audioProcessor) audioProcessor->Clear();
}

auto cRpi5Device::StillPicture(const uchar *Data, int Length) -> void {
    if (!Data || Length <= 0) return;
    if (Data[0] == 0x47) { cDevice::StillPicture(Data, Length); return; }
    const bool wasPaused = paused.exchange(false, std::memory_order_relaxed);
    PlayVideo(Data, Length);
    if (wasPaused) paused.store(true, std::memory_order_relaxed);
}

auto cRpi5Device::SetVolumeDevice(int Volume) -> void {
    if (audioProcessor) audioProcessor->SetVolume(Volume);
}

auto cRpi5Device::SetAudioTrackDevice(eTrackType Type) -> void {
    (void)Type;
    prevAudioStreamId.store(0xFF, std::memory_order_relaxed);
    if (audioProcessor) audioProcessor->Clear();
}

[[nodiscard]] auto cRpi5Device::Flush(int TimeoutMs) -> bool {
    if (!decoder) return true;
    const cTimeMs timeout(TimeoutMs);
    while (!decoder->IsQueueEmpty() && !timeout.TimedOut())
        cCondWait::SleepMs(10);
    return decoder->IsQueueEmpty();
}

[[nodiscard]] auto cRpi5Device::Poll(cPoller & /*Poller*/, int TimeoutMs) -> bool {
    if (!decoder) return true;
    if (liveMode)  return true;

    const int curSpeed = trickSpeed.load(std::memory_order_relaxed);
    auto hasSpace = [&]() -> bool {
        if (curSpeed != 0)
            return decoder->IsReadyForNextTrickFrame() && decoder->GetQueueSize() < DECODER_TRICK_QUEUE_DEPTH;
        return !decoder->IsQueueFull() && (!audioProcessor || !audioProcessor->IsQueueFull());
    };

    if (hasSpace()) return true;
    if (TimeoutMs > 0) {
        const cTimeMs timeout(TimeoutMs);
        while (!hasSpace() && !timeout.TimedOut()) cCondWait::SleepMs(5);
        return hasSpace();
    }
    return false;
}

[[nodiscard]] auto cRpi5Device::PlayVideo(const uchar *Data, int Length) -> int {
    if (!Data || Length <= 0) return Length;
    if (paused.load(std::memory_order_relaxed) && trickSpeed.load(std::memory_order_relaxed) == 0)
        return Length;
    if (!decoder || !decoder->IsReady()) return Length;

    const auto pes = ParsePes({Data, static_cast<size_t>(Length)});
    if (!pes.isVideo || pes.payloadSize == 0) return Length;

    const AVCodecID currentCodec = videoCodecId.load(std::memory_order_relaxed);

    if (currentCodec == AV_CODEC_ID_NONE) [[unlikely]] {
        liveMode = Transferring();
        const AVCodecID det = ::DetectVideoCodec({pes.payload, pes.payloadSize});
        if (det == AV_CODEC_ID_NONE) return Length;

        if (det == previousVideoCodec && previousVideoCodec != AV_CODEC_ID_NONE) {
            if (det == videoCodecCandidate) ++videoCodecCandidateCount;
            else { videoCodecCandidate = det; videoCodecCandidateCount = 1; }
            if (videoCodecCandidateCount < 2) return Length;
        }
        videoCodecCandidate = AV_CODEC_ID_NONE;
        videoCodecCandidateCount = 0;

        if (!decoder->OpenCodec(det)) return Length;
        videoCodecId.store(det, std::memory_order_relaxed);
        isyslog("rpi5video/device: Video-Codec %s (%s)",
                avcodec_get_name(det), liveMode ? "live" : "replay");
    }

    if (!liveMode) {
        if (trickSpeed.load(std::memory_order_relaxed) != 0) {
            if (!decoder->IsReadyForNextTrickFrame() || decoder->GetQueueSize() >= DECODER_TRICK_QUEUE_DEPTH)
                return 0;
        } else if (decoder->IsQueueFull()) {
            return 0;
        }
    }

    decoder->EnqueueData(pes.payload, pes.payloadSize, pes.pts);
    return Length;
}

[[nodiscard]] auto cRpi5Device::PlayAudio(const uchar *Data, int Length, uchar Id) -> int {
    if (!Data || Length <= 0) return Length;
    if (paused.load(std::memory_order_relaxed) || trickSpeed.load(std::memory_order_relaxed) != 0)
        return Length;
    if (!audioProcessor || !audioProcessor->IsInitialized()) return Length;

    const uchar lastId = prevAudioStreamId.load(std::memory_order_relaxed);
    if (lastId != Id) [[unlikely]] {
        audioCodecId.store(AV_CODEC_ID_NONE, std::memory_order_relaxed);
        codecHysteresis = AV_CODEC_ID_NONE;
        codecHysteresisCount = 0;
    }
    prevAudioStreamId.store(Id, std::memory_order_relaxed);

    const auto pes = ParsePes({Data, static_cast<size_t>(Length)});
    if (!pes.isAudio || pes.payloadSize == 0) return Length;

    const AVCodecID currentCodec = audioCodecId.load(std::memory_order_relaxed);

    if (currentCodec == AV_CODEC_ID_NONE) {
        if (!liveMode) liveMode = Transferring();
        const AVCodecID det = ::DetectAudioCodec({pes.payload, pes.payloadSize});
        if (det == AV_CODEC_ID_NONE) return Length;
        if (!audioProcessor->OpenCodec(det, 48000, 2)) return Length;
        audioCodecId.store(det, std::memory_order_relaxed);
    } else {
        // Hysterese für Codec-Wechsel (3 aufeinanderfolgende Erkennungen)
        const AVCodecID det = ::DetectAudioCodec({pes.payload, pes.payloadSize});
        if (det != AV_CODEC_ID_NONE && det != currentCodec) {
            if (det == codecHysteresis) {
                if (++codecHysteresisCount >= 3) {
                    if (audioProcessor->OpenCodec(det, 48000, 2))
                        audioCodecId.store(det, std::memory_order_relaxed);
                    codecHysteresis = AV_CODEC_ID_NONE;
                    codecHysteresisCount = 0;
                }
            } else {
                codecHysteresis = det;
                codecHysteresisCount = 1;
            }
        }
    }

    if (!liveMode && audioProcessor->IsQueueFull()) return 0;
    audioProcessor->Decode(pes.payload, pes.payloadSize, pes.pts);
    return Length;
}

// ============================================================================
// === HARDWARE-INITIALISIERUNG ===
// ============================================================================

[[nodiscard]] auto cRpi5Device::Initialize(
    std::string_view drmDevicePath,
    std::string_view audioDevicePath) -> bool
{
    int expected = 0;
    if (!initState.compare_exchange_strong(expected, 1)) [[unlikely]] {
        esyslog("rpi5video/device: bereits initialisiert (state=%d)", expected);
        return false;
    }

    drmPath    = drmDevicePath;
    audioDevice= audioDevicePath;

    if (!OpenHardware()) {
        esyslog("rpi5video/device: Hardware-Initialisierung fehlgeschlagen");
        initState.store(0); return false;
    }

    audioProcessor = std::make_unique<cAudioProcessor>();
    if (!audioProcessor->Initialize(audioDevice)) {
        esyslog("rpi5video/device: Audio-Initialisierung fehlgeschlagen");
        audioProcessor.reset(); ReleaseHardware(); initState.store(0); return false;
    }

    display = std::make_unique<cRpi5Display>();
    if (!display->Initialize(drmFd, rpi5.hwDeviceRef, crtcId, connectorId, activeMode)) {
        esyslog("rpi5video/device: Display-Initialisierung fehlgeschlagen");
        display.reset(); audioProcessor->Stop(); audioProcessor.reset();
        ReleaseHardware(); initState.store(0); return false;
    }

    decoder = std::make_unique<cRpi5Decoder>(display.get(), &rpi5);
    if (!decoder->Initialize()) {
        esyslog("rpi5video/device: Decoder-Initialisierung fehlgeschlagen");
        decoder.reset(); display->Shutdown(); display.reset();
        audioProcessor->Stop(); audioProcessor.reset();
        ReleaseHardware(); initState.store(0); return false;
    }

    decoder->SetAudioProcessor(audioProcessor.get());

    osdWidth  = static_cast<int>(display->GetOutputWidth());
    osdHeight = static_cast<int>(display->GetOutputHeight());

    initState.store(2, std::memory_order_release);
    isyslog("rpi5video/device: initialisiert DRM=%s Audio=%s %ux%u@%uHz",
            drmPath.c_str(), audioDevice.c_str(),
            rpi5Config.display.width, rpi5Config.display.height, rpi5Config.display.refreshRate);
    return true;
}

auto cRpi5Device::Stop() -> void {
    if (display)        { display->Shutdown();       display.reset();        }
    if (decoder)        { decoder->Shutdown();        decoder.reset();        }
    if (audioProcessor) { audioProcessor->Stop();    audioProcessor.reset(); }
}

auto cRpi5Device::Detach() -> void {
    isyslog("rpi5video/device: Hardware wird freigegeben");
    Stop();
    if (drmFd >= 0) drmDropMaster(drmFd);
    ReleaseHardware();
    osdWidth = osdHeight = 0;
    initState.store(0, std::memory_order_release);
}

[[nodiscard]] auto cRpi5Device::Attach() -> bool {
    if (drmPath.empty()) { esyslog("rpi5video/device: kein DRM-Pfad gespeichert"); return false; }
    return Initialize(drmPath, audioDevice);
}

[[nodiscard]] auto cRpi5Device::OpenHardware() -> bool {
    if (drmPath.empty()) { esyslog("rpi5video/device: kein DRM-Gerät angegeben"); return false; }

    if (access(drmPath.c_str(), R_OK | W_OK) != 0) {
        esyslog("rpi5video/device: '%s' nicht zugänglich -- %s", drmPath.c_str(), strerror(errno));
        return false;
    }

    drmFd = open(drmPath.c_str(), O_RDWR | O_CLOEXEC);
    if (drmFd < 0) { esyslog("rpi5video/device: '%s' öffnen fehlgeschlagen: %s", drmPath.c_str(), strerror(errno)); return false; }

    if (!SelectDrmConnector()) {
        close(drmFd); drmFd = -1; return false;
    }

    // V4L2-Request Hardware-Device-Kontext erstellen
    // Beim RPi5 ist der V4L2-M2M-Decoder auf /dev/video10 (H.264) und /dev/video11 (HEVC)
    // FFmpeg's v4l2_request-Backend findet das automatisch über das DRM-FD
    AVBufferRef *hwDevice = nullptr;
    // Wir übergeben den DRM-Gerätepfad als "device" für v4l2_request
    const int ret = av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_V4L2REQUEST,
                                            drmPath.c_str(), nullptr, 0);
    if (ret < 0) {
        esyslog("rpi5video/device: V4L2-Request-Kontext fehlgeschlagen: %s", AvErr(ret).data());
        esyslog("rpi5video/device: Test: v4l2-ctl --list-devices");
        close(drmFd); drmFd = -1; return false;
    }

    rpi5.hwDeviceRef = hwDevice;
    rpi5.drmFd       = drmFd;

    if (!ProbeDecoderCaps()) {
        av_buffer_unref(&rpi5.hwDeviceRef);
        close(drmFd); drmFd = -1; return false;
    }
    return true;
}

auto cRpi5Device::ReleaseHardware() -> void {
    if (rpi5.hwDeviceRef) {
        av_buffer_unref(&rpi5.hwDeviceRef);
        rpi5.drmFd = -1;
    }
    if (drmFd >= 0) { close(drmFd); drmFd = -1; }
}

// ============================================================================
// === DECODER-FÄHIGKEITEN ERMITTELN ===
// ============================================================================

[[nodiscard]] auto cRpi5Device::ProbeDecoderCaps() -> bool {
    // Beim RPi5 mit v4l2_request prüfen wir welche Codecs der Kernel-Decoder
    // unterstützt, indem wir versuchen einen AVCodecContext zu öffnen.
    // Anders als bei VAAPI gibt es keine vaQueryConfigProfiles().

    rpi5.hwH264  = false;
    rpi5.hwHevc  = false;
    rpi5.hwMpeg2 = false;

    struct CodecProbe { AVCodecID id; bool &flag; const char *name; };
    const CodecProbe probes[] = {
        {AV_CODEC_ID_H264,       rpi5.hwH264,  "H.264"},
        {AV_CODEC_ID_HEVC,       rpi5.hwHevc,  "H.265/HEVC"},
        {AV_CODEC_ID_MPEG2VIDEO, rpi5.hwMpeg2, "MPEG-2"},
    };

    for (const auto &[codecId, flag, name] : probes) {
        const AVCodec *dec = avcodec_find_decoder(codecId);
        if (!dec) continue;

        for (int i = 0;; ++i) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(dec, i);
            if (!cfg) break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                cfg->device_type == AV_HWDEVICE_TYPE_V4L2REQUEST) {
                flag = true;
                break;
            }
        }
        dsyslog("rpi5video/device: %s: %s", name, flag ? "Hardware" : "Software");
    }

    isyslog("rpi5video/device: Decoder -- h264=%s hevc=%s mpeg2=%s",
            rpi5.hwH264  ? "hw" : "sw",
            rpi5.hwHevc  ? "hw" : "sw",
            rpi5.hwMpeg2 ? "hw" : "sw");

    // Deinterlacing-Modus (Software, da v4l2_request kein VPP hat)
    rpi5.deinterlaceMode = "bwdif";
    rpi5.hasDenoise      = false; // Software-Filter hqdn3d verfügbar
    rpi5.hasSharpness    = false;

    return true; // Immer erfolgreich -- Software-Fallback immer möglich
}

// ============================================================================
// === DRM-CONNECTOR AUSWAHL ===
// ============================================================================

[[nodiscard]] auto cRpi5Device::SelectDrmConnector() -> bool {
    if (drmSetClientCap(drmFd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        esyslog("rpi5video/device: DRM Atomic nicht verfügbar"); return false;
    }

    std::unique_ptr<drmModeRes, FreeDrmResources> resources{drmModeGetResources(drmFd)};
    if (!resources) { esyslog("rpi5video/device: DRM-Ressourcen nicht abrufbar"); return false; }

    const auto targetW = rpi5Config.display.GetWidth();
    const auto targetH = rpi5Config.display.GetHeight();
    const auto targetR = rpi5Config.display.GetRefreshRate();

    for (int i = 0; i < resources->count_connectors; ++i) {
        std::unique_ptr<drmModeConnector, FreeDrmConnector> conn{
            drmModeGetConnector(drmFd, resources->connectors[i])};
        if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0)
            continue;

        // Modus-Auswahl: (1) exakte Übereinstimmung, (2) bevorzugter Modus, (3) erster Modus
        drmModeModeInfo bestMode{};
        int bestScore = -1;
        for (int m = 0; m < conn->count_modes; ++m) {
            const auto &mode = conn->modes[m];
            int score = 0;
            if (mode.hdisplay == targetW && mode.vdisplay == targetH && mode.vrefresh == targetR)
                score = 3;
            else if (mode.type & DRM_MODE_TYPE_PREFERRED)
                score = 2;
            else if (m == 0)
                score = 1;
            if (score > bestScore) { bestScore = score; bestMode = mode; }
        }
        activeMode = bestMode;

        // CRTC-ID aus Connector-Property lesen
        std::unique_ptr<drmModeObjectProperties, FreeDrmObjectProperties> props{
            drmModeObjectGetProperties(drmFd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR)};
        if (props) {
            for (uint32_t p = 0; p < props->count_props; ++p) {
                std::unique_ptr<drmModePropertyRes, FreeDrmProperty> prop{
                    drmModeGetProperty(drmFd, props->props[p])};
                if (prop && std::strcmp(prop->name, "CRTC_ID") == 0) {
                    crtcId = static_cast<uint32_t>(props->prop_values[p]);
                    break;
                }
            }
        }

        if (crtcId == 0 && resources->count_crtcs > 0)
            crtcId = resources->crtcs[0];

        if (crtcId != 0) {
            connectorId = conn->connector_id;
            isyslog("rpi5video/device: Display %ux%u@%uHz (Connector %u, CRTC %u)",
                    activeMode.hdisplay, activeMode.vdisplay, activeMode.vrefresh,
                    connectorId, crtcId);
            return true;
        }
    }

    esyslog("rpi5video/device: kein angeschlossenes Display gefunden");
    return false;
}
