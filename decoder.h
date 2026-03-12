// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file decoder.h
 * @brief Hardware-Decoder für RPi5 (FFmpeg + v4l2_request)
 *
 * Pipeline:
 *   EnqueueData() → Paket-Queue → V4L2-Request Decode
 *                → FFmpeg-Filter (scale/deinterlace) → A/V-Sync → Display
 *
 * Unterschied zu VAAPI:
 *   - hw_device_type = AV_HWDEVICE_TYPE_V4L2REQUEST
 *   - Ausgabeformat: AV_PIX_FMT_DRM_PRIME (direkt, kein separater hwupload)
 *   - Filter-Graph: v4l2_request-Frames direkt in scale_v4l2m2m oder
 *     drm_prime → scale_drmprime → buffersink
 */
#pragma once

#include "common.h"
#include "audio.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <queue>
#include <string>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/packet.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}
#pragma GCC diagnostic pop

#include <vdr/thread.h>

// Vorwärts-Deklaration
class cRpi5Display;

// ============================================================================
// === KONSTANTEN ===
// ============================================================================

/// Tiefe der Video-Paket-Warteschlange (~2 s bei 25 fps)
constexpr size_t DECODER_QUEUE_CAPACITY    = 50;
/// Tiefe der Trick-Mode-Warteschlange (1 Keyframe)
constexpr size_t DECODER_TRICK_QUEUE_DEPTH = 1;
/// Maximale Wartezeit beim Einreichen eines Frames an das Display (ms)
constexpr int    DECODER_SUBMIT_TIMEOUT_MS = 100;
/// Haltezeit pro Trick-Frame (ms Basiswert)
constexpr uint64_t DECODER_TRICK_HOLD_MS   = 40;

/// A/V-Sync: Re-Sync bei Drift > diesem Wert (90-kHz-Ticks, ~500 ms, Replay)
constexpr int64_t DECODER_SYNC_THRESHOLD_REPLAY = 500 * 90;
/// A/V-Sync: Strikterer Schwellwert für Live-TV (~80 ms)
constexpr int64_t DECODER_SYNC_THRESHOLD_LIVE   =  80 * 90;

// ============================================================================
// === FRAME-WRAPPER ===
// ============================================================================

/**
 * @brief Eigentümer eines dekodierten AVFrame.
 *
 * Für v4l2_request ist avFrame->format == AV_PIX_FMT_DRM_PRIME.
 * Der Frame trägt bereits einen AVDRMFrameDescriptor, der direkt
 * an drmModeAddFB2WithModifiers() weitergegeben wird.
 */
struct Rpi5Frame {
    AVFrame *avFrame{nullptr};
    int64_t  pts{AV_NOPTS_VALUE};
    bool     ownsFrame{true};

    Rpi5Frame() = default;
    Rpi5Frame(Rpi5Frame &&) noexcept;
    Rpi5Frame &operator=(Rpi5Frame &&) noexcept;
    ~Rpi5Frame() noexcept;

    Rpi5Frame(const Rpi5Frame &)            = delete;
    Rpi5Frame &operator=(const Rpi5Frame &) = delete;
};

// ============================================================================
// === DECODER ===
// ============================================================================

/**
 * @brief Dekodiert Video-ES-Daten per FFmpeg/v4l2_request und reicht
 *        fertige DRM-PRIME-Frames an cRpi5Display weiter.
 */
class cRpi5Decoder : public cThread {
public:
    explicit cRpi5Decoder(cRpi5Display *displayPtr, Rpi5Context *ctxPtr);
    ~cRpi5Decoder() noexcept override;

    // --- Lebenszyklus ---
    [[nodiscard]] auto Initialize()         -> bool;
    auto Shutdown()                         -> void;
    auto Clear()                            -> void;

    // --- Dateneingabe ---
    auto EnqueueData(const uint8_t *data, size_t size, int64_t pts) -> void;

    // --- Codec ---
    [[nodiscard]] auto OpenCodec(AVCodecID codecId) -> bool;

    // --- Zustand ---
    [[nodiscard]] auto IsReady()              const noexcept -> bool;
    [[nodiscard]] auto IsQueueEmpty()         const          -> bool;
    [[nodiscard]] auto IsQueueFull()          const          -> bool;
    [[nodiscard]] auto GetQueueSize()         const          -> size_t;
    [[nodiscard]] auto GetLastPts()           const noexcept -> int64_t;
    [[nodiscard]] auto GetStreamWidth()       const          -> int;
    [[nodiscard]] auto GetStreamHeight()      const          -> int;
    [[nodiscard]] auto GetStreamAspect()      const          -> double;
    [[nodiscard]] auto IsReadyForNextTrickFrame() const noexcept -> bool;

    // --- Trick-Mode ---
    auto SetTrickSpeed(int speed, bool forward = true, bool fast = false) -> void;

    // --- A/V-Sync ---
    auto SetAudioProcessor(cAudioProcessor *audio) -> void;
    auto DrainQueue() -> void;

private:
    // --- Hilfstypen ---
    struct FreeAVPacket      { auto operator()(AVPacket      *p) const noexcept -> void { av_packet_free(&p);        } };
    struct FreeAVFrame       { auto operator()(AVFrame       *f) const noexcept -> void { av_frame_free(&f);         } };
    struct FreeAVParser    { auto operator()(AVCodecParserContext *p) const noexcept -> void { av_parser_close(p);        } };
    struct FreeAVCodecCtx  { auto operator()(AVCodecContext*c) const noexcept -> void { avcodec_free_context(&c);  } };
    struct FreeFilterGraph   { auto operator()(AVFilterGraph *g) const noexcept -> void { avfilter_graph_free(&g);   } };

    // --- Thread ---
    auto Action() -> void override;

    // --- Interne Methoden ---
    [[nodiscard]] auto DecodePacket(AVPacket *pkt)                            -> bool;
    [[nodiscard]] auto ProcessFilteredFrame(AVFrame *frame)                   -> bool;
    [[nodiscard]] auto InitFilterGraph(AVFrame *firstFrame)                   -> bool;
    auto ResetFilterGraph()                                                   -> void;
    [[nodiscard]] auto SyncAndSubmitFrame(std::unique_ptr<Rpi5Frame> frame)   -> bool;

    // --- Zeiger (nicht besessen) ---
    cRpi5Display   *display{nullptr};
    Rpi5Context    *rpi5Context{nullptr};
    cAudioProcessor*audioProcessor{nullptr};

    // --- Codec-Zustand ---
    mutable cMutex  codecMutex;
    std::unique_ptr<AVCodecContext,     FreeAVCodecCtx>  codecCtx;
    std::unique_ptr<AVCodecParserContext, FreeAVParser>  parserCtx;
    AVCodecID currentCodecId{AV_CODEC_ID_NONE};
    bool      isSoftwareDecode{false};

    // --- Filter ---
    std::unique_ptr<AVFilterGraph, FreeFilterGraph> filterGraph;
    AVFilterContext *bufferSrcCtx{nullptr};
    AVFilterContext *bufferSinkCtx{nullptr};
    int  filterWidth{0}, filterHeight{0};
    int  srcWidth{0},    srcHeight{0};
    bool isInterlaced{false};
    std::string filterChain;

    // --- Frames ---
    std::unique_ptr<AVFrame, FreeAVFrame> decodedFrame;
    std::unique_ptr<AVFrame, FreeAVFrame> filteredFrame;

    // --- Paket-Queue ---
    mutable cMutex         packetMutex;
    cCondVar               packetCondition;
    std::queue<AVPacket *> packetQueue;

    // --- A/V-Sync ---
    std::atomic<int64_t> lastPts{AV_NOPTS_VALUE};
    bool     syncCorrectionDone{false};
    bool     liveMode{false};
    cTimeMs  nextSyncLog;

    // --- Trick-Speed ---
    std::atomic<int>     trickSpeed{0};
    std::atomic<uint64_t>nextTrickFrameDue{0};
    int64_t  prevTrickPts{AV_NOPTS_VALUE};
    bool     isTrickFastForward{false};
    bool     isTrickReverse{false};
    int      trickMultiplier{0};
    uint64_t trickHoldMs{DECODER_TRICK_HOLD_MS};

    // --- Lebenszyklus ---
    std::atomic<bool> ready{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> hasExited{false};
    std::atomic<bool> hasLoggedFirstFrame{false};
};
