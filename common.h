// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file common.h
 * @brief Gemeinsame Typen, Hilfsfunktionen und Konstanten
 */
#pragma once

#include <string_view>
#include <array>
#include <cstdint>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
extern "C" {
#include <libavutil/error.h>
}
#pragma GCC diagnostic pop

// ============================================================================
// === VERSION ===
// ============================================================================

static constexpr std::string_view PLUGIN_VERSION  = "0.1.0";
static constexpr std::string_view PLUGIN_NAME     = "rpi5video";
static constexpr std::string_view PLUGIN_DESC     = "RPi5 Video-Ausgabe (FFmpeg/v4l2_request + DRM)";

// ============================================================================
// === VAAPI/HARDWARE KONTEXT ===
// ============================================================================

/**
 * @brief Gesammelte Hardware-Fähigkeiten des RPi5 Decoders.
 *
 * Wird einmalig beim Plugin-Start durch ProbeDecoderCaps() gefüllt
 * und danach nur gelesen (kein Mutex nötig).
 */
struct Rpi5Context {
    /// FFmpeg AVBufferRef auf den V4L2-Request Hardware-Device-Kontext.
    /// Eigentümer: cRpi5Device; alle anderen borgen nur eine Referenz.
    struct AVBufferRef *hwDeviceRef{nullptr};

    /// DRM-Dateideskriptor (primärer Node, z.B. /dev/dri/card0).
    int drmFd{-1};

    // --- Decoder-Fähigkeiten (durch ProbeDecoderCaps() gesetzt) ---
    bool hwH264{false};  ///< H.264 Hardware-Decode verfügbar
    bool hwHevc{false};  ///< H.265/HEVC Hardware-Decode verfügbar
    bool hwMpeg2{false}; ///< MPEG-2 Hardware-Decode verfügbar (optional)

    // --- VPP-Fähigkeiten ---
    bool hasDenoise{false};    ///< Rauschunterdrückung verfügbar
    bool hasSharpness{false};  ///< Schärfe-Filter verfügbar
    std::string deinterlaceMode; ///< Bester Deinterlace-Algorithmus ("" = keiner)
};

// ============================================================================
// === HILFSFUNKTIONEN ===
// ============================================================================

/**
 * @brief Wandelt einen FFmpeg-Fehlercode in einen lesbaren String.
 * @param errnum Negativer AVERROR-Wert
 * @return Nullterminierter Fehlertext (statischer Puffer, nicht thread-safe)
 */
[[nodiscard]] inline auto AvErr(int errnum) noexcept -> std::string_view {
    static thread_local std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
    av_strerror(errnum, buf.data(), buf.size());
    return {buf.data()};
}

// ============================================================================
// === SHUTDOWN-TIMEOUT ===
// ============================================================================

/// Maximale Wartezeit beim Herunterfahren von Threads (ms).
constexpr int SHUTDOWN_TIMEOUT_MS = 3000;
