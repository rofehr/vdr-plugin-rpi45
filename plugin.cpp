// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 VDR-Plugin rpi5video
/**
 * @file plugin.cpp
 * @brief VDR-Plugin-Einstiegspunkt für rpi5video
 */

#include "common.h"
#include "config.h"
#include "device.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/log.h>
}
#pragma GCC diagnostic pop

#include <vdr/plugin.h>
#include <vdr/tools.h>

// ============================================================================
// === PLUGIN-KLASSE ===
// ============================================================================

class cPluginRpi5Video : public cPlugin {
public:
    cPluginRpi5Video() = default;
    ~cPluginRpi5Video() override = default;

    // --- Plugin-Metadaten ---
    [[nodiscard]] auto Version()     -> const char * override { return PLUGIN_VERSION.data(); }
    [[nodiscard]] auto Description() -> const char * override { return PLUGIN_DESC.data(); }
    [[nodiscard]] auto CommandLineHelp() -> const char * override;

    // --- Lebenszyklus ---
    [[nodiscard]] auto ProcessArgs(int argc, char *argv[]) -> bool override;
    [[nodiscard]] auto Initialize()  -> bool override;
    [[nodiscard]] auto Start()       -> bool override;
    auto Stop()                      -> void override;

    // --- Setup-Menü ---
    [[nodiscard]] auto SetupMenu()    -> cMenuSetupPage * override;
    [[nodiscard]] auto SetupParse(const char *Name, const char *Value) -> bool override;

private:
    cRpi5Device *device{nullptr};
};

// ============================================================================
// === KOMMANDOZEILE ===
// ============================================================================

[[nodiscard]] auto cPluginRpi5Video::CommandLineHelp() -> const char * {
    return
        "  -d DEV   DRM-Gerät (Standard: /dev/dri/card0)\n"
        "  -a DEV   ALSA-Gerät (Standard: default)\n"
        "  -r WxH@R Auflösung und Bildwiederholrate (z.B. 1920x1080@50)\n";
}

[[nodiscard]] auto cPluginRpi5Video::ProcessArgs(int argc, char *argv[]) -> bool {
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "-d" && i + 1 < argc) {
            rpi5Config.drmDevice = argv[++i];
        } else if (std::string_view(argv[i]) == "-a" && i + 1 < argc) {
            rpi5Config.audioDevice = argv[++i];
        } else if (std::string_view(argv[i]) == "-r" && i + 1 < argc) {
            unsigned w = 0, h = 0, r = 0;
            if (std::sscanf(argv[++i], "%ux%u@%u", &w, &h, &r) == 3 && w > 0 && h > 0 && r > 0) {
                rpi5Config.display.width       = w;
                rpi5Config.display.height      = h;
                rpi5Config.display.refreshRate = r;
            } else {
                esyslog("rpi5video: ungültiges Auflösungsformat '%s' (erwartet WxH@R)", argv[i]);
                return false;
            }
        }
    }
    return true;
}

// ============================================================================
// === LEBENSZYKLUS ===
// ============================================================================

[[nodiscard]] auto cPluginRpi5Video::Initialize() -> bool {
    isyslog("rpi5video: Plugin v%s wird initialisiert", PLUGIN_VERSION.data());

    // FFmpeg-Log auf VDR-Level umleiten
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback([](void *, int level, const char *fmt, va_list vl) {
        if (level > AV_LOG_WARNING) return;
        char buf[512];
        std::vsnprintf(buf, sizeof(buf), fmt, vl);
        // Zeilenumbruch entfernen
        for (char *p = buf; *p; ++p) if (*p == '\n') *p = '\0';
        if (level <= AV_LOG_ERROR)   esyslog("FFmpeg: %s", buf);
        else                          dsyslog("FFmpeg: %s", buf);
    });

    device = new cRpi5Device();
    return true;
}

[[nodiscard]] auto cPluginRpi5Video::Start() -> bool {
    isyslog("rpi5video: starte (DRM=%s, Audio=%s, %ux%u@%uHz)",
            rpi5Config.drmDevice.c_str(),
            rpi5Config.audioDevice.c_str(),
            rpi5Config.display.width,
            rpi5Config.display.height,
            rpi5Config.display.refreshRate);

    if (!device->Initialize(rpi5Config.drmDevice, rpi5Config.audioDevice)) {
        esyslog("rpi5video: Hardware-Initialisierung fehlgeschlagen");
        return false;
    }
    return true;
}

auto cPluginRpi5Video::Stop() -> void {
    isyslog("rpi5video: wird gestoppt");
    if (device) device->Detach();
}

// ============================================================================
// === SETUP-MENÜ ===
// ============================================================================

/**
 * @brief Einfaches VDR-Setup-Menü für Plugin-Einstellungen.
 */
class cRpi5SetupMenu : public cMenuSetupPage {
public:
    cRpi5SetupMenu() {
        SetTitle("RPi5 Video");
        Add(new cMenuEditIntItem("Audio-Latenz (ms)", &audioLatency, -500, 500));
        Add(new cMenuEditBoolItem("Deinterlacing",   &deinterlace));
        Add(new cMenuEditBoolItem("Rauschfilter",    &denoise));
        Add(new cMenuEditBoolItem("Schärfe-Filter",  &sharpness));
    }

protected:
    auto Store() -> void override {
        SetupStore("AudioLatency", audioLatency);
        SetupStore("Deinterlace",  deinterlace);
        SetupStore("Denoise",      denoise);
        SetupStore("Sharpness",    sharpness);
        rpi5Config.audioLatency = audioLatency;
        rpi5Config.deinterlace  = (deinterlace != 0);
        rpi5Config.denoise      = (denoise     != 0);
        rpi5Config.sharpness    = (sharpness   != 0);
    }

private:
    int audioLatency{rpi5Config.audioLatency};
    int deinterlace {rpi5Config.deinterlace  ? 1 : 0};
    int denoise     {rpi5Config.denoise      ? 1 : 0};
    int sharpness   {rpi5Config.sharpness    ? 1 : 0};
};

[[nodiscard]] auto cPluginRpi5Video::SetupMenu() -> cMenuSetupPage * {
    return new cRpi5SetupMenu();
}

[[nodiscard]] auto cPluginRpi5Video::SetupParse(const char *Name, const char *Value) -> bool {
    if      (std::strcmp(Name, "AudioLatency") == 0) rpi5Config.audioLatency = std::atoi(Value);
    else if (std::strcmp(Name, "Deinterlace")  == 0) rpi5Config.deinterlace  = std::atoi(Value) != 0;
    else if (std::strcmp(Name, "Denoise")      == 0) rpi5Config.denoise      = std::atoi(Value) != 0;
    else if (std::strcmp(Name, "Sharpness")    == 0) rpi5Config.sharpness    = std::atoi(Value) != 0;
    else return false;
    return true;
}

// ============================================================================
// === PLUGIN-REGISTRIERUNG ===
// ============================================================================

VDRPLUGINCREATOR(cPluginRpi5Video);
