// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file config.h
 * @brief Plugin-Konfiguration (Setup-Menü und persistente Einstellungen)
 */
#pragma once

#include <string>
#include <cstdint>

// ============================================================================
// === DISPLAY-KONFIGURATION ===
// ============================================================================

struct DisplayConfig {
    uint32_t width{1920};        ///< Ausgabebreite in Pixel
    uint32_t height{1080};       ///< Ausgabehöhe in Pixel
    uint32_t refreshRate{50};    ///< Bildwiederholrate in Hz (50 für DVB-Europa)

    [[nodiscard]] auto GetWidth()       const noexcept -> uint32_t { return width; }
    [[nodiscard]] auto GetHeight()      const noexcept -> uint32_t { return height; }
    [[nodiscard]] auto GetRefreshRate() const noexcept -> uint32_t { return refreshRate; }
};

// ============================================================================
// === PLUGIN-GESAMTKONFIGURATION ===
// ============================================================================

struct Rpi5Config {
    DisplayConfig display;

    std::string drmDevice{"/dev/dri/card0"};   ///< DRM-Gerätepfad
    std::string audioDevice{"default"};         ///< ALSA-Gerätename

    /// Audio-Latenz-Kompensation in Millisekunden.
    /// Positiv = Video wartet auf Audio (Audio kommt früher an).
    int audioLatency{0};

    /// Deinterlacing aktivieren (Standard: an).
    bool deinterlace{true};

    /// Rauschunterdrückung aktivieren (nur wenn Hardware es unterstützt).
    bool denoise{false};

    /// Schärfe-Filter aktivieren.
    bool sharpness{false};
};

/// Globale Plugin-Konfiguration.
extern Rpi5Config rpi5Config;
