#include "settings.h"
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
    std::puts("FG settings: legacy migration, backend exclusion, fixed Smooth Motion 2x and persisted DLSS multiplier passed");
    return 0;
}
