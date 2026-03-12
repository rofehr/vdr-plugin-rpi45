// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file pes.h
 * @brief PES-Paket-Parsing und Codec-Erkennung
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/avutil.h>
}
#pragma GCC diagnostic pop

// ============================================================================
// === PES-PAKET ===
// ============================================================================

/**
 * @brief Geparster PES-Paket-Header.
 *
 * payload zeigt in den Eingabepuffer (kein Eigentümer).
 * Ungültige PTS/DTS werden als AV_NOPTS_VALUE zurückgegeben.
 */
struct PesPacket {
    const uint8_t *payload{nullptr}; ///< Zeiger auf ES-Nutzdaten im Eingabepuffer
    size_t   payloadSize{0};         ///< Größe der ES-Nutzdaten in Bytes
    int64_t  pts{AV_NOPTS_VALUE};    ///< Presentation Timestamp (90 kHz)
    int64_t  dts{AV_NOPTS_VALUE};    ///< Decoding Timestamp   (90 kHz)
    bool     isVideo{false};         ///< true = Video-PES
    bool     isAudio{false};         ///< true = Audio-PES
};

// ============================================================================
// === ÖFFENTLICHE API ===
// ============================================================================

/**
 * @brief Parst einen PES-Paket-Header.
 * @param data Rohe PES-Bytes (einschließlich Start-Code)
 * @return Gefülltes PesPacket; bei Fehler sind isVideo/isAudio false
 */
[[nodiscard]] auto ParsePes(std::span<const uint8_t> data) noexcept -> PesPacket;

/**
 * @brief Erkennt den Audio-Codec aus ES-Rohdaten.
 * @param data Elementary-Stream-Bytes
 * @return Erkannter AVCodecID oder AV_CODEC_ID_NONE
 */
[[nodiscard]] auto DetectAudioCodec(std::span<const uint8_t> data) noexcept -> AVCodecID;

/**
 * @brief Erkennt den Video-Codec aus ES-Rohdaten.
 *
 * Evidenzbasiert: sucht nach Annex-B Start-Codes und
 * akkumuliert NAL-Typ-Belege (HEVC VPS/SPS/PPS, H.264 SPS/PPS/IDR,
 * MPEG-2 Sequence-Header). Benötigt mindestens 2 verschiedene
 * starke Marker für eine sichere Entscheidung.
 *
 * @param data Elementary-Stream-Bytes
 * @return Erkannter AVCodecID oder AV_CODEC_ID_NONE
 */
[[nodiscard]] auto DetectVideoCodec(std::span<const uint8_t> data) noexcept -> AVCodecID;
