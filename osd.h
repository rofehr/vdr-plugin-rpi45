// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file osd.h
 * @brief VDR-OSD-Provider für RPi5 via DRM-PRIME Overlay-Plane
 *
 * Architektur:
 *
 *   VDR ruft cRpi5OsdProvider::CreateOsd() auf
 *       → gibt cRpi5Osd zurück
 *   VDR zeichnet in cRpi5Osd (Bitmaps, Text, Menüs)
 *       → Flush() konvertiert ARGB8888 → DRM-Framebuffer
 *       → display->UpdateOsd() übergibt FB-ID an DRM-Atomic-Commit
 *
 * Pixelformat:
 *   VDR liefert OSD-Bitmaps intern als ARGB8888 (tARGB).
 *   Der DRM-Overlay akzeptiert AR24 (DRM_FORMAT_ARGB8888).
 *   Da das Format identisch ist, wird kein Pixel-Konverter benötigt.
 *
 * Speicher:
 *   OSD-Framebuffer wird als DRM-DUMB-Buffer angelegt
 *   (kein DMA-BUF nötig, kein VAAPI, kein V4L2).
 *   drmIoctl(DRM_IOCTL_MODE_CREATE_DUMB) → mmap → füllen →
 *   drmModeAddFB2() → display->UpdateOsd().
 */
#pragma once

#include "display.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <libdrm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <vdr/osd.h>

// ============================================================================
// === DRM DUMB BUFFER ===
// ============================================================================

/**
 * @brief RAII-Wrapper für einen DRM-Dumb-Buffer.
 *
 * Dumb-Buffer sind CPU-seitig per mmap beschreibbar und vom KMS-Treiber
 * direkt als Framebuffer verwendbar — ohne DMA-BUF-Import oder GEM-Handle.
 * Ideal für OSD-Inhalte, die selten wechseln.
 */
struct DumbBuffer {
    int      drmFd{-1};
    uint32_t handle{0};   ///< GEM-Handle
    uint32_t fbId{0};     ///< KMS-Framebuffer-ID
    uint32_t width{0};
    uint32_t height{0};
    uint32_t pitch{0};    ///< Bytes pro Zeile
    uint64_t size{0};     ///< Gesamtgröße in Bytes
    void    *map{nullptr};///< mmap-Zeiger (ARGB8888)

    DumbBuffer() = default;
    ~DumbBuffer() noexcept;
    DumbBuffer(DumbBuffer &&) noexcept;
    DumbBuffer &operator=(DumbBuffer &&) noexcept;
    DumbBuffer(const DumbBuffer &)            = delete;
    DumbBuffer &operator=(const DumbBuffer &) = delete;

    [[nodiscard]] auto IsValid() const noexcept -> bool { return fbId != 0 && map != nullptr; }
};

/**
 * @brief Erstellt einen DRM-Dumb-Buffer der Größe width×height (ARGB8888).
 * @return Befüllter DumbBuffer, IsValid()==false bei Fehler.
 */
[[nodiscard]] auto CreateDumbBuffer(int drmFd, uint32_t width, uint32_t height) -> DumbBuffer;

// ============================================================================
// === OSD-SURFACE ===
// ============================================================================

/**
 * @brief Eine einzelne VDR-OSD-Bitmap-Ebene, die in den Dumb-Buffer gerendert wird.
 *
 * VDR kann pro Kanal mehrere cBitmap-Objekte an verschiedenen Positionen
 * haben (z.B. Hauptmenü + Statusleiste). cRpi5Osd hält sie alle und
 * composited sie beim Flush() in den einzelnen Dumb-Buffer.
 */
class cRpi5Osd : public cOsd {
public:
    /**
     * @param drmFd   DRM-Dateideskriptor (geliehen vom Display)
     * @param display Zeiger auf das Display für UpdateOsd()
     * @param x       OSD-Ursprung X (Pixel, linke Kante)
     * @param y       OSD-Ursprung Y (Pixel, obere Kante)
     * @param level   VDR-OSD-Ebene (0=hinterste, 127=vorderste)
     */
    cRpi5Osd(int drmFd, cRpi5Display *display, int x, int y, uint level);
    ~cRpi5Osd() override;

    // --- VDR-OSD-Schnittstelle ---
    auto Flush() -> void override;

private:
    // --- Interne Hilfsmethoden ---
    [[nodiscard]] auto EnsureBuffer(uint32_t width, uint32_t height) -> bool;
    auto ClearBuffer()     -> void;
    auto CompositeBitmaps()-> void;
    auto UploadToDisplay() -> void;

    int           drmFd{-1};
    cRpi5Display *display{nullptr};
    DumbBuffer    dumbBuf;

    /// Breite/Höhe des zuletzt allokierten Dumb-Buffers.
    uint32_t bufWidth{0};
    uint32_t bufHeight{0};
};

// ============================================================================
// === OSD-PROVIDER ===
// ============================================================================

/**
 * @brief VDR-OSD-Provider für den RPi5.
 *
 * Wird einmalig in cRpi5Device::MakePrimaryDevice() registriert.
 * VDR ruft CreateOsd() für jedes neue OSD-Fenster auf
 * (Menü, EPG, Untertitel, Status-Icons, …).
 */
class cRpi5OsdProvider : public cOsdProvider {
public:
    /**
     * @param drmFd   DRM-Dateideskriptor (geliehen, nicht Eigentümer)
     * @param display Zeiger auf das Display-Objekt
     */
    explicit cRpi5OsdProvider(int drmFd, cRpi5Display *display);
    ~cRpi5OsdProvider() override = default;

    // --- Display-Attachment (für Detach/Re-Attach) ---
    auto AttachDisplay(cRpi5Display *newDisplay) noexcept -> void;
    auto DetachDisplay() noexcept -> void;

private:
    [[nodiscard]] auto CreateOsd(int Left, int Top, uint Level) -> cOsd * override;
    [[nodiscard]] auto ProvidesTrueColor() -> bool override { return true; }

    int           drmFd{-1};
    cRpi5Display *display{nullptr};
};
