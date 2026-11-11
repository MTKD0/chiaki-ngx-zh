// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once
#include "settings.h"
#include <QStringList>

namespace EnhancementChoices {
inline bool spatialPreset(int value) { return value == 3 || value == 4; }
inline bool spatialScaler(int value) { return value >= int(PlaceboUpscaler::FSR) && value <= int(PlaceboUpscaler::FSRCNNX16); }
inline QString conflicts(const Settings &settings, const QString &choice, int value)
{
    if (!value) return {};
    QStringList result;
    const bool vsr = settings.GetEnhancementVsrQuality() > 0;
    if (choice == "vsr") {
        if (settings.GetEnhancementNeural())
            for (int pass = 0; pass < settings.GetEnhancementPassCount(); ++pass)
                if (settings.GetEnhancementEffectivePassConfig(pass).fsr) { result << "FSR (NR)"; break; }
        if (spatialPreset(int(settings.GetPlaceboPreset()))) result << "FSRCNNX / FSR";
        else if (settings.GetPlaceboPreset() == PlaceboPreset::Custom && spatialScaler(int(settings.GetPlaceboUpscaler())))
            result << (settings.GetPlaceboUpscaler() == PlaceboUpscaler::FSR ? "FSR" : "FSRCNNX");
    } else if (choice == "fsr" || (choice == "upscaler" && spatialScaler(value)) || (choice == "preset" && spatialPreset(value))) {
        if (vsr) result << "NVIDIA VSR";
        if (choice != "fsr" && settings.GetEnhancementNeural())
            for (int pass = 0; pass < settings.GetEnhancementPassCount(); ++pass)
                if (settings.GetEnhancementEffectivePassConfig(pass).fsr) { result << "FSR (NR)"; break; }
        if (spatialPreset(int(settings.GetPlaceboPreset())) && (choice != "preset" || value != int(settings.GetPlaceboPreset())))
            result << "FSRCNNX / FSR";
        if (settings.GetPlaceboPreset() == PlaceboPreset::Custom && spatialScaler(int(settings.GetPlaceboUpscaler())) &&
            (choice != "upscaler" || value != int(settings.GetPlaceboUpscaler())))
            result << (settings.GetPlaceboUpscaler() == PlaceboUpscaler::FSR ? "FSR" : "FSRCNNX");
    } else if (choice == "fgBackend" && settings.GetEnhancementFrameGenerationEnabled() && value != settings.GetEnhancementFrameGenerationBackend()) {
        result << (settings.GetEnhancementFrameGenerationBackend() == 1 ? "DLSS FG" : "smooth motion");
    }
    return result.join(" / ");
}
// Call only after the UI confirms a conflict. Unrelated NR, color and FG settings survive.
inline void apply(Settings &settings, const QString &choice, int value, int editingPass = 0)
{
    if (choice == "vsr" && value) {
        for (int pass = 0; pass < 3; ++pass) settings.SetEnhancementPassFsr(pass, false);
        if (spatialScaler(int(settings.GetPlaceboUpscaler()))) settings.SetPlaceboUpscaler(PlaceboUpscaler::Bilinear);
        if (spatialPreset(int(settings.GetPlaceboPreset()))) settings.SetPlaceboPreset(PlaceboPreset::HighQuality);
    } else if (value && (choice == "fsr" || (choice == "upscaler" && spatialScaler(value)) || (choice == "preset" && spatialPreset(value)))) {
        settings.SetEnhancementVsrQuality(0);
        if (choice == "fsr") {
            if (spatialPreset(int(settings.GetPlaceboPreset()))) settings.SetPlaceboPreset(PlaceboPreset::HighQuality);
            if (spatialScaler(int(settings.GetPlaceboUpscaler()))) settings.SetPlaceboUpscaler(PlaceboUpscaler::Bilinear);
        } else {
            for (int pass = 0; pass < 3; ++pass) settings.SetEnhancementPassFsr(pass, false);
            if (choice == "upscaler") settings.SetPlaceboPreset(PlaceboPreset::Custom);
        }
    }
    if (choice == "vsr") settings.SetEnhancementVsrQuality(value);
    else if (choice == "fsr") settings.SetEnhancementPassFsr(editingPass, bool(value));
    else if (choice == "fgBackend") settings.SetEnhancementFrameGenerationBackend(value);
    else if (choice == "upscaler" && value >= 0 && value <= int(PlaceboUpscaler::FSRCNNX16)) settings.SetPlaceboUpscaler(static_cast<PlaceboUpscaler>(value));
    else if (choice == "preset" && value >= 0 && value <= int(PlaceboPreset::Custom)) settings.SetPlaceboPreset(static_cast<PlaceboPreset>(value));
}
}
