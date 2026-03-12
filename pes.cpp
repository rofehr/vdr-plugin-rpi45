// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file pes.cpp
 * @brief PES-Paket-Parsing und Codec-Erkennung
 *
 * Portiert vom vaapivideo-Plugin; keine VAAPI-Abhängigkeiten,
 * läuft unverändert auf dem RPi5.
 */

#include "pes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/avutil.h>
#include <libavutil/intreadwrite.h>
}
#pragma GCC diagnostic pop

#include <vdr/tools.h>

// ============================================================================
// === KONSTANTEN ===
// ============================================================================

constexpr size_t PES_HEADER_EXT_OFFSET  = 9U;
constexpr size_t PES_HEADER_MIN_SIZE    = 6U;
constexpr size_t PES_OFFSET_FLAGS2      = 7U;
constexpr size_t PES_OFFSET_HDR_DATA_LEN= 8U;
constexpr size_t PES_TIMESTAMP_SIZE     = 5U;
constexpr uint32_t PES_START_CODE_PREFIX= 0x000001U;
constexpr uint8_t PES_STREAM_ID_AUDIO_FIRST = 0xC0;
constexpr uint8_t PES_STREAM_ID_PRIVATE     = 0xBD;
constexpr uint8_t PES_STREAM_ID_VIDEO_FIRST = 0xE0;

// ============================================================================
// === INTERNE HILFSFUNKTIONEN ===
// ============================================================================

[[nodiscard]] static inline auto ParseTimestamp(const uint8_t *bytes) noexcept -> int64_t {
    if ((AV_RB8(bytes) & AV_RB8(bytes + 2) & AV_RB8(bytes + 4) & 0x01) == 0) [[unlikely]]
        return AV_NOPTS_VALUE;

    const uint64_t b32_30 = (AV_RB8(bytes) >> 1) & 0x07;
    const uint64_t b29_15 = (AV_RB16(bytes + 1) >> 1) & 0x7FFF;
    const uint64_t b14_0  = (AV_RB16(bytes + 3) >> 1) & 0x7FFF;
    return static_cast<int64_t>((b32_30 << 30) | (b29_15 << 15) | b14_0);
}

namespace {
struct CodecEvidence {
    uint8_t seenMask{};
    int     hits{};
    size_t  lastPos{};

    auto Record(uint8_t bit, size_t pos) noexcept -> void {
        if ((seenMask & bit) == 0) { seenMask |= bit; ++hits; }
        lastPos = pos;
    }
};
} // namespace

// ============================================================================
// === CODEC-ERKENNUNG ===
// ============================================================================

[[nodiscard]] auto DetectAudioCodec(std::span<const uint8_t> data) noexcept -> AVCodecID {
    if (data.size() < 4) [[unlikely]] return AV_CODEC_ID_NONE;

    const size_t  size = data.size();
    const uint8_t *p   = data.data();

    for (size_t i = 0; i + 4 <= size; ++i) {
        const uint16_t sync = AV_RB16(p + i);
        if ((sync & 0xFF00) == 0xFF00) [[unlikely]] {
            if ((sync & 0xFFF6) == 0xFFF0) return AV_CODEC_ID_AAC;
            if ((sync & 0xFFE0) == 0xFFE0) return AV_CODEC_ID_MP2;
        }
        if (sync == 0x0B77) [[unlikely]] {
            if (i + 5 < size && ((AV_RB8(p + i + 5) >> 3) & 0x1F) > 10)
                return AV_CODEC_ID_EAC3;
            return AV_CODEC_ID_AC3;
        }
        if (i + 4 <= size && AV_RB32(p + i) == 0x7FFE8001) [[unlikely]]
            return AV_CODEC_ID_DTS;
    }
    return AV_CODEC_ID_NONE;
}

[[nodiscard]] auto DetectVideoCodec(std::span<const uint8_t> data) noexcept -> AVCodecID {
    const size_t size = data.size();
    if (size < 6) [[unlikely]] return AV_CODEC_ID_NONE;

    const uint8_t *p = data.data();
    CodecEvidence hevc{}, avc{}, mpeg2{};
    constexpr uint8_t kHevcParamMask = 0x07;

    for (size_t i = 0; i + 4 <= size;) {
        const auto *found = static_cast<const uint8_t *>(std::memchr(p + i, 0x00, size - i));
        if (!found) [[unlikely]] break;
        i = static_cast<size_t>(found - p);
        if (i + 4 > size) [[unlikely]] break;

        if (p[i + 1] != 0x00) { ++i; continue; }

        size_t scLen = 0;
        if      (p[i + 2] == 0x01)                             scLen = 3;
        else if (i + 3 < size && p[i + 2] == 0x00 && p[i + 3] == 0x01) scLen = 4;

        if (scLen == 0) { ++i; continue; }

        const size_t nalPos = i + scLen;
        if (nalPos >= size) [[unlikely]] break;

        const uint8_t b0 = p[nalPos];

        // MPEG-2
        { uint8_t bit = 0;
          if      (b0 == 0xB3) bit = 0x01;
          else if (b0 == 0xB5) bit = 0x02;
          else if (b0 == 0xB8) bit = 0x04;
          if (bit) {
              mpeg2.Record(bit, i);
              if ((mpeg2.seenMask & 0x01) && (mpeg2.seenMask & 0x06)) break;
          }
        }

        // HEVC
        if (nalPos + 1 < size && (b0 & 0x80) == 0) {
            const uint8_t b1 = p[nalPos + 1];
            if (b1 & 0x07) {
                const uint8_t hevcType = (b0 >> 1) & 0x3F;
                const bool layerIdZero = ((b0 & 0x01) == 0) && (((b1 >> 3) & 0x1F) == 0);
                uint8_t bit = 0;
                if      (hevcType == 32 && layerIdZero) bit = 0x01;
                else if (hevcType == 33 && layerIdZero) bit = 0x02;
                else if (hevcType == 34 && layerIdZero) bit = 0x04;
                else if ((hevcType == 19 || hevcType == 20 || hevcType == 21) &&
                         layerIdZero && (hevc.seenMask & kHevcParamMask))
                    bit = 0x08;
                if (bit) hevc.Record(bit, i);
            }
        }

        // H.264
        if ((b0 & 0x80) == 0 && (b0 & 0x60) != 0) {
            const uint8_t avcType = b0 & 0x1F;
            uint8_t bit = 0;
            if      (avcType == 7) bit = 0x01;
            else if (avcType == 8) bit = 0x02;
            else if (avcType == 5) bit = 0x04;
            if (bit) avc.Record(bit, i);
        }
        ++i;
    }

    const bool hevcOk = (hevc.hits >= 2) &&
                        ((hevc.seenMask & 0x01) ||
                         (__builtin_popcount(hevc.seenMask & 0x07) >= 2));
    const bool avcOk  = (avc.hits  >= 2);
    const bool mpegOk = (mpeg2.seenMask & 0x01) && (mpeg2.seenMask & 0x06);

    const bool hevcFinal = hevcOk && !mpegOk;
    const bool avcFinal  = avcOk  && !mpegOk;

    AVCodecID best = AV_CODEC_ID_NONE;
    size_t bestPos = 0; int bestHits = 0;

    const auto consider = [&](AVCodecID id, bool ok, const CodecEvidence &ev) noexcept {
        if (!ok) return;
        if (best == AV_CODEC_ID_NONE || ev.hits > bestHits ||
            (ev.hits == bestHits && ev.lastPos > bestPos)) {
            best = id; bestPos = ev.lastPos; bestHits = ev.hits;
        }
    };
    consider(AV_CODEC_ID_HEVC,       hevcFinal, hevc);
    consider(AV_CODEC_ID_H264,       avcFinal,  avc);
    consider(AV_CODEC_ID_MPEG2VIDEO, mpegOk,    mpeg2);
    return best;
}

// ============================================================================
// === PES-PARSING ===
// ============================================================================

[[nodiscard]] auto ParsePes(std::span<const uint8_t> data) noexcept -> PesPacket {
    PesPacket result{};
    const size_t size = data.size();
    if (size < PES_HEADER_MIN_SIZE) [[unlikely]] return result;

    const uint8_t *p = data.data();
    if (AV_RB24(p) != PES_START_CODE_PREFIX) [[unlikely]] return result;

    const uint8_t streamId = p[3];
    const bool isVideo = (streamId & 0xF0) == PES_STREAM_ID_VIDEO_FIRST;
    const bool isAudio = ((streamId & 0xE0) == PES_STREAM_ID_AUDIO_FIRST) ||
                         (streamId == PES_STREAM_ID_PRIVATE);
    if (!isVideo && !isAudio) [[unlikely]] return result;

    result.isVideo = isVideo;
    result.isAudio = isAudio;

    if (size < PES_HEADER_EXT_OFFSET) [[unlikely]] return result;

    const uint8_t headerExtLen = p[PES_OFFSET_HDR_DATA_LEN];
    const size_t  headerSize   = PES_HEADER_EXT_OFFSET + headerExtLen;
    if (headerSize > size) [[unlikely]] return result;

    const uint8_t ptsDtsFlags = p[PES_OFFSET_FLAGS2] & 0xC0;
    if (ptsDtsFlags == 0x80 && headerExtLen >= PES_TIMESTAMP_SIZE) [[likely]]
        result.pts = ParseTimestamp(p + PES_HEADER_EXT_OFFSET);
    else if (ptsDtsFlags == 0xC0 && headerExtLen >= PES_TIMESTAMP_SIZE * 2) {
        result.pts = ParseTimestamp(p + PES_HEADER_EXT_OFFSET);
        result.dts = ParseTimestamp(p + PES_HEADER_EXT_OFFSET + PES_TIMESTAMP_SIZE);
    }

    size_t payloadLen = size - headerSize;
    const uint16_t pesLen = AV_RB16(p + 4);
    if (pesLen > 0) [[unlikely]] {
        const auto declared = static_cast<size_t>(pesLen);
        if (declared + 6 > headerSize)
            payloadLen = std::min(declared + 6 - headerSize, payloadLen);
        else
            payloadLen = 0;
    }
    if (payloadLen > 0) [[likely]] {
        result.payload     = p + headerSize;
        result.payloadSize = payloadLen;
    }
    return result;
}
