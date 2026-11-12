// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once
#include "metrics.h"
#include "raw_video.h"
#include <QString>
#include <QStringList>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace videolab
{
struct LabOptions
{
    QString mode = "quality", input, ffmpeg = "ffmpeg", ffprobe = "ffprobe";
    QString outDir, record, label, history;
    QStringList encoder;
    int width = 0, height = 0;
    double fps = 0, start = 0, duration = 0;
    unsigned multiplier = 2, decimate = 0;
    bool frameGeneration = true, neural = false, fsr = true, vsync = false;
    int percent = 65, sharpness = 0, maxRealFrames = 0, worst = 8;
    int windowWidth = 1280, windowHeight = 720;
};

// A frame generator under test. One real frame goes in; its generated phases (1-based) and then
// the real frame (phase 0) come back in display order, followed by frameDone.
// Frames are tightly packed RGBA8 and valid only during the callback.
class Backend
{
  public:
    virtual ~Backend() {}
    std::function<void()> ready;
    std::function<void(uint64_t frameId, unsigned phase, const uint8_t *rgba)> output;
    std::function<void(uint64_t frameId)> frameDone;
    std::function<void(const QString &message)> failed;
    std::function<void(const QString &message)> status;
    virtual QString name() const = 0;
    virtual void start() = 0;
    virtual bool submit(uint64_t frameId, uint64_t timestampUs, const uint8_t *rgba) = 0;
    virtual void stop() {}
    Series fgMs, flowMs, nrMs; // GPU stage times reported while running, when the backend has them
};

// Linear cross-fade, on the CPU. It is the floor any motion-compensated generator has to beat,
// and it runs the whole lab without an RTX GPU.
class BlendBackend : public Backend
{
  public:
    BlendBackend(int width, int height, unsigned multiplier)
        : pixels(size_t(width) * height), multiplier(multiplier), scratch(pixels * 4)
    {
    }
    QString name() const override { return QStringLiteral("cpu-blend"); }
    void start() override
    {
        QTimer::singleShot(0, &context, [this] { if (ready) ready(); });
    }
    bool submit(uint64_t frameId, uint64_t, const uint8_t *rgba) override
    {
        if (busy)
            return false;
        busy = true;
        current.assign(rgba, rgba + pixels * 4);
        QTimer::singleShot(0, &context, [this, frameId] {
            if (!previous.empty())
                for (unsigned phase = 1; phase < multiplier; ++phase)
                {
                    blendRgba(previous.data(), current.data(), double(phase) / multiplier, scratch.data(), pixels);
                    output(frameId, phase, scratch.data());
                }
            output(frameId, 0, current.data());
            previous.swap(current);
            busy = false;
            frameDone(frameId);
        });
        return true;
    }

  private:
    QObject context;
    size_t pixels;
    unsigned multiplier;
    bool busy = false;
    std::vector<uint8_t> previous, current, scratch;
};

// Windows + RTX: the real EnhancementPipeline and its worker process (pipeline_backend.cpp).
std::unique_ptr<Backend> createPipelineBackend(const LabOptions &options);
// Paced playback into a window, with presentation timing statistics (realtime.cpp).
int runRealtime(const LabOptions &options, const VideoInfo &video);
} // namespace videolab
