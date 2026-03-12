// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file device.h
 * @brief VDR-Device-Integration, PES-Routing und Lebenszyklus
 */
#pragma once

#include "common.h"
#include "audio.h"
#include "decoder.h"
#include "display.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/avutil.h>
}
#pragma GCC diagnostic pop

#include <vdr/device.h>

// ============================================================================
// === HAUPTDEVICE ===
// ============================================================================

/**
 * @brief VDR-Ausgabegerät für den Raspberry Pi 5.
 *
 * Implementiert die VDR-cDevice-Schnittstelle und vermittelt zwischen
 * dem VDR-Framework (PES-Eingabe, Steuerkommandos) und den Plugin-internen
 * Subsystemen (Decoder, Display, Audio).
 */
class cRpi5Device : public cDevice {
public:
    cRpi5Device();
    ~cRpi5Device() noexcept override;

    // --- Initialisierung ---
    [[nodiscard]] auto Initialize(std::string_view drmDevicePath,
                                   std::string_view audioDevicePath) -> bool;
    auto Detach() -> void;
    [[nodiscard]] auto Attach()  -> bool;

    // --- VDR-Device-Schnittstelle ---
    [[nodiscard]] auto CanReplay()   const -> bool override;
    [[nodiscard]] auto HasDecoder()  const -> bool;
    [[nodiscard]] auto HasIBPTrickSpeed()  -> bool override;
    [[nodiscard]] auto DeviceType()  const -> cString override;
    [[nodiscard]] auto Ready()             -> bool override;
    [[nodiscard]] auto GetSTC()            -> int64_t override;
    auto GetOsdSize(int &Width, int &Height, double &PixelAspect) -> void override;
    auto GetVideoSize(int &Width, int &Height, double &VideoAspect) -> void override;
    auto MakePrimaryDevice(bool On) -> void override;

    // --- Wiedergabe ---
    [[nodiscard]] auto SetPlayMode(ePlayMode PlayMode) -> bool override;
    [[nodiscard]] auto PlayVideo(const uchar *Data, int Length) -> int override;
    [[nodiscard]] auto PlayAudio(const uchar *Data, int Length, uchar Id) -> int override;
    [[nodiscard]] auto Poll(cPoller &Poller, int TimeoutMs) -> bool override;
    [[nodiscard]] auto Flush(int TimeoutMs) -> bool override;
    auto Clear()  -> void override;
    auto Play()   -> void override;
    auto Freeze() -> void override;
    auto Mute()   -> void override;
    auto TrickSpeed(int Speed, bool Forward) -> void override;
    auto StillPicture(const uchar *Data, int Length) -> void override;
    auto SetVolumeDevice(int Volume) -> void override;
    auto SetAudioTrackDevice(eTrackType Type) -> void override;

private:
    // --- Hardware-Initialisierung ---
    [[nodiscard]] auto OpenHardware()        -> bool;
    auto ReleaseHardware()                   -> void;
    [[nodiscard]] auto SelectDrmConnector()  -> bool;
    [[nodiscard]] auto ProbeDecoderCaps()    -> bool;
    auto Stop()                              -> void;

    // --- Subsysteme ---
    std::unique_ptr<cAudioProcessor>  audioProcessor;
    std::unique_ptr<cRpi5Display>     display;
    std::unique_ptr<cRpi5Decoder>     decoder;

    // --- Hardware-Kontext ---
    Rpi5Context        rpi5;
    int                drmFd{-1};
    std::string        drmPath;
    std::string        audioDevice;
    uint32_t           crtcId{0};
    uint32_t           connectorId{0};
    drmModeModeInfo    activeMode{};

    // --- OSD-Cache ---
    int  osdWidth{0};
    int  osdHeight{0};

    // --- Initialisierungs-Zustand (0=uninit, 1=in Arbeit, 2=bereit) ---
    std::atomic<int> initState{0};

    // --- Wiedergabe-Zustand ---
    std::atomic<bool>     paused{false};
    std::atomic<int>      trickSpeed{0};
    bool                  liveMode{false};

    // --- Video-Codec-Erkennung ---
    std::atomic<AVCodecID> videoCodecId{AV_CODEC_ID_NONE};
    AVCodecID  previousVideoCodec{AV_CODEC_ID_NONE};
    AVCodecID  videoCodecCandidate{AV_CODEC_ID_NONE};
    int        videoCodecCandidateCount{0};

    // --- Audio-Codec-Erkennung ---
    std::atomic<AVCodecID>    audioCodecId{AV_CODEC_ID_NONE};
    std::atomic<unsigned char>prevAudioStreamId{0xFF};
    AVCodecID  codecHysteresis{AV_CODEC_ID_NONE};
    int        codecHysteresisCount{0};

    // --- DRM Deleter ---
    struct FreeDrmResources { auto operator()(drmModeRes *p) const noexcept -> void { drmModeFreeResources(p); } };
    struct FreeDrmConnector { auto operator()(drmModeConnector *p) const noexcept -> void { drmModeFreeConnector(p); } };
    struct FreeDrmObjectProperties { auto operator()(drmModeObjectProperties *p) const noexcept -> void { drmModeFreeObjectProperties(p); } };
    struct FreeDrmProperty { auto operator()(drmModePropertyRes *p) const noexcept -> void { drmModeFreeProperty(p); } };
};
