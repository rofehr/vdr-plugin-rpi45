// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file display.cpp
 * @brief DRM-Atomic-Modesetting und Page-Flip-Ausgabe für RPi5
 *
 * Kernunterschied zu VAAPI:
 *  - MapDrmPrimeFrame() benötigt kein av_hwframe_map():
 *    v4l2_request liefert AVDRMFrameDescriptor direkt im avFrame->data[0].
 *  - NV12-Modifier: bei RPi5/V3D wird DRM_FORMAT_MOD_LINEAR erwartet,
 *    da der V4L2-M2M-Decoder keine komprimierten Tiling-Modi exportiert.
 */

#include "display.h"
#include "common.h"
#include "config.h"

#include <cerrno>
#include <cstring>
#include <memory>
#include <sys/poll.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>
}
#pragma GCC diagnostic pop

#include <libdrm/drm.h>
#include <libdrm/drm_fourcc.h>
#include <libdrm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <vdr/thread.h>
#include <vdr/tools.h>

// ============================================================================
// === KONSTANTEN ===
// ============================================================================

constexpr int DISPLAY_PAGE_FLIP_TIMEOUT_MS = 40;
constexpr int MAX_DRAIN_ITERATIONS         = 10;

// ============================================================================
// === ATOMIC REQUEST ===
// ============================================================================

AtomicRequest::AtomicRequest() : request(drmModeAtomicAlloc()) {}
AtomicRequest::~AtomicRequest() noexcept { if (request) drmModeAtomicFree(request); }

AtomicRequest::AtomicRequest(AtomicRequest &&other) noexcept
    : propCount(other.propCount), request(other.request) {
    other.request = nullptr; other.propCount = 0;
}
auto AtomicRequest::operator=(AtomicRequest &&other) noexcept -> AtomicRequest & {
    if (this != &other) {
        if (request) drmModeAtomicFree(request);
        request = other.request; propCount = other.propCount;
        other.request = nullptr; other.propCount = 0;
    }
    return *this;
}

auto AtomicRequest::AddProperty(uint32_t objId, uint32_t propId, uint64_t value) -> void {
    if (!request || propId == 0) return;
    if (drmModeAtomicAddProperty(request, objId, propId, value) >= 0) ++propCount;
}
[[nodiscard]] auto AtomicRequest::Count()  const noexcept -> int              { return propCount; }
[[nodiscard]] auto AtomicRequest::Handle() const noexcept -> drmModeAtomicReq *{ return request; }

// ============================================================================
// === DRM-FRAMEBUFFER ===
// ============================================================================

cRpi5Display::DrmFramebuffer::DrmFramebuffer(DrmFramebuffer &&other) noexcept
    : drmFd(other.drmFd), fbId(other.fbId), gemHandle(other.gemHandle),
      width(other.width), height(other.height), modifier(other.modifier), frame(other.frame) {
    other.drmFd = -1; other.fbId = 0; other.gemHandle = 0; other.frame = nullptr;
}

cRpi5Display::DrmFramebuffer::~DrmFramebuffer() noexcept {
    if (frame)               av_frame_free(&frame);
    if (fbId != 0 && drmFd >= 0) drmModeRmFB(drmFd, fbId);
    if (gemHandle != 0 && drmFd >= 0) {
        drm_gem_close closeArgs{.handle = gemHandle, .pad = 0};
        drmIoctl(drmFd, DRM_IOCTL_GEM_CLOSE, &closeArgs);
    }
}

auto cRpi5Display::DrmFramebuffer::operator=(DrmFramebuffer &&other) noexcept -> DrmFramebuffer & {
    if (this != &other) {
        if (frame)               av_frame_free(&frame);
        if (fbId && drmFd >= 0)  drmModeRmFB(drmFd, fbId);
        if (gemHandle && drmFd >= 0) {
            drm_gem_close ca{.handle = gemHandle, .pad = 0};
            drmIoctl(drmFd, DRM_IOCTL_GEM_CLOSE, &ca);
        }
        drmFd = other.drmFd; fbId = other.fbId; gemHandle = other.gemHandle;
        width = other.width; height = other.height; modifier = other.modifier;
        frame = other.frame;
        other.drmFd = -1; other.fbId = 0; other.gemHandle = 0; other.frame = nullptr;
    }
    return *this;
}

// ============================================================================
// === DISPLAY THREAD ===
// ============================================================================

cRpi5Display::DisplayThread::DisplayThread(cRpi5Display *owner)
    : cThread("rpi5video/display"), display(owner) {}

auto cRpi5Display::DisplayThread::Stop() -> void {
    stopping.store(true, std::memory_order_release);
    Cancel(2);
}

auto cRpi5Display::DisplayThread::Action() -> void {
    isyslog("rpi5video/display: Thread gestartet");
    while (!stopping.load(std::memory_order_acquire)) {
        std::unique_ptr<Rpi5Frame> frame;
        {
            const cMutexLock lock(&display->frameMutex);
            if (!display->pendingFrame) {
                display->frameCondition.TimedWait(display->frameMutex, 20);
                if (!display->pendingFrame) continue;
            }
            frame = std::move(display->pendingFrame);
        }

        auto fb = display->MapDrmPrimeFrame(std::move(frame));
        if (!fb.IsValid()) {
            dsyslog("rpi5video/display: Frame-Mapping fehlgeschlagen");
            continue;
        }

        display->WaitForPageFlip(DISPLAY_PAGE_FLIP_TIMEOUT_MS);

        if (display->PresentBuffer(fb)) {
            display->previousFb = std::move(display->currentFb);
            display->currentFb  = std::move(fb);
        }
    }
    isyslog("rpi5video/display: Thread beendet");
}

// ============================================================================
// === DISPLAY ===
// ============================================================================

cRpi5Display::cRpi5Display()
    : eventContext{.version            = DRM_EVENT_CONTEXT_VERSION,
                   .vblank_handler     = nullptr,
                   .page_flip_handler  = OnPageFlipEvent,
                   .page_flip_handler2 = nullptr,
                   .sequence_handler   = nullptr} {
    dsyslog("rpi5video/display: erstellt");
}

cRpi5Display::~cRpi5Display() noexcept {
    Shutdown();
    if (hwDeviceRef) av_buffer_unref(&hwDeviceRef);
}

[[nodiscard]] auto cRpi5Display::Initialize(
    int drmFd_, AVBufferRef *hwDeviceRef_,
    uint32_t crtcId_, uint32_t connectorId_,
    const drmModeModeInfo &mode) -> bool
{
    drmFd       = drmFd_;
    hwDeviceRef = av_buffer_ref(hwDeviceRef_);
    crtcId      = crtcId_;
    connectorId = connectorId_;
    activeMode  = mode;
    outputWidth  = mode.hdisplay;
    outputHeight = mode.vdisplay;

    if (!DiscoverPlanes())    { esyslog("rpi5video/display: Planes nicht gefunden"); return false; }
    if (!DiscoverProperties()){ esyslog("rpi5video/display: Properties nicht gefunden"); return false; }
    if (!PerformModeset())    { esyslog("rpi5video/display: Modeset fehlgeschlagen"); return false; }

    displayThread = std::make_unique<DisplayThread>(this);
    displayThread->Start();

    isReady.store(true, std::memory_order_release);
    isyslog("rpi5video/display: initialisiert %ux%u@%uHz",
            outputWidth, outputHeight, activeMode.vrefresh);
    return true;
}

auto cRpi5Display::Shutdown() -> void {
    isStopping.store(true, std::memory_order_release);

    if (displayThread) {
        displayThread->Stop();
        displayThread.reset();
    }

    WaitForPageFlip(DISPLAY_PAGE_FLIP_TIMEOUT_MS * 2);
    previousFb = {};
    currentFb  = {};

    if (modeBlobId != 0 && drmFd >= 0) {
        drmModeDestroyPropertyBlob(drmFd, modeBlobId);
        modeBlobId = 0;
    }
    isReady.store(false, std::memory_order_release);
    isStopping.store(false, std::memory_order_release);
}

auto cRpi5Display::BeginStreamSwitch() -> void { isClearing.store(true, std::memory_order_release); }
auto cRpi5Display::EndStreamSwitch()   -> void { isClearing.store(false, std::memory_order_release); }

[[nodiscard]] auto cRpi5Display::SubmitFrame(std::unique_ptr<Rpi5Frame> frame, int timeoutMs) -> bool {
    if (!isReady.load(std::memory_order_acquire)) return false;
    const cMutexLock lock(&frameMutex);
    pendingFrame = std::move(frame);
    frameCondition.Broadcast();
    (void)timeoutMs;
    return true;
}

[[nodiscard]] auto cRpi5Display::IsInitialized()  const noexcept -> bool    { return isReady.load(std::memory_order_acquire); }
[[nodiscard]] auto cRpi5Display::GetOutputWidth()  const noexcept -> uint32_t{ return outputWidth;  }
[[nodiscard]] auto cRpi5Display::GetOutputHeight() const noexcept -> uint32_t{ return outputHeight; }
[[nodiscard]] auto cRpi5Display::GetAspectRatio()  const noexcept -> double  {
    return (outputHeight > 0) ? static_cast<double>(outputWidth) / outputHeight : 1.0;
}

auto cRpi5Display::UpdateOsd(const OsdBuffer &osd) -> void {
    const cMutexLock lock(&osdMutex);
    currentOsd = osd;
    osdDirty   = true;
}

// ============================================================================
// === DRM-PRIME FRAME MAPPING ===
// ============================================================================

[[nodiscard]] auto cRpi5Display::MapDrmPrimeFrame(std::unique_ptr<Rpi5Frame> vaapiFrame) const
    -> DrmFramebuffer
{
    if (!vaapiFrame || !vaapiFrame->avFrame) [[unlikely]] return {};

    const AVFrame *srcFrame = vaapiFrame->avFrame;

    // V4L2-Request: Frame ist bereits AV_PIX_FMT_DRM_PRIME
    // Kein av_hwframe_map() nötig – AVDRMFrameDescriptor direkt auslesen
    const AVDRMFrameDescriptor *desc = nullptr;

    if (srcFrame->format == AV_PIX_FMT_DRM_PRIME) {
        // Direkt: DRM-PRIME aus v4l2_request
        desc = reinterpret_cast<const AVDRMFrameDescriptor *>( // NOLINT
            srcFrame->data[0]);
    } else {
        esyslog("rpi5video/display: unerwartetes Pixelformat %d", srcFrame->format);
        return {};
    }

    if (!desc || desc->nb_objects == 0 || desc->nb_layers == 0) [[unlikely]] return {};

    const int primeFd = desc->objects[0].fd;
    if (primeFd < 0) [[unlikely]] {
        esyslog("rpi5video/display: ungültiger PRIME-FD %d", primeFd);
        return {};
    }

    uint32_t gemHandle = 0;
    if (drmPrimeFDToHandle(drmFd, primeFd, &gemHandle) != 0) [[unlikely]] {
        esyslog("rpi5video/display: drmPrimeFDToHandle fehlgeschlagen: %s", strerror(errno));
        return {};
    }

    const uint32_t format = DRM_FORMAT_NV12;
    const auto width  = static_cast<uint32_t>(srcFrame->width);
    const auto height = static_cast<uint32_t>(srcFrame->height);

    uint32_t handles[4]  = {0};
    uint32_t pitches[4]  = {0};
    uint32_t offsets[4]  = {0};
    uint64_t modifiers[4]= {0};

    int planeIdx = 0;
    for (int i = 0; i < desc->nb_layers && planeIdx < 4; ++i) {
        for (int j = 0; j < desc->layers[i].nb_planes && planeIdx < 4; ++j) {
            const auto &plane = desc->layers[i].planes[j];
            handles[planeIdx]   = gemHandle;
            pitches[planeIdx]   = static_cast<uint32_t>(plane.pitch);
            offsets[planeIdx]   = static_cast<uint32_t>(plane.offset);
            // RPi5/V4L2-Request: kein Tiling → LINEAR Modifier
            modifiers[planeIdx] = desc->objects[plane.object_index].format_modifier;
            if (modifiers[planeIdx] == DRM_FORMAT_MOD_INVALID)
                modifiers[planeIdx] = DRM_FORMAT_MOD_LINEAR;
            ++planeIdx;
        }
    }

    if (planeIdx != 2) [[unlikely]] {
        esyslog("rpi5video/display: unerwartete Plane-Anzahl %d (erwartet 2 für NV12)", planeIdx);
        drm_gem_close ca{.handle = gemHandle, .pad = 0};
        drmIoctl(drmFd, DRM_IOCTL_GEM_CLOSE, &ca);
        return {};
    }

    uint32_t fbId = 0;
    if (drmModeAddFB2WithModifiers(drmFd, width, height, format,
                                   handles, pitches, offsets, modifiers,
                                   &fbId, DRM_MODE_FB_MODIFIERS) != 0) {
        esyslog("rpi5video/display: drmModeAddFB2WithModifiers fehlgeschlagen: %s", strerror(errno));
        drm_gem_close ca{.handle = gemHandle, .pad = 0};
        drmIoctl(drmFd, DRM_IOCTL_GEM_CLOSE, &ca);
        return {};
    }

    DrmFramebuffer fb;
    fb.drmFd     = drmFd;
    fb.fbId      = fbId;
    fb.gemHandle = gemHandle;
    fb.width     = width;
    fb.height    = height;
    fb.modifier  = modifiers[0];
    // AVFrame übernehmen – hält DMA-BUF-FD am Leben bis nach dem nächsten Flip
    fb.frame              = vaapiFrame->avFrame;
    vaapiFrame->avFrame   = nullptr;
    vaapiFrame->ownsFrame = false;
    return fb;
}

// ============================================================================
// === PAGE FLIP ===
// ============================================================================

[[nodiscard]] auto cRpi5Display::PresentBuffer(const DrmFramebuffer &fb) -> bool {
    if (!fb.IsValid()) return false;

    const uint32_t destX = (fb.width  < outputWidth)  ? (outputWidth  - fb.width)  / 2 : 0;
    const uint32_t destY = (fb.height < outputHeight)  ? (outputHeight - fb.height) / 2 : 0;

    AtomicRequest req;
    req.AddProperty(videoPlaneId, videoProps.crtcId,  crtcId);
    req.AddProperty(videoPlaneId, videoProps.fbId,    fb.fbId);
    if (videoProps.colorEncodingValid)
        req.AddProperty(videoPlaneId, videoProps.colorEncoding, videoProps.colorEncodingBt709);
    if (videoProps.colorRangeValid)
        req.AddProperty(videoPlaneId, videoProps.colorRange, videoProps.colorRangeLimited);
    req.AddProperty(videoPlaneId, videoProps.srcX, 0);
    req.AddProperty(videoPlaneId, videoProps.srcY, 0);
    req.AddProperty(videoPlaneId, videoProps.srcW, static_cast<uint64_t>(fb.width)  << 16);
    req.AddProperty(videoPlaneId, videoProps.srcH, static_cast<uint64_t>(fb.height) << 16);
    req.AddProperty(videoPlaneId, videoProps.crtcX, destX);
    req.AddProperty(videoPlaneId, videoProps.crtcY, destY);
    req.AddProperty(videoPlaneId, videoProps.crtcW, fb.width);
    req.AddProperty(videoPlaneId, videoProps.crtcH, fb.height);

    bool osdCommitted = false;
    {
        const cMutexLock lock(&osdMutex);
        if (osdDirty) {
            if (currentOsd.fbId != 0) {
                AppendOsdPlane(req, currentOsd);
                osdCommitted = true;
            } else if (osdPlaneId != 0) {
                req.AddProperty(osdPlaneId, osdProps.fbId,   0);
                req.AddProperty(osdPlaneId, osdProps.crtcId, 0);
                osdCommitted = true;
            }
        }
    }

    const bool success = AtomicCommit(req, 0);
    if (success && osdCommitted) {
        const cMutexLock lock(&osdMutex);
        osdDirty = false;
    }
    return success;
}

[[nodiscard]] auto cRpi5Display::AtomicCommit(AtomicRequest &req, uint32_t flags) -> bool {
    if (!req.Handle() || req.Count() == 0) return false;
    isFlipPending.store(true, std::memory_order_release);
    const int ret = drmModeAtomicCommit(
        drmFd, req.Handle(),
        DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK | flags,
        this);
    if (ret != 0) {
        isFlipPending.store(false, std::memory_order_release);
        if (!isStopping.load(std::memory_order_relaxed))
            dsyslog("rpi5video/display: drmModeAtomicCommit fehlgeschlagen: %s", strerror(errno));
        return false;
    }
    return true;
}

auto cRpi5Display::AppendOsdPlane(AtomicRequest &req, const OsdBuffer &osd) -> void {
    if (osdPlaneId == 0 || osd.fbId == 0) return;
    req.AddProperty(osdPlaneId, osdProps.crtcId, crtcId);
    req.AddProperty(osdPlaneId, osdProps.fbId,   osd.fbId);
    req.AddProperty(osdPlaneId, osdProps.srcX, 0);
    req.AddProperty(osdPlaneId, osdProps.srcY, 0);
    req.AddProperty(osdPlaneId, osdProps.srcW, static_cast<uint64_t>(osd.width)  << 16);
    req.AddProperty(osdPlaneId, osdProps.srcH, static_cast<uint64_t>(osd.height) << 16);
    req.AddProperty(osdPlaneId, osdProps.crtcX, 0);
    req.AddProperty(osdPlaneId, osdProps.crtcY, 0);
    req.AddProperty(osdPlaneId, osdProps.crtcW, outputWidth);
    req.AddProperty(osdPlaneId, osdProps.crtcH, outputHeight);
}

auto cRpi5Display::WaitForPageFlip(int timeoutMs) -> void {
    const cTimeMs deadline(timeoutMs);
    while (isFlipPending.load(std::memory_order_acquire) && !deadline.TimedOut()) {
        if (isStopping.load(std::memory_order_acquire) ||
            !isReady.load(std::memory_order_acquire)   ||
            isClearing.load(std::memory_order_acquire)) break;
        (void)DrainDrmEvents(5);
    }
}

[[nodiscard]] auto cRpi5Display::DrainDrmEvents(int timeoutMs) -> bool {
    struct pollfd pfd{.fd = drmFd, .events = POLLIN, .revents = 0};
    const int ready = poll(&pfd, 1, timeoutMs);
    if (ready <= 0 || !(pfd.revents & POLLIN)) return false;
    drmHandleEvent(drmFd, &eventContext);
    return true;
}

auto cRpi5Display::OnPageFlipEvent([[maybe_unused]] int fd,
                                    [[maybe_unused]] unsigned seq,
                                    [[maybe_unused]] unsigned sec,
                                    [[maybe_unused]] unsigned usec,
                                    void *data) -> void {
    if (auto *d = static_cast<cRpi5Display *>(data))
        d->isFlipPending.store(false, std::memory_order_release);
}

// ============================================================================
// === DRM-PLANE ERKENNUNG ===
// ============================================================================

[[nodiscard]] auto cRpi5Display::DiscoverPlanes() -> bool {
    std::unique_ptr<drmModePlaneRes, FreeDrmPlaneRes> planeRes{drmModeGetPlaneResources(drmFd)};
    if (!planeRes) { esyslog("rpi5video/display: drmModeGetPlaneResources fehlgeschlagen"); return false; }

    for (uint32_t i = 0; i < planeRes->count_planes; ++i) {
        std::unique_ptr<drmModePlane, FreeDrmPlane> plane{drmModeGetPlane(drmFd, planeRes->planes[i])};
        if (!plane) continue;

        // Nur Planes auf unserem CRTC
        if (!(plane->possible_crtcs & (1u << /* crtc_index */ 0))) continue;

        std::unique_ptr<drmModeObjectProperties, FreeDrmObjProps> props{
            drmModeObjectGetProperties(drmFd, plane->plane_id, DRM_MODE_OBJECT_PLANE)};
        if (!props) continue;

        uint32_t planeType = 0;
        for (uint32_t j = 0; j < props->count_props; ++j) {
            std::unique_ptr<drmModePropertyRes, FreeDrmProp> prop{
                drmModeGetProperty(drmFd, props->props[j])};
            if (prop && std::strcmp(prop->name, "type") == 0)
                planeType = static_cast<uint32_t>(props->prop_values[j]);
        }

        if (planeType == DRM_PLANE_TYPE_PRIMARY && videoPlaneId == 0)
            videoPlaneId = plane->plane_id;
        else if (planeType == DRM_PLANE_TYPE_OVERLAY && osdPlaneId == 0)
            osdPlaneId = plane->plane_id;
    }

    if (videoPlaneId == 0) {
        esyslog("rpi5video/display: kein Primary-Plane gefunden");
        return false;
    }
    dsyslog("rpi5video/display: Video-Plane=%u OSD-Plane=%u", videoPlaneId, osdPlaneId);
    return true;
}

[[nodiscard]] auto cRpi5Display::DiscoverProperties() -> bool {
    // Hilfslambda: sucht eine Property ID nach Name
    const auto findProp = [&](uint32_t objId, uint32_t objType,
                               const char *name) -> uint32_t {
        std::unique_ptr<drmModeObjectProperties, FreeDrmObjProps> props{
            drmModeObjectGetProperties(drmFd, objId, objType)};
        if (!props) return 0;
        for (uint32_t i = 0; i < props->count_props; ++i) {
            std::unique_ptr<drmModePropertyRes, FreeDrmProp> prop{
                drmModeGetProperty(drmFd, props->props[i])};
            if (prop && std::strcmp(prop->name, name) == 0)
                return prop->prop_id;
        }
        return 0;
    };

    // Video-Plane Properties
    videoProps.crtcId  = findProp(videoPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    videoProps.fbId    = findProp(videoPlaneId, DRM_MODE_OBJECT_PLANE, "FB_ID");
    videoProps.srcX    = findProp(videoPlaneId, DRM_MODE_OBJECT_PLANE, "SRC_X");
    videoProps.srcY    = findProp(videoPlaneId, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    videoProps.srcW    = findProp(videoPlaneId, DRM_MODE_OBJECT_PLANE, "SRC_W");
    videoProps.srcH    = findProp(videoPlaneId, DRM_MODE_OBJECT_PLANE, "SRC_H");
    videoProps.crtcX   = findProp(videoPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    videoProps.crtcY   = findProp(videoPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    videoProps.crtcW   = findProp(videoPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    videoProps.crtcH   = findProp(videoPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_H");

    // Farbraum-Properties (optional, ignoriert wenn nicht vorhanden)
    videoProps.colorEncoding = findProp(videoPlaneId, DRM_MODE_OBJECT_PLANE, "COLOR_ENCODING");
    videoProps.colorRange    = findProp(videoPlaneId, DRM_MODE_OBJECT_PLANE, "COLOR_RANGE");
    videoProps.colorEncodingValid = (videoProps.colorEncoding != 0);
    videoProps.colorRangeValid    = (videoProps.colorRange != 0);

    // CRTC/Connector Properties für Modeset
    modesetProps.crtcActive      = findProp(crtcId,      DRM_MODE_OBJECT_CRTC,      "ACTIVE");
    modesetProps.crtcModeId      = findProp(crtcId,      DRM_MODE_OBJECT_CRTC,      "MODE_ID");
    modesetProps.connectorCrtcId = findProp(connectorId, DRM_MODE_OBJECT_CONNECTOR,  "CRTC_ID");
    modesetProps.isValid = (modesetProps.crtcActive && modesetProps.crtcModeId && modesetProps.connectorCrtcId);

    if (!modesetProps.isValid) {
        esyslog("rpi5video/display: Modeset-Properties unvollständig");
        return false;
    }

    // OSD-Plane Properties (nur wenn OSD-Plane vorhanden)
    if (osdPlaneId != 0) {
        osdProps.crtcId = findProp(osdPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
        osdProps.fbId   = findProp(osdPlaneId, DRM_MODE_OBJECT_PLANE, "FB_ID");
        osdProps.srcX   = findProp(osdPlaneId, DRM_MODE_OBJECT_PLANE, "SRC_X");
        osdProps.srcY   = findProp(osdPlaneId, DRM_MODE_OBJECT_PLANE, "SRC_Y");
        osdProps.srcW   = findProp(osdPlaneId, DRM_MODE_OBJECT_PLANE, "SRC_W");
        osdProps.srcH   = findProp(osdPlaneId, DRM_MODE_OBJECT_PLANE, "SRC_H");
        osdProps.crtcX  = findProp(osdPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_X");
        osdProps.crtcY  = findProp(osdPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
        osdProps.crtcW  = findProp(osdPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_W");
        osdProps.crtcH  = findProp(osdPlaneId, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    }
    return true;
}

[[nodiscard]] auto cRpi5Display::PerformModeset() -> bool {
    if (drmModeCreatePropertyBlob(drmFd, &activeMode, sizeof(activeMode), &modeBlobId) != 0) {
        esyslog("rpi5video/display: Modus-Blob erstellen fehlgeschlagen");
        return false;
    }

    AtomicRequest req;
    req.AddProperty(connectorId, modesetProps.connectorCrtcId, crtcId);
    req.AddProperty(crtcId,      modesetProps.crtcActive,      1);
    req.AddProperty(crtcId,      modesetProps.crtcModeId,      modeBlobId);

    const bool ok = AtomicCommit(req, DRM_MODE_ATOMIC_ALLOW_MODESET);
    if (!ok) esyslog("rpi5video/display: Modeset fehlgeschlagen");
    else     isyslog("rpi5video/display: Modeset %ux%u@%uHz erfolgreich",
                     activeMode.hdisplay, activeMode.vdisplay, activeMode.vrefresh);
    return ok;
}
