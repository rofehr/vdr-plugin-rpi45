// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file audio.h
 * @brief ALSA-Ausgabe mit IEC61937-Passthrough und PCM-Fallback
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
#include <libswresample/swresample.h>
}
#pragma GCC diagnostic pop

#include <vdr/thread.h>

// ============================================================================
// === HILFSTYPEN ===
// ============================================================================

struct FreeAVPacket {
    auto operator()(AVPacket *p) const noexcept -> void { av_packet_free(&p); }
};
struct FreeAVFrame {
    auto operator()(AVFrame *f) const noexcept -> void { av_frame_free(&f); }
};
struct FreeSwrContext {
    auto operator()(SwrContext *s) const noexcept -> void { swr_free(&s); }
};
struct FreeAVCodecContext {
    auto operator()(AVCodecContext *c) const noexcept -> void { avcodec_free_context(&c); }
};

// ============================================================================
// === AUDIO PROCESSOR ===
// ============================================================================

/**
 * @brief Dekodiert Audio-ES-Daten und gibt sie über ALSA aus.
 *
 * Läuft in einem eigenen Thread (Action()). Unterstützt:
 *  - PCM-Ausgabe (S16LE, Stereo, Resample via libswresample)
 *  - IEC61937-Passthrough für AC-3, E-AC-3, DTS
 *  - Software-Lautstärkeregelung (PCM-Modus)
 *  - Dreistufige ALSA-Fehlerbehandlung
 */
class cAudioProcessor : public cThread {
public:
    cAudioProcessor();
    ~cAudioProcessor() noexcept override;

    // --- Lebenszyklus ---
    [[nodiscard]] auto Initialize(std::string_view alsaDevice) -> bool;
    auto Stop()     -> void;
    auto Clear()    -> void; ///< Puffer sofort leeren, ALSA zurücksetzen
    [[nodiscard]] auto IsInitialized() const noexcept -> bool;
    [[nodiscard]] auto IsQueueFull()   const          -> bool;

    // --- Dateneingabe ---
    auto Decode(const uint8_t *data, size_t size, int64_t pts) -> void;

    // --- A/V-Sync ---
    /**
     * @brief Liefert den aktuellen Audio-Wiedergabe-Uhr-Wert (90 kHz).
     *
     * clock = pcmQueueEndPts - ALSA-Puffer-Delay
     * Gibt AV_NOPTS_VALUE zurück wenn keine Uhr verfügbar ist.
     */
    [[nodiscard]] auto GetClock() const noexcept -> int64_t;

    // --- Lautstärke ---
    auto SetVolume(int vol) noexcept -> void;

    // --- Codec-Parameter ---
    struct StreamParams {
        AVCodecID codecId{AV_CODEC_ID_NONE};
        int sampleRate{48000};
        int channels{2};
    };
    auto OpenCodec(AVCodecID codecId, int sampleRate, int channels) -> bool;

private:
    // --- Thread ---
    auto Action() -> void override;

    // --- Interne Methoden ---
    [[nodiscard]] auto OpenAlsaDevice()  -> bool;
    [[nodiscard]] auto EnqueuePacket(AVPacket *pkt) -> bool;
    [[nodiscard]] auto DecodeToPcm()     -> bool;
    [[nodiscard]] auto WritePcmToAlsa(std::span<const uint8_t> data,
                                      int64_t startPts90k, unsigned frames) -> bool;
    [[nodiscard]] auto WriteToAlsa(std::span<const uint8_t> data) -> bool;
    auto Shutdown() -> void;
    auto ProbeSinkCaps() -> void;
    auto SetupPassthrough(AVCodecID codecId) -> bool;

    // --- ALSA ---
    snd_pcm_t      *alsaHandle{nullptr};
    std::string     alsaDeviceName;
    unsigned        alsaSampleRate{0};
    size_t          alsaFrameBytes{0};
    bool            alsaPassthroughActive{false};
    cTimeMs         lastReopenAttempt;

    // --- Codec ---
    std::unique_ptr<AVCodecContext, FreeAVCodecContext> decoder;
    std::unique_ptr<AVCodecParserContext,
        decltype([](AVCodecParserContext *p){ av_parser_close(p); })> parserCtx{nullptr,
        [](AVCodecParserContext *p){ av_parser_close(p); }};
    std::unique_ptr<SwrContext, FreeSwrContext> swrCtx;
    std::unique_ptr<AVFrame, FreeAVFrame> decodedFrame;
    std::unique_ptr<AVFrame, FreeAVFrame> resampledFrame;
    StreamParams streamParams;

    // --- Warteschlange ---
    mutable std::unique_ptr<cMutex> mutex;
    cCondVar                         queueCondition;
    std::queue<AVPacket *>           packetQueue;

    // --- Uhren (atomisch für lock-freies Lesen durch Decoder-Thread) ---
    std::atomic<int64_t>  playbackPts{AV_NOPTS_VALUE};
    std::atomic<uint32_t> clearGeneration{0};
    int64_t               pcmQueueEndPts{AV_NOPTS_VALUE}; ///< Unter Mutex geschützt
    int64_t               pcmNextPts{AV_NOPTS_VALUE};

    // --- Zustands-Flags ---
    std::atomic<bool> initialized{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> hasExited{false};
    std::atomic<bool> needsFlush{false};
    std::atomic<int>  volume{255};
    std::atomic<int>  alsaErrorCount{0};

    // --- Sink-Fähigkeiten ---
    struct HdmiSinkCaps {
        bool ac3{false}, eac3{false}, truehd{false};
        bool dts{false}, dtshd{false}, ac4{false}, mpegh3d{false};
    } sinkCaps;

    int decoderGracePackets{0};
    int decoderErrorCount{0};
    cTimeMs lastErrorLog;
};
