// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file audio.h
 * @brief ALSA-Ausgabe mit IEC61937-Passthrough und PCM-Fallback
 *
 * Portiert vom vaapivideo-Plugin; keine VAAPI-Abhängigkeiten,
 * läuft unverändert auf dem RPi5.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <queue>
#include <span>
#include <string>
#include <string_view>

#include <alsa/asoundlib.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#pragma GCC diagnostic pop

#include <vdr/thread.h>
#include <vdr/tools.h>

// ============================================================================
// === HILFSTYPEN ===
// ============================================================================

struct FreeAVPacket {
    auto operator()(AVPacket *p) const noexcept -> void { av_packet_free(&p); }
};
struct FreeAVFrame {
    auto operator()(AVFrame *f) const noexcept -> void { av_frame_free(&f); }
};
struct FreeAVCodecContext {
    auto operator()(AVCodecContext *c) const noexcept -> void { avcodec_free_context(&c); }
};
struct FreeAVParser {
    auto operator()(AVCodecParserContext *p) const noexcept -> void { av_parser_close(p); }
};

// ============================================================================
// === STREAM-PARAMETER ===
// ============================================================================

struct AudioStreamParams {
    AVCodecID      codecId{AV_CODEC_ID_NONE};
    int            sampleRate{48000};
    int            channels{2};
    const uint8_t *extradata{nullptr};
    int            extradataSize{0};
};

// ============================================================================
// === AUDIO PROCESSOR ===
// ============================================================================

class cAudioProcessor : public cThread {
public:
    cAudioProcessor();
    ~cAudioProcessor() noexcept override;

    [[nodiscard]] auto Initialize(std::string_view alsaDevice) -> bool;
    auto Stop()     -> void;
    auto Clear()    -> void;
    [[nodiscard]] auto IsInitialized() const noexcept -> bool;
    [[nodiscard]] auto IsQueueFull()   const          -> bool;

    auto Decode(const uint8_t *data, size_t size, int64_t pts) -> void;
    [[nodiscard]] auto OpenCodec(AVCodecID codecId, int sampleRate, int channels) -> bool;
    [[nodiscard]] auto GetClock() const noexcept -> int64_t;
    auto SetVolume(int vol) noexcept -> void;

private:
    auto Action() -> void override;

    [[nodiscard]] auto OpenAlsaDevice()  -> bool;
    [[nodiscard]] auto ConfigureAlsaParams(snd_pcm_t *handle, snd_pcm_format_t format,
                                            unsigned channels, unsigned rate,
                                            bool allowResample) -> bool;
    [[nodiscard]] auto EnqueuePacket(const AVPacket *rawPacket) -> bool;
    [[nodiscard]] auto DecodeToPcm(std::span<const uint8_t> data, int64_t pts) -> bool;
    [[nodiscard]] auto WritePcmToAlsa(std::span<const uint8_t> data,
                                      int64_t startPts90k, unsigned frames) -> bool;
    [[nodiscard]] auto WriteToAlsa(std::span<const uint8_t> data) -> bool;
    [[nodiscard]] auto CanPassthrough(AVCodecID codecId) const -> bool;
    [[nodiscard]] auto ComputeAlsaRate(AVCodecID codecId, unsigned streamRate,
                                        bool passthrough) const -> unsigned;
    auto SetStreamParams(const AudioStreamParams &params) -> void;
    auto OpenDecoder()  -> void;
    auto CloseDecoder() -> void;
    auto ProbeSinkCaps()-> void;
    auto Shutdown()     -> void;

    // ALSA
    snd_pcm_t      *alsaHandle{nullptr};
    std::string     alsaDeviceName;
    unsigned        alsaSampleRate{0};
    unsigned        alsaChannels{0};
    size_t          alsaFrameBytes{0};
    bool            alsaPassthroughActive{false};
    cTimeMs         lastReopenAttempt;

    // Codec
    std::unique_ptr<AVCodecContext,       FreeAVCodecContext> decoder;
    std::unique_ptr<AVCodecParserContext, FreeAVParser>       parserCtx;
    SwrContext     *swrCtx{nullptr};
    AVSampleFormat  swrFormat{AV_SAMPLE_FMT_NONE};
    int             swrChannels{0};
    AudioStreamParams streamParams;

    // Warteschlange
    std::unique_ptr<cMutex> mutex;
    cCondVar                packetCondition;
    std::queue<AVPacket *>  packetQueue;

    // Uhren
    std::atomic<int64_t>  playbackPts{AV_NOPTS_VALUE};
    std::atomic<uint32_t> clearGeneration{0};
    int64_t               pcmQueueEndPts{AV_NOPTS_VALUE};
    int64_t               pcmNextPts{AV_NOPTS_VALUE};

    // Zustands-Flags
    std::atomic<bool> initialized{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> hasExited{false};
    std::atomic<bool> needsFlush{false};
    std::atomic<int>  volume{255};
    std::atomic<int>  alsaErrorCount{0};

    // Decoder-Fehlerbehandlung
    std::atomic<int> decoderRefCount{0};
    std::atomic<int> decoderGracePackets{0};
    int              consecutiveDecodeErrors{0};
    cTimeMs          lastDecodeErrorLog;
    cTimeMs          lastQueueWarn;

    // Sink-Fähigkeiten
    struct HdmiSinkCaps {
        bool ac3{false}, eac3{false}, truehd{false};
        bool dts{false}, dtshd{false}, ac4{false}, mpegh3d{false};
    } sinkCaps;
    bool        sinkCapsCached{false};
    std::string sinkCapsDevice;
};
