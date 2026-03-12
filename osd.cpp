// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file osd.cpp
 * @brief VDR-OSD-Provider für RPi5 via DRM-Dumb-Buffer Overlay-Plane
 *
 * Datenpfad:
 *   VDR cBitmap (palette/truecolor) → Composite in ARGB8888-Puffer
 *   → DRM-Dumb-Buffer (mmap) → drmModeAddFB2() → display->UpdateOsd()
 *   → nächster DRM-Atomic-Commit zeigt OSD und Video synchron an
 *
 * Warum Dumb-Buffer statt DMA-BUF/GEM?
 *   - Kein V4L2/VAAPI nötig
 *   - CPU-seitiger mmap-Zugriff ohne Kernel-Copy
 *   - Von allen DRM-Treibern unterstützt (auch vc4 auf RPi5)
 *   - Ausreichend schnell für OSD (< 60 Hz, selten volle Aktualisierung)
 *
 * Thread-Sicherheit:
 *   Flush() wird immer vom VDR-Hauptthread aufgerufen.
 *   display->UpdateOsd() ist intern durch einen cMutex geschützt.
 */

#include "osd.h"
#include "common.h"
#include "display.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <sys/mman.h>

#include <libdrm/drm.h>
#include <libdrm/drm_fourcc.h>
#include <libdrm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <vdr/osd.h>
#include <vdr/tools.h>

// ============================================================================
// === DUMB BUFFER ===
// ============================================================================

DumbBuffer::~DumbBuffer() noexcept {
    if (map && map != MAP_FAILED && size > 0)
        munmap(map, size);

    if (fbId != 0 && drmFd >= 0)
        drmModeRmFB(drmFd, fbId);

    if (handle != 0 && drmFd >= 0) {
        drm_mode_destroy_dumb destroyArgs{.handle = handle};
        drmIoctl(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyArgs);
    }
}

DumbBuffer::DumbBuffer(DumbBuffer &&other) noexcept
    : drmFd(other.drmFd), handle(other.handle), fbId(other.fbId),
      width(other.width), height(other.height), pitch(other.pitch),
      size(other.size), map(other.map) {
    other.drmFd  = -1;
    other.handle = 0;
    other.fbId   = 0;
    other.map    = nullptr;
    other.size   = 0;
}

auto DumbBuffer::operator=(DumbBuffer &&other) noexcept -> DumbBuffer & {
    if (this != &other) {
        // Alten Buffer freigeben
        if (map && map != MAP_FAILED && size > 0) munmap(map, size);
        if (fbId   && drmFd >= 0) drmModeRmFB(drmFd, fbId);
        if (handle && drmFd >= 0) {
            drm_mode_destroy_dumb d{.handle = handle};
            drmIoctl(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        }
        drmFd  = other.drmFd;  handle = other.handle;
        fbId   = other.fbId;   width  = other.width;
        height = other.height; pitch  = other.pitch;
        size   = other.size;   map    = other.map;
        other.drmFd = -1; other.handle = 0; other.fbId = 0;
        other.map = nullptr; other.size = 0;
    }
    return *this;
}

// ============================================================================
// === DUMB BUFFER ERSTELLEN ===
// ============================================================================

[[nodiscard]] auto CreateDumbBuffer(int drmFd, uint32_t width, uint32_t height) -> DumbBuffer {
    DumbBuffer buf;
    buf.drmFd  = drmFd;
    buf.width  = width;
    buf.height = height;

    // Schritt 1: Dumb-Buffer allozieren (ARGB8888 = 32 bpp)
    drm_mode_create_dumb createArgs{};
    createArgs.width  = width;
    createArgs.height = height;
    createArgs.bpp    = 32;

    if (drmIoctl(drmFd, DRM_IOCTL_MODE_CREATE_DUMB, &createArgs) != 0) {
        esyslog("rpi5video/osd: DRM_IOCTL_MODE_CREATE_DUMB fehlgeschlagen: %s", strerror(errno));
        return {};
    }

    buf.handle = createArgs.handle;
    buf.pitch  = createArgs.pitch;
    buf.size   = createArgs.size;

    // Schritt 2: Als KMS-Framebuffer registrieren (ARGB8888 = AR24)
    // drmModeAddFB2() benötigt pro Plane: handle, pitch, offset
    // ARGB8888 hat eine einzelne Plane mit Offset 0.
    const uint32_t handles[4]  = {buf.handle, 0, 0, 0};
    const uint32_t pitches[4]  = {buf.pitch,  0, 0, 0};
    const uint32_t offsets[4]  = {0,          0, 0, 0};

    if (drmModeAddFB2(drmFd, width, height,
                      DRM_FORMAT_ARGB8888,
                      handles, pitches, offsets,
                      &buf.fbId, 0) != 0) {
        esyslog("rpi5video/osd: drmModeAddFB2 fehlgeschlagen: %s", strerror(errno));
        // GEM-Handle wieder freigeben
        drm_mode_destroy_dumb d{.handle = buf.handle};
        drmIoctl(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        return {};
    }

    // Schritt 3: mmap für CPU-Schreibzugriff
    drm_mode_map_dumb mapArgs{};
    mapArgs.handle = buf.handle;

    if (drmIoctl(drmFd, DRM_IOCTL_MODE_MAP_DUMB, &mapArgs) != 0) {
        esyslog("rpi5video/osd: DRM_IOCTL_MODE_MAP_DUMB fehlgeschlagen: %s", strerror(errno));
        drmModeRmFB(drmFd, buf.fbId);
        drm_mode_destroy_dumb d{.handle = buf.handle};
        drmIoctl(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        return {};
    }

    buf.map = mmap(nullptr, buf.size,
                   PROT_READ | PROT_WRITE, MAP_SHARED,
                   drmFd, static_cast<off_t>(mapArgs.offset));

    if (buf.map == MAP_FAILED) {
        esyslog("rpi5video/osd: mmap fehlgeschlagen: %s", strerror(errno));
        drmModeRmFB(drmFd, buf.fbId);
        drm_mode_destroy_dumb d{.handle = buf.handle};
        drmIoctl(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        buf.map = nullptr;
        return {};
    }

    // Puffer initial transparent füllen
    std::memset(buf.map, 0, buf.size);

    dsyslog("rpi5video/osd: Dumb-Buffer %ux%u pitch=%u size=%llu FB=%u erstellt",
            width, height, buf.pitch,
            static_cast<unsigned long long>(buf.size), buf.fbId);
    return buf;
}

// ============================================================================
// === cRpi5Osd ===
// ============================================================================

cRpi5Osd::cRpi5Osd(int drmFd_, cRpi5Display *display_, int x, int y, uint level)
    : cOsd(x, y, level), drmFd(drmFd_), display(display_) {
    dsyslog("rpi5video/osd: erstellt x=%d y=%d level=%u", x, y, level);
}

cRpi5Osd::~cRpi5Osd() {
    // OSD vom Display entfernen bevor der Buffer zerstört wird
    if (display) {
        cRpi5Display::OsdBuffer empty{};
        display->UpdateOsd(empty);
    }
    dsyslog("rpi5video/osd: zerstört");
}

// ============================================================================
// === BUFFER-VERWALTUNG ===
// ============================================================================

[[nodiscard]] auto cRpi5Osd::EnsureBuffer(uint32_t width, uint32_t height) -> bool {
    if (dumbBuf.IsValid() && bufWidth == width && bufHeight == height)
        return true; // Bereits passend alloziert

    // Alten Buffer freigeben (Destruktor macht das automatisch)
    dumbBuf = DumbBuffer{};
    bufWidth = bufHeight = 0;

    if (drmFd < 0 || width == 0 || height == 0) [[unlikely]] return false;

    dumbBuf = CreateDumbBuffer(drmFd, width, height);
    if (!dumbBuf.IsValid()) return false;

    bufWidth  = width;
    bufHeight = height;
    return true;
}

auto cRpi5Osd::ClearBuffer() -> void {
    if (dumbBuf.map && dumbBuf.size > 0)
        std::memset(dumbBuf.map, 0, dumbBuf.size);
}

// ============================================================================
// === BITMAP-COMPOSITING ===
// ============================================================================

/**
 * @brief Composited alle VDR-Bitmaps in den Dumb-Buffer.
 *
 * VDR's cOsd kann mehrere cBitmap-Objekte enthalten (z.B. bei
 * True-Color-OSD mehrere Ebenen). Jede Bitmap liegt an einer bestimmten
 * (x,y)-Position relativ zum OSD-Ursprung.
 *
 * Pixelformat-Konvertierung:
 *   - True-Color-Bitmap (tARGB): direkt als ARGB8888 übernehmen.
 *   - Palette-Bitmap (tIndex):    jeder Index → tColor (ARGB32).
 *   tColor ist uint32_t mit Layout 0xAARRGGBB (VDR-intern),
 *   DRM_FORMAT_ARGB8888 erwartet ebenfalls ARGB im Speicher
 *   (little-endian: B G R A). Auf ARM LE passt das direkt.
 */
auto cRpi5Osd::CompositeBitmaps() -> void {
    if (!dumbBuf.map) return;

    auto *dst = static_cast<uint32_t *>(dumbBuf.map);
    const uint32_t dstPitch = dumbBuf.pitch / 4; // Pitch in Pixel

    // Über alle Bitmaps iterieren
    for (int bitmapIdx = 0; ; ++bitmapIdx) {
        const cBitmap *bitmap = GetBitmap(bitmapIdx);
        if (!bitmap) break;

        const int bmpX = bitmap->X0(); // Position relativ zum OSD-Ursprung
        const int bmpY = bitmap->Y0();
        const int bmpW = bitmap->Width();
        const int bmpH = bitmap->Height();

        for (int y = 0; y < bmpH; ++y) {
            const int dstY = bmpY + y;
            if (dstY < 0 || static_cast<uint32_t>(dstY) >= bufHeight) continue;

            for (int x = 0; x < bmpW; ++x) {
                const int dstX = bmpX + x;
                if (dstX < 0 || static_cast<uint32_t>(dstX) >= bufWidth) continue;

                // tColor = 0xAARRGGBB (VDR)
                // ARGB8888 im Speicher (LE): Byte0=B Byte1=G Byte2=R Byte3=A
                // = uint32_t 0xAARRGGBB → passt 1:1
                const tColor color = bitmap->GetColor(x, y);

                // Alpha-Blending mit bestehendem Inhalt (für überlagerte Bitmaps)
                const uint32_t srcA = (color >> 24) & 0xFF;

                if (srcA == 0) continue; // Vollständig transparent: überspringen

                if (srcA == 0xFF) {
                    // Vollständig opak: direkt schreiben
                    dst[static_cast<uint32_t>(dstY) * dstPitch +
                        static_cast<uint32_t>(dstX)] = color;
                } else {
                    // Alpha-Blending: src over dst
                    const uint32_t existing = dst[static_cast<uint32_t>(dstY) * dstPitch +
                                                   static_cast<uint32_t>(dstX)];
                    const uint32_t dstA = (existing >> 24) & 0xFF;
                    const uint32_t dstR = (existing >> 16) & 0xFF;
                    const uint32_t dstG = (existing >>  8) & 0xFF;
                    const uint32_t dstB =  existing        & 0xFF;

                    const uint32_t srcR = (color >> 16) & 0xFF;
                    const uint32_t srcG = (color >>  8) & 0xFF;
                    const uint32_t srcB =  color        & 0xFF;

                    const uint32_t outA = srcA + ((dstA * (255 - srcA)) / 255);
                    const uint32_t outR = (srcR * srcA + dstR * dstA * (255 - srcA) / 255) /
                                          std::max(outA, 1U);
                    const uint32_t outG = (srcG * srcA + dstG * dstA * (255 - srcA) / 255) /
                                          std::max(outA, 1U);
                    const uint32_t outB = (srcB * srcA + dstB * dstA * (255 - srcA) / 255) /
                                          std::max(outA, 1U);

                    dst[static_cast<uint32_t>(dstY) * dstPitch +
                        static_cast<uint32_t>(dstX)] =
                        (outA << 24) | (outR << 16) | (outG << 8) | outB;
                }
            }
        }
    }
}

auto cRpi5Osd::UploadToDisplay() -> void {
    if (!display || !dumbBuf.IsValid()) return;

    cRpi5Display::OsdBuffer osd;
    osd.fbId   = dumbBuf.fbId;
    osd.width  = bufWidth;
    osd.height = bufHeight;
    display->UpdateOsd(osd);
}

// ============================================================================
// === FLUSH ===
// ============================================================================

/**
 * @brief Überträgt alle ausstehenden OSD-Änderungen zum DRM-Display.
 *
 * Ablauf:
 *  1. Gesamtgröße aller Bitmaps berechnen
 *  2. Dumb-Buffer sicherstellen (ggf. neu allozieren)
 *  3. Buffer leeren (transparent)
 *  4. Alle Bitmaps compositen
 *  5. FB-ID an Display übergeben → nächster Page-Flip zeigt OSD
 */
auto cRpi5Osd::Flush() -> void {
    if (!IsDirty()) return;

    // Bounding-Box aller Bitmaps berechnen
    int maxX = 0;
    int maxY = 0;
    bool hasBitmaps = false;

    for (int i = 0; ; ++i) {
        const cBitmap *bmp = GetBitmap(i);
        if (!bmp) break;
        hasBitmaps = true;
        const int right  = bmp->X0() + bmp->Width();
        const int bottom = bmp->Y0() + bmp->Height();
        if (right  > maxX) maxX = right;
        if (bottom > maxY) maxY = bottom;
    }

    if (!hasBitmaps) {
        // Alle Bitmaps entfernt → OSD ausblenden
        if (display) {
            cRpi5Display::OsdBuffer empty{};
            display->UpdateOsd(empty);
        }
        dumbBuf = DumbBuffer{};
        bufWidth = bufHeight = 0;
        return;
    }

    const auto width  = static_cast<uint32_t>(maxX);
    const auto height = static_cast<uint32_t>(maxY);

    if (width == 0 || height == 0) return;

    // Buffer sicherstellen
    if (!EnsureBuffer(width, height)) {
        esyslog("rpi5video/osd: Dumb-Buffer %ux%u allozieren fehlgeschlagen", width, height);
        return;
    }

    // Compositing
    ClearBuffer();
    CompositeBitmaps();
    UploadToDisplay();
}

// ============================================================================
// === cRpi5OsdProvider ===
// ============================================================================

cRpi5OsdProvider::cRpi5OsdProvider(int drmFd_, cRpi5Display *display_)
    : drmFd(drmFd_), display(display_) {
    isyslog("rpi5video/osd: Provider erstellt");
}

auto cRpi5OsdProvider::AttachDisplay(cRpi5Display *newDisplay) noexcept -> void {
    display = newDisplay;
    dsyslog("rpi5video/osd: Display angehängt");
}

auto cRpi5OsdProvider::DetachDisplay() noexcept -> void {
    display = nullptr;
    dsyslog("rpi5video/osd: Display abgehängt");
}

[[nodiscard]] auto cRpi5OsdProvider::CreateOsd(int Left, int Top, uint Level) -> cOsd * {
    if (!display || drmFd < 0) [[unlikely]] {
        esyslog("rpi5video/osd: CreateOsd ohne Display aufgerufen");
        return nullptr;
    }
    dsyslog("rpi5video/osd: CreateOsd(%d, %d, %u)", Left, Top, Level);
    return new cRpi5Osd(drmFd, display, Left, Top, Level);
}
