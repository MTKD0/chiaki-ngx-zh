#include "settings.h"
#include "enhancementchoices.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FG settings failure: %s\n", #x); return 1; } } while (0)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName("ChiakiFgSettingsTest");
    app.setApplicationName("Routing");
    QTemporaryDir directory;
    CHECK(directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    Settings settings(QString{});
    CHECK(!settings.GetLowLatencyPresent()); // Existing and fresh profiles are opt-in.
    settings.SetLowLatencyPresent(true);
    QSettings().sync();
    { Settings reopened(QString{}); CHECK(reopened.GetLowLatencyPresent()); }
    settings.SetLowLatencyPresent(false);
    QSettings().sync();
    { Settings reopened(QString{}); CHECK(!reopened.GetLowLatencyPresent()); }
    CHECK(!settings.GetEnhancementFrameGenerationEnabled());
    CHECK(settings.GetEnhancementFrameGenerationBackend() == 1);
    CHECK(settings.GetEnhancementFrameGenerationMultiplier() == 2);
    // Existing installations retain their old 2x DLSS checkbox setting.
    settings.SetEnhancementFrameGeneration(true);
    CHECK(settings.GetEnhancementFrameGeneration());
    settings.SetEnhancementFrameGenerationMultiplier(4);
    settings.SetEnhancementFrameGenerationBackend(2);
    CHECK(settings.GetEnhancementFrameGenerationEnabled());
    CHECK(!settings.GetEnhancementFrameGeneration()); // No in-app FG in Smooth Motion mode.
    CHECK(settings.GetEnhancementFrameGenerationMultiplier() == 2);
    settings.SetEnhancementFrameGenerationMultiplier(3); // Cannot give Smooth Motion extra phases.
    QSettings().sync();
    Settings reopened(QString{});
    CHECK(reopened.GetEnhancementFrameGenerationBackend() == 2);
    CHECK(reopened.GetEnhancementFrameGenerationMultiplier() == 2);
    CHECK(!reopened.GetEnhancementFrameGeneration());
    reopened.SetEnhancementFrameGenerationBackend(1);
    CHECK(reopened.GetEnhancementFrameGenerationMultiplier() == 4); // Remember DLSS choice.
    CHECK(reopened.GetEnhancementFrameGeneration());
    reopened.SetEnhancementFrameGeneration(false);
    CHECK(!reopened.GetEnhancementFrameGeneration());
    reopened.SetEnhancementFrameGenerationMultiplier(0);
    CHECK(reopened.GetEnhancementFrameGenerationMultiplier() == 2);
    reopened.SetEnhancementFrameGenerationMultiplier(99);
    CHECK(reopened.GetEnhancementFrameGenerationMultiplier() == 4);
    reopened.SetEnhancementNeural(true);
    reopened.SetEnhancementPassFsr(0, true);
    reopened.SetPlaceboPreset(PlaceboPreset::HighQualityAdvancedSpatial);
    CHECK(EnhancementChoices::conflicts(reopened, "vsr", 3).contains("FSR"));
    CHECK(reopened.GetEnhancementVsrQuality() == 0); // Query/cancel must not mutate.
    CHECK(reopened.GetEnhancementPassFsr(0));
    EnhancementChoices::apply(reopened, "vsr", 3);
    CHECK(reopened.GetEnhancementNeural()); // NR has a different purpose and survives.
    CHECK(reopened.GetEnhancementVsrQuality() == 3);
    CHECK(!reopened.GetEnhancementPassFsr(0));
    CHECK(reopened.GetPlaceboPreset() == PlaceboPreset::HighQuality);
    CHECK(EnhancementChoices::conflicts(reopened, "fsr", 1) == "NVIDIA VSR");
    CHECK(EnhancementChoices::conflicts(reopened, "upscaler", int(PlaceboUpscaler::FSRCNNX16)) == "NVIDIA VSR");
    CHECK(EnhancementChoices::conflicts(reopened, "preset", 3) == "NVIDIA VSR");
    CHECK(EnhancementChoices::conflicts(reopened, "upscaler", int(PlaceboUpscaler::Bilinear)).isEmpty());
    EnhancementChoices::apply(reopened, "fsr", 1);
    CHECK(reopened.GetEnhancementVsrQuality() == 0 && reopened.GetEnhancementPassFsr(0));
    CHECK(EnhancementChoices::conflicts(reopened, "upscaler", int(PlaceboUpscaler::FSRCNNX16)).contains("FSR (NR)"));
    EnhancementChoices::apply(reopened, "upscaler", int(PlaceboUpscaler::FSRCNNX16));
    CHECK(reopened.GetPlaceboPreset() == PlaceboPreset::Custom && reopened.GetEnhancementNeural());
    CHECK(!reopened.GetEnhancementPassFsr(0));
    CHECK(EnhancementChoices::conflicts(reopened, "upscaler", int(PlaceboUpscaler::FSR)) == "FSRCNNX");
    reopened.SetEnhancementFrameGeneration(true);
    CHECK(EnhancementChoices::conflicts(reopened, "fgBackend", 2) == "DLSS FG");
    EnhancementChoices::apply(reopened, "fgBackend", 2);
    CHECK(!reopened.GetEnhancementFrameGeneration());
    CHECK(reopened.GetEnhancementFrameGenerationMultiplier() == 2);
    CHECK(EnhancementChoices::conflicts(reopened, "fgBackend", 3) == "smooth motion");
    CHECK(reopened.GetEnhancementFrameGenerationBackend() == 2); // Cancel preserves Smooth Motion.
    EnhancementChoices::apply(reopened, "fgBackend", 3);
    CHECK(reopened.GetEnhancementFrameGeneration());
    CHECK(reopened.GetEnhancementFrameGenerationMultiplier() == 4);
    for (int multiplier = 2; multiplier <= 4; ++multiplier) {
        reopened.SetEnhancementFrameGenerationMultiplier(multiplier);
        CHECK(reopened.GetEnhancementFrameGenerationMultiplier() == multiplier);
    }
    CHECK(EnhancementChoices::conflicts(reopened, "fgBackend", 1) == "AFG");
    CHECK(EnhancementChoices::conflicts(reopened, "fgBackend", 3).isEmpty());
    QSettings().sync();
    Settings afgReopened(QString{});
    CHECK(afgReopened.GetEnhancementFrameGenerationBackend() == 3);
    CHECK(afgReopened.GetEnhancementFrameGenerationMultiplier() == 4);
    CHECK(afgReopened.GetEnhancementFrameGeneration());
    afgReopened.SetEnhancementFrameGeneration(false);
    CHECK(!afgReopened.GetEnhancementFrameGeneration());
    std::puts("AFG persistence, 2x/3x/4x, cancellation, FG migration, backend exclusion, VSR overlap detection/cancellation/replacement and unrelated NR preservation passed");
    return 0;
}
