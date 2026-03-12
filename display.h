// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file display.h
 * @brief DRM-Atomic-Modesetting und Page-Flip-Ausgabe für RPi5
 *
 * Unterschied zu VAAPI:
 *  - Frames kommen als AV_PIX_FMT_DRM_PRIME aus dem v4l2_request-Decoder.
 *    Kein av_hwframe_map() nötig: AVDRMFrameDescriptor ist direkt verwendbar.
 *  - drmModeAddFB2WithModifiers() wird identisch aufgerufen.
 */
#pragma once

#include "decoder.h"

#include <atomic>
#include <cstdint>
#include <memory>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}
#pragma GCC diagnostic pop

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <vdr/thread.h>

// ============================================================================
// === DRM PROPERTY IDS ===
// ============================================================================

struct PlaneProps {
    uint32_t crtcId{0}, fbId{0};
    uint32_t srcX{0}, srcY{0}, srcW{0}, srcH{0};
    uint32_t crtcX{0}, crtcY{0}, crtcW{0}, crtcH{0};
    uint32_t colorEncoding{0}, colorRange{0};
    uint64_t colorEncodingBt709{0}, colorRangeLimited{0};
    bool     colorEncodingValid{false}, colorRangeValid{false};
};

struct ModesetProps {
    uint32_t crtcActive{0}, crtcModeId{0}, connectorCrtcId{0};
    bool     isValid{false};
};

// ============================================================================
// === ATOMIC REQUEST WRAPPER ===
// ============================================================================

class AtomicRequest {
public:
    AtomicRequest();
    ~AtomicRequest() noexcept;
    AtomicRequest(AtomicRequest &&) noexcept;
    AtomicRequest &operator=(AtomicRequest &&) noexcept;
    AtomicRequest(const AtomicRequest &)            = delete;
    AtomicRequest &operator=(const AtomicRequest &) = delete;

    auto AddProperty(uint32_t objId, uint32_t propId, uint64_t value) -> void;
    [[nodiscard]] auto Count()  const noexcept -> int;
    [[nodiscard]] auto Handle() const noexcept -> drmModeAtomicReq *;

private:
    int               propCount{0};
    drmModeAtomicReq *request{nullptr};
};

// ============================================================================
// === DISPLAY ===
// ============================================================================

/**
 * @brief Verwaltet DRM-Ausgabe für den RPi5.
 *
 * Nimmt fertige Rpi5Frame-Objekte (AV_PIX_FMT_DRM_PRIME) entgegen,
 * importiert deren DMA-BUF-FDs als KMS-Framebuffer und führt
 * atomare Page-Flips durch.
 */
class cRpi5Display {
public:
    cRpi5Display();
    ~cRpi5Display() noexcept;

    // --- Lebenszyklus ---
    [[nodiscard]] auto Initialize(int drmFd,
                                   AVBufferRef *hwDeviceRef,
                                   uint32_t crtcId,
                                   uint32_t connectorId,
                                   const drmModeModeInfo &mode) -> bool;
    auto Shutdown()      -> void;
    auto BeginStreamSwitch() -> void;
    auto EndStreamSwitch()   -> void;

    // --- Frame-Ausgabe ---
    [[nodiscard]] auto SubmitFrame(std::unique_ptr<Rpi5Frame> frame, int timeoutMs) -> bool;

    // --- Abfragen ---
    [[nodiscard]] auto IsInitialized()  const noexcept -> bool;
    [[nodiscard]] auto GetOutputWidth()  const noexcept -> uint32_t;
    [[nodiscard]] auto GetOutputHeight() const noexcept -> uint32_t;
    [[nodiscard]] auto GetAspectRatio()  const noexcept -> double;

    // --- OSD ---
    struct OsdBuffer {
        uint32_t fbId{0};
        uint32_t width{0}, height{0};
    };
    auto UpdateOsd(const OsdBuffer &osd) -> void;

private:
    // --- DRM-Framebuffer ---
    struct DrmFramebuffer {
        DrmFramebuffer() = default;
        DrmFramebuffer(DrmFramebuffer &&) noexcept;
        DrmFramebuffer &operator=(DrmFramebuffer &&) noexcept;
        ~DrmFramebuffer() noexcept;
        DrmFramebuffer(const DrmFramebuffer &)            = delete;
        DrmFramebuffer &operator=(const DrmFramebuffer &) = delete;

        [[nodiscard]] auto IsValid() const noexcept -> bool { return fbId != 0; }

        int      drmFd{-1};
        uint32_t fbId{0};
        uint32_t gemHandle{0};
        uint32_t width{0}, height{0};
        uint64_t modifier{0};
        AVFrame *frame{nullptr}; ///< Hält DMA-BUF-FD am Leben
    };

    // --- Interne Methoden ---
    [[nodiscard]] auto DiscoverPlanes()                              -> bool;
    [[nodiscard]] auto DiscoverProperties()                          -> bool;
    [[nodiscard]] auto PerformModeset()                              -> bool;
    [[nodiscard]] auto MapDrmPrimeFrame(std::unique_ptr<Rpi5Frame> f) const -> DrmFramebuffer;
    [[nodiscard]] auto PresentBuffer(const DrmFramebuffer &fb)       -> bool;
    [[nodiscard]] auto AtomicCommit(AtomicRequest &req, uint32_t flags) -> bool;
    auto AppendOsdPlane(AtomicRequest &req, const OsdBuffer &osd)    -> void;
    auto WaitForPageFlip(int timeoutMs)                               -> void;
    [[nodiscard]] auto DrainDrmEvents(int timeoutMs)                  -> bool;

    static auto OnPageFlipEvent(int fd, unsigned seq, unsigned sec,
                                unsigned usec, void *data) -> void;

    // --- Display-Thread ---
    class DisplayThread : public cThread {
    public:
        explicit DisplayThread(cRpi5Display *owner);
        auto Action() -> void override;
        auto Stop()   -> void;
        std::atomic<bool> stopping{false};
        cRpi5Display *display;
    };
    std::unique_ptr<DisplayThread> displayThread;

    // --- Frame-Queue ---
    mutable cMutex       frameMutex;
    cCondVar             frameCondition;
    std::unique_ptr<Rpi5Frame> pendingFrame;

    // --- DRM-Zustand ---
    int              drmFd{-1};
    AVBufferRef     *hwDeviceRef{nullptr};
    uint32_t         crtcId{0};
    uint32_t         connectorId{0};
    uint32_t         videoPlaneId{0};
    uint32_t         osdPlaneId{0};
    uint32_t         outputWidth{0};
    uint32_t         outputHeight{0};
    drmModeModeInfo  activeMode{};
    uint32_t         modeBlobId{0};
    DrmFramebuffer   currentFb;
    DrmFramebuffer   previousFb; ///< Freigegeben nach nächstem Flip

    PlaneProps   videoProps{};
    PlaneProps   osdProps{};
    ModesetProps modesetProps{};

    // --- OSD ---
    mutable cMutex osdMutex;
    OsdBuffer currentOsd{};
    bool      osdDirty{false};

    // --- Synchronisation ---
    std::atomic<bool> isFlipPending{false};
    std::atomic<bool> isReady{false};
    std::atomic<bool> isStopping{false};
    std::atomic<bool> isClearing{false};

    drmEventContext eventContext{};

    // --- DRM Deleter-Typen ---
    struct FreeDrmResources    { auto operator()(drmModeRes       *p) const noexcept -> void { drmModeFreeResources(p);     } };
    struct FreeDrmPlaneRes     { auto operator()(drmModePlaneRes  *p) const noexcept -> void { drmModeFreePlaneResources(p);} };
    struct FreeDrmPlane        { auto operator()(drmModePlane     *p) const noexcept -> void { drmModeFreePlane(p);         } };
    struct FreeDrmObjProps     { auto operator()(drmModeObjectProperties*p) const noexcept -> void { drmModeFreeObjectProperties(p);} };
    struct FreeDrmProp         { auto operator()(drmModePropertyRes*p) const noexcept -> void { drmModeFreeProperty(p);     } };
    struct FreeDrmConnector    { auto operator()(drmModeConnector *p) const noexcept -> void { drmModeFreeConnector(p);     } };
    struct FreeDrmCrtc         { auto operator()(drmModeCrtc      *p) const noexcept -> void { drmModeFreeCrtc(p);          } };
};
