// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file decoder.cpp
 * @brief Hardware-Decoder für RPi5 (FFmpeg + v4l2_request)
 *
 * Kernunterschiede zu VAAPI:
 *  1. hw_device_type = AV_HWDEVICE_TYPE_V4L2REQUEST
 *  2. Ausgabeformat nach Decode: AV_PIX_FMT_DRM_PRIME
 *     (v4l2_request exportiert DRM-PRIME-FDs direkt, kein hwmap nötig)
 *  3. Filter-Graph: drmprime_scale (scale_drmprime) für DRM-PRIME-Frames
 *     oder Software-Fallback (scale) nach hwdownload
 *  4. Kein hwupload erforderlich (Hardware übernimmt das direkt)
 */

#include "decoder.h"
#include "audio.h"
#include "common.h"
#include "config.h"
#include "display.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <utility>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/packet.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}
#pragma GCC diagnostic pop

#include <vdr/thread.h>
#include <vdr/tools.h>

// ============================================================================
// === RAII-FRAME ===
// ============================================================================

Rpi5Frame::Rpi5Frame(Rpi5Frame &&other) noexcept
    : avFrame(other.avFrame), pts(other.pts), ownsFrame(other.ownsFrame) {
    other.avFrame   = nullptr;
    other.ownsFrame = false;
}

Rpi5Frame::~Rpi5Frame() noexcept {
    if (avFrame && ownsFrame) av_frame_free(&avFrame);
}

auto Rpi5Frame::operator=(Rpi5Frame &&other) noexcept -> Rpi5Frame & {
    if (this != &other) {
        if (avFrame && ownsFrame) av_frame_free(&avFrame);
        avFrame         = other.avFrame;
        pts             = other.pts;
        ownsFrame       = other.ownsFrame;
        other.avFrame   = nullptr;
        other.ownsFrame = false;
    }
    return *this;
}

// ============================================================================
// === KONSTRUKTOR / DESTRUKTOR ===
// ============================================================================

cRpi5Decoder::cRpi5Decoder(cRpi5Display *displayPtr, Rpi5Context *ctxPtr)
    : cThread("rpi5video/decoder"), display(displayPtr), rpi5Context(ctxPtr) {}

cRpi5Decoder::~cRpi5Decoder() noexcept {
    Shutdown();
    DrainQueue();
}

// ============================================================================
// === ÖFFENTLICHE API ===
// ============================================================================

[[nodiscard]] auto cRpi5Decoder::Initialize() -> bool {
    if (!display || !rpi5Context || !rpi5Context->hwDeviceRef) [[unlikely]] {
        esyslog("rpi5video/decoder: fehlendes Display oder V4L2-Request-Kontext");
        return false;
    }

    decodedFrame.reset(av_frame_alloc());
    filteredFrame.reset(av_frame_alloc());
    if (!decodedFrame || !filteredFrame) [[unlikely]] {
        esyslog("rpi5video/decoder: Frame-Allokation fehlgeschlagen");
        return false;
    }

    ready.store(true, std::memory_order_release);
    Start();
    isyslog("rpi5video/decoder: initialisiert (Queue-Tiefe=%zu)", DECODER_QUEUE_CAPACITY);
    return true;
}

auto cRpi5Decoder::Shutdown() -> void {
    const bool wasStopping = stopping.exchange(true, std::memory_order_acq_rel);
    if (wasStopping) return;

    { const cMutexLock lock(&packetMutex); packetCondition.Broadcast(); }

    if (!hasExited.load(std::memory_order_acquire)) Cancel(3);

    const cTimeMs timeout(SHUTDOWN_TIMEOUT_MS);
    while (!hasExited.load(std::memory_order_acquire) && !timeout.TimedOut()) {
        { const cMutexLock lock(&packetMutex); packetCondition.Broadcast(); }
        cCondWait::SleepMs(10);
    }
}

auto cRpi5Decoder::Clear() -> void {
    const cMutexLock decodeLock(&codecMutex);
    DrainQueue();
    if (codecCtx) avcodec_flush_buffers(codecCtx.get());
    ResetFilterGraph();
    if (currentCodecId != AV_CODEC_ID_NONE)
        parserCtx.reset(av_parser_init(currentCodecId));
    else
        parserCtx.reset();
    if (decodedFrame)  av_frame_unref(decodedFrame.get());
    if (filteredFrame) av_frame_unref(filteredFrame.get());
    lastPts.store(AV_NOPTS_VALUE, std::memory_order_relaxed);
    syncCorrectionDone = false;
    nextSyncLog.Set(0);
}

auto cRpi5Decoder::DrainQueue() -> void {
    const cMutexLock lock(&packetMutex);
    while (!packetQueue.empty()) {
        const std::unique_ptr<AVPacket, FreeAVPacket> dropped{packetQueue.front()};
        packetQueue.pop();
    }
    packetCondition.Broadcast();
}

auto cRpi5Decoder::EnqueueData(const uint8_t *data, size_t size, int64_t pts) -> void {
    if (!data || size == 0 || stopping.load(std::memory_order_relaxed)) return;

    const cMutexLock decodeLock(&codecMutex);
    if (!codecCtx || !parserCtx) return;

    const uint8_t *parseData = data;
    int parseSize = static_cast<int>(size);
    int64_t currentPts = pts;

    while (parseSize > 0) {
        uint8_t *parsedData = nullptr;
        int parsedSize = 0;

        const int parsed = av_parser_parse2(
            parserCtx.get(), codecCtx.get(),
            &parsedData, &parsedSize,
            parseData, parseSize,
            currentPts, AV_NOPTS_VALUE, 0);

        if (parsed < 0) [[unlikely]] break;
        if (parsed == 0 && parsedSize == 0) break;

        parseData   += parsed;
        parseSize   -= parsed;
        currentPts   = AV_NOPTS_VALUE;

        if (parsedSize > 0) {
            // Trick-Mode: nur Keyframes
            if (trickSpeed.load(std::memory_order_acquire) != 0 &&
                (isTrickFastForward || isTrickReverse) &&
                parserCtx->key_frame == 0)
                continue;

            AVPacket *pkt = av_packet_alloc();
            if (!pkt) [[unlikely]] break;
            if (av_new_packet(pkt, parsedSize) < 0) [[unlikely]] {
                av_packet_free(&pkt);
                continue;
            }
            std::memcpy(pkt->data, parsedData, static_cast<size_t>(parsedSize));
            pkt->pts = parserCtx->pts;
            pkt->dts = parserCtx->dts;

            const cMutexLock lock(&packetMutex);
            const bool isTrickMode = trickSpeed.load(std::memory_order_relaxed) != 0;
            const size_t maxDepth  = isTrickMode ? DECODER_TRICK_QUEUE_DEPTH
                                                 : DECODER_QUEUE_CAPACITY;
            if (packetQueue.size() >= maxDepth) {
                if (isTrickMode) { av_packet_free(&pkt); continue; }
                av_packet_free(&packetQueue.front());
                packetQueue.pop();
            }
            packetQueue.push(pkt);
            packetCondition.Broadcast();
        }
    }
}

[[nodiscard]] auto cRpi5Decoder::OpenCodec(AVCodecID codecId) -> bool {
    if (!ready.load(std::memory_order_acquire)) [[unlikely]] return false;
    if (codecCtx && currentCodecId == codecId) return true;

    parserCtx.reset();
    codecCtx.reset();
    ResetFilterGraph();
    currentCodecId = AV_CODEC_ID_NONE;
    hasLoggedFirstFrame.store(false, std::memory_order_relaxed);

    const AVCodec *dec = avcodec_find_decoder(codecId);
    if (!dec) [[unlikely]] {
        esyslog("rpi5video/decoder: Codec %d nicht gefunden", static_cast<int>(codecId));
        return false;
    }

    // Prüfe ob Hardware-Decode für diesen Codec konfiguriert ist
    bool useHw = false;
    switch (codecId) {
        case AV_CODEC_ID_H264:       useHw = rpi5Context->hwH264;  break;
        case AV_CODEC_ID_HEVC:       useHw = rpi5Context->hwHevc;  break;
        case AV_CODEC_ID_MPEG2VIDEO: useHw = rpi5Context->hwMpeg2; break;
        default: break;
    }

    // Verifiziere FFmpeg-HW-Konfiguration für v4l2_request
    if (useHw) {
        bool hasHwCfg = false;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(dec, i);
            if (!cfg) break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                cfg->device_type == AV_HWDEVICE_TYPE_V4L2REQUEST) {
                hasHwCfg = true; break;
            }
        }
        useHw = hasHwCfg;
    }
    isSoftwareDecode = !useHw;

    parserCtx.reset(av_parser_init(codecId));
    if (!parserCtx) [[unlikely]] {
        esyslog("rpi5video/decoder: Parser für %s fehlgeschlagen", dec->name);
        return false;
    }

    std::unique_ptr<AVCodecContext, FreeAVCodecCtx> ctx{avcodec_alloc_context3(dec)};
    if (!ctx) [[unlikely]] {
        esyslog("rpi5video/decoder: Kontext-Allokation für %s fehlgeschlagen", dec->name);
        return false;
    }

    if (useHw) {
        // V4L2-Request: Einzel-Thread, GPU erledigt Parallelisierung
        ctx->thread_count  = 1;
        ctx->hw_device_ctx = av_buffer_ref(rpi5Context->hwDeviceRef);
        if (!ctx->hw_device_ctx) [[unlikely]] {
            esyslog("rpi5video/decoder: av_buffer_ref fehlgeschlagen");
            return false;
        }
        // get_format: V4L2-Request-Ausgabe ist AV_PIX_FMT_DRM_PRIME
        ctx->get_format = [](AVCodecContext *, const AVPixelFormat *fmts) -> AVPixelFormat {
            for (const AVPixelFormat *f = fmts; *f != AV_PIX_FMT_NONE; ++f) {
                if (*f == AV_PIX_FMT_DRM_PRIME) return AV_PIX_FMT_DRM_PRIME;
            }
            // Fallback: Software-Format
            return fmts[0];
        };
    }

    if (const int ret = avcodec_open2(ctx.get(), dec, nullptr); ret < 0) [[unlikely]] {
        esyslog("rpi5video/decoder: %s öffnen fehlgeschlagen: %s", dec->name, AvErr(ret).data());
        return false;
    }

    codecCtx       = std::move(ctx);
    currentCodecId = codecId;
    isyslog("rpi5video/decoder: %s geöffnet (%s)", dec->name, useHw ? "Hardware" : "Software");
    return true;
}

// --- Getter ---
[[nodiscard]] auto cRpi5Decoder::GetLastPts()    const noexcept -> int64_t { return lastPts.load(std::memory_order_acquire); }
[[nodiscard]] auto cRpi5Decoder::IsReady()       const noexcept -> bool    { return ready.load(std::memory_order_acquire); }
[[nodiscard]] auto cRpi5Decoder::IsQueueEmpty()  const          -> bool    { const cMutexLock l(&packetMutex); return packetQueue.empty(); }
[[nodiscard]] auto cRpi5Decoder::IsQueueFull()   const          -> bool    { const cMutexLock l(&packetMutex); return packetQueue.size() >= DECODER_QUEUE_CAPACITY; }
[[nodiscard]] auto cRpi5Decoder::GetQueueSize()  const          -> size_t  { const cMutexLock l(&packetMutex); return packetQueue.size(); }

[[nodiscard]] auto cRpi5Decoder::GetStreamWidth() const -> int {
    const cMutexLock l(&codecMutex); return codecCtx ? codecCtx->width : 0;
}
[[nodiscard]] auto cRpi5Decoder::GetStreamHeight() const -> int {
    const cMutexLock l(&codecMutex); return codecCtx ? codecCtx->height : 0;
}
[[nodiscard]] auto cRpi5Decoder::GetStreamAspect() const -> double {
    const cMutexLock l(&codecMutex);
    if (!codecCtx || codecCtx->width == 0 || codecCtx->height == 0) return 0.0;
    const int sarNum = codecCtx->sample_aspect_ratio.num > 0 ? codecCtx->sample_aspect_ratio.num : 1;
    const int sarDen = codecCtx->sample_aspect_ratio.den > 0 ? codecCtx->sample_aspect_ratio.den : 1;
    return (static_cast<double>(codecCtx->width) * sarNum) / (static_cast<double>(codecCtx->height) * sarDen);
}
[[nodiscard]] auto cRpi5Decoder::IsReadyForNextTrickFrame() const noexcept -> bool {
    if (trickSpeed.load(std::memory_order_relaxed) == 0) return true;
    return cTimeMs::Now() >= nextTrickFrameDue.load(std::memory_order_acquire);
}

auto cRpi5Decoder::SetAudioProcessor(cAudioProcessor *audio) -> void { audioProcessor = audio; }

auto cRpi5Decoder::SetTrickSpeed(int speed, bool forward, bool fast) -> void {
    if (fast && speed > 0) {
        const cMutexLock dl(&codecMutex);
        DrainQueue();
        if (codecCtx) avcodec_flush_buffers(codecCtx.get());
        if (currentCodecId != AV_CODEC_ID_NONE) parserCtx.reset(av_parser_init(currentCodecId));
        ResetFilterGraph();
        if (decodedFrame)  av_frame_unref(decodedFrame.get());
        if (filteredFrame) av_frame_unref(filteredFrame.get());
    }

    isTrickFastForward = forward && fast;
    isTrickReverse     = !forward;
    prevTrickPts       = AV_NOPTS_VALUE;

    if (fast && speed > 0) {
        trickMultiplier = (speed >= 6) ? 2 : (speed >= 3) ? 4 : 8;
        trickHoldMs     = DECODER_TRICK_HOLD_MS;
    } else {
        trickMultiplier = 0;
        trickHoldMs     = static_cast<uint64_t>(speed) * DECODER_TRICK_HOLD_MS;
    }
    nextTrickFrameDue.store(cTimeMs::Now(), std::memory_order_release);

    if (speed == 0) {
        lastPts.store(AV_NOPTS_VALUE, std::memory_order_relaxed);
        syncCorrectionDone = false;
    }
    trickSpeed.store(speed, std::memory_order_release);
}

// ============================================================================
// === THREAD ===
// ============================================================================

auto cRpi5Decoder::Action() -> void {
    isyslog("rpi5video/decoder: Thread gestartet");

    AVPacket *workPacket = av_packet_alloc();
    if (!workPacket) [[unlikely]] {
        esyslog("rpi5video/decoder: Paket-Allokation fehlgeschlagen");
        hasExited.store(true, std::memory_order_release);
        return;
    }

    while (!stopping.load(std::memory_order_acquire)) {
        std::unique_ptr<AVPacket, FreeAVPacket> queuedPacket;

        {
            const cMutexLock lock(&packetMutex);
            if (packetQueue.empty()) {
                packetCondition.TimedWait(packetMutex, 10);
                if (packetQueue.empty()) continue;
            }
            queuedPacket.reset(packetQueue.front());
            packetQueue.pop();
        }

        const cMutexLock decodeLock(&codecMutex);
        if (!codecCtx) continue;

        av_packet_unref(workPacket);
        av_packet_move_ref(workPacket, queuedPacket.get());

        if (!DecodePacket(workPacket)) {
            dsyslog("rpi5video/decoder: Dekodierung fehlgeschlagen");
        }
    }

    av_packet_free(&workPacket);
    hasExited.store(true, std::memory_order_release);
    isyslog("rpi5video/decoder: Thread beendet");
}

// ============================================================================
// === DEKODIERUNG ===
// ============================================================================

[[nodiscard]] auto cRpi5Decoder::DecodePacket(AVPacket *pkt) -> bool {
    int ret = avcodec_send_packet(codecCtx.get(), pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) [[unlikely]] {
        dsyslog("rpi5video/decoder: avcodec_send_packet: %s", AvErr(ret).data());
        return false;
    }

    while (!stopping.load(std::memory_order_relaxed)) {
        av_frame_unref(decodedFrame.get());
        ret = avcodec_receive_frame(codecCtx.get(), decodedFrame.get());

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) [[unlikely]] {
            dsyslog("rpi5video/decoder: avcodec_receive_frame: %s", AvErr(ret).data());
            break;
        }

        if (!hasLoggedFirstFrame.exchange(true, std::memory_order_relaxed)) {
            isyslog("rpi5video/decoder: erster Frame %dx%d fmt=%s",
                    decodedFrame->width, decodedFrame->height,
                    av_get_pix_fmt_name(static_cast<AVPixelFormat>(decodedFrame->format)));
        }

        if (!ProcessFilteredFrame(decodedFrame.get())) return false;
    }
    return true;
}

[[nodiscard]] auto cRpi5Decoder::ProcessFilteredFrame(AVFrame *frame) -> bool {
    // Filter-Graph bei Bedarf initialisieren oder neu aufbauen
    if (!filterGraph) {
        if (!InitFilterGraph(frame)) {
            // Fallback: Frame direkt weiterreichen ohne Filter
            auto f = std::make_unique<Rpi5Frame>();
            f->avFrame = av_frame_clone(frame);
            if (!f->avFrame) return false;
            f->pts = frame->pts;
            return SyncAndSubmitFrame(std::move(f));
        }
    }

    int ret = av_buffersrc_add_frame_flags(
        bufferSrcCtx, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) [[unlikely]] {
        dsyslog("rpi5video/decoder: buffersrc fehlgeschlagen: %s", AvErr(ret).data());
        return false;
    }

    while (!stopping.load(std::memory_order_relaxed)) {
        av_frame_unref(filteredFrame.get());
        ret = av_buffersink_get_frame(bufferSinkCtx, filteredFrame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) [[unlikely]] { break; }

        auto f = std::make_unique<Rpi5Frame>();
        f->avFrame = av_frame_clone(filteredFrame.get());
        if (!f->avFrame) return false;
        f->pts = filteredFrame->pts;
        if (!SyncAndSubmitFrame(std::move(f))) return false;
    }
    return true;
}

// ============================================================================
// === FILTER-GRAPH ===
// ============================================================================

[[nodiscard]] auto cRpi5Decoder::InitFilterGraph(AVFrame *firstFrame) -> bool {
    ResetFilterGraph();

    srcWidth    = firstFrame->width;
    srcHeight   = firstFrame->height;
    isInterlaced= (firstFrame->flags & AV_FRAME_FLAG_INTERLACED) != 0;

    // Ausgabeauflösung: auf Displaygröße skalieren (letterbox)
    const auto dispW = static_cast<int>(rpi5Config.display.GetWidth());
    const auto dispH = static_cast<int>(rpi5Config.display.GetHeight());

    const double srcAR = (srcHeight > 0) ? static_cast<double>(srcWidth) / srcHeight : 1.0;
    if (static_cast<double>(dispW) / dispH > srcAR) {
        filterHeight = dispH;
        filterWidth  = static_cast<int>(dispH * srcAR) & ~1;
    } else {
        filterWidth  = dispW;
        filterHeight = static_cast<int>(dispW / srcAR) & ~1;
    }
    if (filterWidth  <= 0) filterWidth  = dispW;
    if (filterHeight <= 0) filterHeight = dispH;

    // Filter-Kette aufbauen
    // Für v4l2_request/DRM-PRIME nutzen wir scale (Software-Fallback),
    // da scale_drmprime hardwareabhängig ist.
    // Bei künftiger Kernel-Unterstützung: scale_drmprime ersetzen.
    filterChain.clear();

    if (isInterlaced && rpi5Config.deinterlace) {
        // bwdif läuft auf CPU; yadif wäre schneller aber schlechter
        filterChain += "hwdownload,format=nv12,bwdif=mode=send_field,";
    } else if (firstFrame->format == AV_PIX_FMT_DRM_PRIME) {
        filterChain += "hwdownload,format=nv12,";
    }

    filterChain += std::format("scale={}:{}", filterWidth, filterHeight);
    filterChain += ":flags=lanczos";

    if (rpi5Config.denoise && rpi5Context->hasDenoise) {
        filterChain += ",hqdn3d=4:3:6:4.5";
    }

    std::unique_ptr<AVFilterGraph, FreeFilterGraph> graph{avfilter_graph_alloc()};
    if (!graph) [[unlikely]] return false;

    // Puffer-Quelle
    std::string bufferSrcArgs = std::format(
        "video_size={}x{}:pix_fmt={}:time_base=1/90000:pixel_aspect=1/1",
        srcWidth, srcHeight,
        static_cast<int>(firstFrame->format));

    int ret = avfilter_graph_create_filter(
        &bufferSrcCtx,
        avfilter_get_by_name("buffer"), "in",
        bufferSrcArgs.c_str(), nullptr, graph.get());
    if (ret < 0) [[unlikely]] {
        esyslog("rpi5video/decoder: buffersrc erstellen fehlgeschlagen: %s", AvErr(ret).data());
        return false;
    }

    // HW-Device-Kontext für hwdownload verfügbar machen
    bufferSrcCtx->hw_device_ctx = av_buffer_ref(rpi5Context->hwDeviceRef);

    ret = avfilter_init_str(bufferSrcCtx, bufferSrcArgs.c_str());
    if (ret < 0) [[unlikely]] {
        esyslog("rpi5video/decoder: buffersrc init fehlgeschlagen: %s", AvErr(ret).data());
        ResetFilterGraph();
        return false;
    }

    // Puffer-Senke
    ret = avfilter_graph_create_filter(
        &bufferSinkCtx,
        avfilter_get_by_name("buffersink"), "out",
        nullptr, nullptr, graph.get());
    if (ret < 0) [[unlikely]] {
        esyslog("rpi5video/decoder: buffersink erstellen fehlgeschlagen: %s", AvErr(ret).data());
        ResetFilterGraph();
        return false;
    }

    // Filter-Kette verbinden
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterInOut *outputs = avfilter_inout_alloc();
    if (!inputs || !outputs) [[unlikely]] {
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        ResetFilterGraph();
        return false;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = bufferSrcCtx;
    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = bufferSinkCtx;

    ret = avfilter_graph_parse_ptr(graph.get(), filterChain.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    if (ret < 0) [[unlikely]] {
        esyslog("rpi5video/decoder: Filter-Kette '%s' parsen fehlgeschlagen: %s",
                filterChain.c_str(), AvErr(ret).data());
        ResetFilterGraph();
        return false;
    }

    // HW-Device auf alle Filter setzen (für hwdownload)
    for (unsigned i = 0; i < graph->nb_filters; ++i) {
        if (!graph->filters[i]->hw_device_ctx) {
            graph->filters[i]->hw_device_ctx = av_buffer_ref(rpi5Context->hwDeviceRef);
        }
    }

    ret = avfilter_graph_config(graph.get(), nullptr);
    if (ret < 0) [[unlikely]] {
        esyslog("rpi5video/decoder: Filter-Graph konfigurieren fehlgeschlagen: %s", AvErr(ret).data());
        ResetFilterGraph();
        return false;
    }

    filterGraph = std::move(graph);
    isyslog("rpi5video/decoder: Filter initialisiert (%dx%d → %dx%d%s)",
            srcWidth, srcHeight, filterWidth, filterHeight,
            isInterlaced ? ", deinterlaced" : "");
    dsyslog("rpi5video/decoder: Filter-Kette='%s'", filterChain.c_str());
    return true;
}

auto cRpi5Decoder::ResetFilterGraph() -> void {
    bufferSrcCtx  = nullptr;
    bufferSinkCtx = nullptr;
    filterGraph.reset();
}

// ============================================================================
// === A/V-SYNC ===
// ============================================================================

[[nodiscard]] auto cRpi5Decoder::SyncAndSubmitFrame(std::unique_ptr<Rpi5Frame> frame) -> bool {
    if (!frame || !display) [[unlikely]] return false;

    const int64_t originalPts = frame->pts;

    // --- Trick-Mode: Pacing per Timer ---
    if (trickSpeed.load(std::memory_order_acquire) != 0) {
        const bool     newSource     = (originalPts != prevTrickPts);
        const int64_t  savedPrevPts  = prevTrickPts;

        if (isTrickReverse && newSource &&
            originalPts != AV_NOPTS_VALUE && savedPrevPts != AV_NOPTS_VALUE &&
            originalPts > savedPrevPts)
            return true;

        if (newSource) prevTrickPts = originalPts;
        if (newSource && originalPts != AV_NOPTS_VALUE)
            lastPts.store(originalPts, std::memory_order_release);

        if (newSource && trickMultiplier > 0) {
            const uint64_t due = nextTrickFrameDue.load(std::memory_order_acquire);
            while (cTimeMs::Now() < due &&
                   !stopping.load(std::memory_order_relaxed) &&
                   trickSpeed.load(std::memory_order_relaxed) != 0)
                cCondWait::SleepMs(10);
        }
        if (newSource) {
            if (trickMultiplier > 0 && originalPts != AV_NOPTS_VALUE && savedPrevPts != AV_NOPTS_VALUE) {
                const auto ptsDelta = static_cast<uint64_t>(std::abs(originalPts - savedPrevPts));
                const uint64_t holdMs = std::clamp(
                    ptsDelta / (static_cast<uint64_t>(90) * trickMultiplier),
                    uint64_t{10}, uint64_t{2000});
                nextTrickFrameDue.store(cTimeMs::Now() + holdMs, std::memory_order_release);
            } else {
                nextTrickFrameDue.store(cTimeMs::Now() + trickHoldMs, std::memory_order_release);
            }
        }
        return display->SubmitFrame(std::move(frame), DECODER_SUBMIT_TIMEOUT_MS);
    }

    if (originalPts != AV_NOPTS_VALUE)
        lastPts.store(originalPts, std::memory_order_release);

    // --- Freerun: kein Audio oder kein PTS ---
    if (!audioProcessor || originalPts == AV_NOPTS_VALUE)
        return display->SubmitFrame(std::move(frame), DECODER_SUBMIT_TIMEOUT_MS);

    const int64_t latency = static_cast<int64_t>(rpi5Config.audioLatency) * 90;
    int64_t clock = audioProcessor->GetClock();

    // Erster Frame: auf Audio-Uhr warten (max. 1 s Live / 500 ms Replay)
    if (clock == AV_NOPTS_VALUE && !syncCorrectionDone) {
        const int waitMs = liveMode ? 1000 : 500;
        const cTimeMs waitTimeout(waitMs);
        while (!stopping.load(std::memory_order_relaxed) &&
               trickSpeed.load(std::memory_order_relaxed) == 0 &&
               !waitTimeout.TimedOut()) {
            cCondWait::SleepMs(10);
            clock = audioProcessor->GetClock();
            if (clock != AV_NOPTS_VALUE) break;
        }
    }
    if (clock == AV_NOPTS_VALUE)
        return display->SubmitFrame(std::move(frame), DECODER_SUBMIT_TIMEOUT_MS);

    int64_t delta = originalPts - clock - latency;

    // Sync-Logging
    if (nextSyncLog.TimedOut()) {
        dsyslog("rpi5video/decoder: Sync d=%+lldms vPTS=%lld aPTS=%lld",
                static_cast<long long>(delta / 90),
                static_cast<long long>(originalPts / 90),
                static_cast<long long>(clock / 90));
#ifdef NDEBUG
        nextSyncLog.Set(30000);
#else
        nextSyncLog.Set(5000);
#endif
    }

    // Schwellwert je nach Modus
    const int64_t syncThreshold = liveMode ? DECODER_SYNC_THRESHOLD_LIVE
                                           : DECODER_SYNC_THRESHOLD_REPLAY;

    // Video zu früh: warten
    if (delta > 0 && (!syncCorrectionDone || delta > syncThreshold)) {
        const cTimeMs waitTimeout(3000);
        while (delta > 0 &&
               !stopping.load(std::memory_order_relaxed) &&
               trickSpeed.load(std::memory_order_relaxed) == 0 &&
               !waitTimeout.TimedOut()) {
            // Adaptiver Sleep: maximal 10 ms, aber weniger wenn fast synchron
            const int sleepMs = static_cast<int>(
                std::clamp(delta / 90, int64_t{1}, int64_t{10}));
            cCondWait::SleepMs(sleepMs);
            const int64_t updatedClock = audioProcessor->GetClock();
            if (updatedClock == AV_NOPTS_VALUE) break;
            delta = originalPts - updatedClock - latency;
        }
    }
    syncCorrectionDone = true;

    // Innerhalb Toleranz: einreichen
    if (std::abs(delta) <= syncThreshold)
        return display->SubmitFrame(std::move(frame), DECODER_SUBMIT_TIMEOUT_MS);

    // Video zu spät: droppen
    if (delta < 0) {
        dsyslog("rpi5video/decoder: Sync drop d=%+lldms", static_cast<long long>(delta / 90));
        return true;
    }

    // Noch zu früh nach Timeout: best-effort einreichen
    return display->SubmitFrame(std::move(frame), DECODER_SUBMIT_TIMEOUT_MS);
}
