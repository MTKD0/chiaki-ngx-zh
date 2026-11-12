// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "vulkan_device.h"
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QTextStream>
#include <QWindow>
#ifdef Q_OS_WIN
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

// Plays the recording at stream cadence through the pipeline into a window, the way a console
// stream is presented, and logs when every real and generated frame reached the swapchain.
namespace videolab
{
namespace
{
struct Presentation
{
    qint64 timeUs;
    uint64_t frameId;
    unsigned phase;
    qint64 workUs;
};
} // namespace

int runRealtime(const LabOptions &o, const VideoInfo &video)
{
#ifndef Q_OS_WIN
    Q_UNUSED(o);
    Q_UNUSED(video);
    qCritical() << "Realtime mode presents through the Windows enhancement worker; use --mode dry-run here.";
    return 1;
#else
    const QString rate = o.fps > 0 ? QString::number(o.fps, 'f', 6) : video.rate;
    const double sourceFps = o.fps > 0 ? o.fps : video.fps;
    const double realFps = sourceFps / o.decimate;
    const qint64 intervalNs = qint64(1e9 / realFps);
    const size_t frameBytes = size_t(o.width) * o.height * 4;

    RawVideoReader reader;
    QString error;
    if (!reader.start(o.ffmpeg, o.input, o.width, o.height, rate, o.start, o.duration, &error))
    {
        qCritical().noquote() << error;
        return 1;
    }

    QWindow window;
    window.setTitle(QStringLiteral("Chiaki FG video lab — %1").arg(o.label.isEmpty() ? o.input : o.label));
    window.resize(o.windowWidth, o.windowHeight);
    window.show();
    VulkanDevice device;
    if (!device.create(true))
    {
        qCritical() << "Cannot create a Vulkan device";
        return 1;
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    auto createSurface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        device.instance->get_proc_addr(device.instance->instance, "vkCreateWin32SurfaceKHR"));
    VkWin32SurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = GetModuleHandleW(nullptr);
    surfaceInfo.hwnd = reinterpret_cast<HWND>(window.winId());
    if (!createSurface || createSurface(device.instance->instance, &surfaceInfo, nullptr, &surface) != VK_SUCCESS)
    {
        qCritical() << "Cannot create a window surface";
        return 1;
    }
    pl_vulkan_swapchain_params swapchainParams{};
    swapchainParams.surface = surface;
    swapchainParams.present_mode = o.vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    pl_swapchain swapchain = pl_vulkan_create_swapchain(device.vk, &swapchainParams);
    if (!swapchain)
    {
        qCritical() << "Cannot create a swapchain";
        return 1;
    }

    int result = 1;
    std::vector<Presentation> presentations;
    uint64_t submitted = 0, busyDrops = 0, underruns = 0, sourceTicks = 0;
    Series fgMs, flowMs;
    QString failure, lastStatus;
    bool vsrSeen = false;
    {
        EnhancementPipeline pipeline;
        const EnhancementOptions options = enhancementOptions(o);
        std::vector<uint8_t> frame(frameBytes);
        bool configured = false, stopping = false;
        QElapsedTimer clock;
        qint64 nextDueNs = 0;

        QObject::connect(&pipeline, &EnhancementPipeline::statusChanged, qApp,
                         [&](QString message, int, int, int, int, bool, bool, bool) {
                             if (stopping)
                                 return;
                             if (message != lastStatus)
                                 qInfo().noquote() << "[lab] pipeline:" << message;
                             lastStatus = message;
                             const bool featureMissing = message.startsWith("GPU bridge ready") &&
                                 o.frameGeneration && message.contains("; FG:");
                             if (pipelineFailure(message) || featureMissing ||
                                 (configured && !pipeline.ownsPresentation()))
                             {
                                 failure = message;
                                 qApp->quit();
                             }
                             else if (message.startsWith("GPU bridge ready") && !configured)
                             {
                                 configured = true;
                                 clock.start();
                             }
                         });
        QObject::connect(&pipeline, &EnhancementPipeline::timingsChanged, qApp,
                         [&](double, double, double fg, double, double flow, double, bool vsrActive) {
                             vsrSeen |= vsrActive;
                             if (fg > 0)
                                 fgMs.add(fg);
                             if (flow > 0)
                                 flowMs.add(flow);
                         });
        QObject::connect(&pipeline, &EnhancementPipeline::frameAvailable, qApp, [&] {
            if (stopping || !pipeline.hasUnpresentedOutput())
                return;
            pl_frame image{};
            if (!pipeline.outputFrame(image))
                return;
            QElapsedTimer work;
            work.start();
            int width = int(window.width() * window.devicePixelRatio()),
                height = int(window.height() * window.devicePixelRatio());
            pl_swapchain_resize(swapchain, &width, &height);
            pl_swapchain_frame swap{};
            if (!pl_swapchain_start_frame(swapchain, &swap))
            {
                // Minimized or mid-resize. Count the phase as shown: an unconsumed output would
                // hold back every later frame until the pipeline's watchdog fires.
                pipeline.notePresented(0);
                return;
            }
            pl_frame target{};
            pl_frame_from_swapchain(&target, &swap);
            pl_rect2df_aspect_copy(&target.crop, &image.crop, 0.0);
            const bool rendered = pl_render_image(device.renderer, &image, &target, &pl_render_default_params);
            if (!pl_swapchain_submit_frame(swapchain) || !rendered)
            {
                failure = QStringLiteral("Presentation failed");
                qApp->quit();
                return;
            }
            pl_swapchain_swap_buffers(swapchain);
            const qint64 workUs = work.nsecsElapsed() / 1000;
            presentations.push_back({clock.nsecsElapsed() / 1000, pipeline.completedFrameId(),
                                     pipeline.outputPhase(), workUs});
            pipeline.notePresented(uint64_t(workUs));
        });

        QTimer pump;
        pump.setTimerType(Qt::PreciseTimer);
        pump.setInterval(1);
        QObject::connect(&pump, &QTimer::timeout, qApp, [&] {
            if (!configured || stopping)
                return;
            if (clock.nsecsElapsed() < nextDueNs)
                return;
            nextDueNs += intervalNs;
            if (clock.nsecsElapsed() > nextDueNs + intervalNs * 4)
                nextDueNs = clock.nsecsElapsed(); // Do not burst after a stall.
            if (reader.finished() || (o.maxRealFrames && sourceTicks >= uint64_t(o.maxRealFrames)))
            {
                result = 0;
                qApp->quit();
                return;
            }
            // The stream delivers one frame per tick whether or not the pipeline is free.
            if (!reader.frameReady())
            {
                ++underruns;
                return;
            }
            for (unsigned n = 0; n < o.decimate; ++n)
                if (!reader.read(frame.data(), 2000))
                    return; // End of video: the next tick sees finished().
            ++sourceTicks;
            pl_tex input = pipeline.beginFrame(submitted + 1, uint64_t(clock.nsecsElapsed() / 1000) + 1000000);
            if (!input)
            {
                ++busyDrops;
                return;
            }
            if (!device.renderInput(frame.data(), o.width, o.height, input) || !pipeline.submitFrame())
            {
                failure = QStringLiteral("Cannot hand the frame to the enhancement pipeline");
                qApp->quit();
                return;
            }
            ++submitted;
        });
        QObject::connect(qApp, &QGuiApplication::lastWindowClosed, qApp, [&] { result = 0; });

        pipeline.setSourceInterval(uint64_t(intervalNs / 1000));
        pipeline.prepare(device.vk, {uint32_t(o.vsr ? o.outputWidth : o.width), uint32_t(o.vsr ? o.outputHeight : o.height)},
                         options, false);
        pump.start();
        qApp->exec();
        stopping = true;
        pump.stop();
        pipeline.reset();
    }
    pl_swapchain_destroy(&swapchain);
    auto destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
        device.instance->get_proc_addr(device.instance->instance, "vkDestroySurfaceKHR"));
    if (destroySurface)
        destroySurface(device.instance->instance, surface, nullptr);

    if (!failure.isEmpty())
    {
        qCritical().noquote() << "[lab] FAILED:" << failure;
        result = 1;
    }
    // Presentation log and statistics.
    QDir().mkpath(o.outDir);
    QFile csv(QDir(o.outDir).filePath("cadence.csv"));
    Series intervals, realIntervals, work;
    uint64_t real = 0, generated = 0, late = 0;
    const unsigned phases = o.frameGeneration ? o.multiplier : 1;
    const double idealMs = 1000.0 / realFps / phases;
    if (csv.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QTextStream out(&csv);
        out << "time_ms,frame_id,phase,interval_ms,present_work_ms\n";
        qint64 previous = -1, previousReal = -1;
        for (const auto &p : presentations)
        {
            const double interval = previous < 0 ? 0 : (p.timeUs - previous) / 1000.0;
            out << p.timeUs / 1000.0 << ',' << p.frameId << ',' << p.phase << ',' << interval << ','
                << p.workUs / 1000.0 << '\n';
            if (previous >= 0)
            {
                intervals.add(interval);
                if (interval > idealMs * 1.5)
                    ++late;
            }
            work.add(p.workUs / 1000.0);
            if (p.phase)
                ++generated;
            else
            {
                ++real;
                if (previousReal >= 0)
                    realIntervals.add((p.timeUs - previousReal) / 1000.0);
                previousReal = p.timeUs;
            }
            previous = p.timeUs;
        }
    }
    const double seconds = presentations.size() > 1
        ? (presentations.back().timeUs - presentations.front().timeUs) / 1e6 : 0;
    QString summary;
    QTextStream text(&summary);
    text.setRealNumberNotation(QTextStream::FixedNotation);
    text.setRealNumberPrecision(2);
    text << "mode: realtime\nclip: " << o.input << "\nlabel: " << o.label << "\n"
         << "size: " << o.width << 'x' << o.height << "  source fps: " << sourceFps << "  decimate: " << o.decimate
         << "  simulated stream fps: " << realFps << "\n"
         << "frame generation: " << (o.frameGeneration ? QStringLiteral("%1x").arg(o.multiplier) : QStringLiteral("off"))
         << "  present mode: " << (o.vsync ? "fifo" : "immediate") << "\n"
         << "VSR: " << (o.vsr ? QStringLiteral("quality %1 -> %2x%3%4").arg(o.vsr).arg(o.outputWidth).arg(o.outputHeight)
                                    .arg(vsrSeen ? "" : " (NEVER ACTIVE)") : QStringLiteral("off")) << "\n"
         << "stream frames offered: " << sourceTicks << "  submitted: " << submitted
         << "  dropped (pipeline busy): " << busyDrops << "  decoder underruns: " << underruns << "\n"
         << "presented: " << real << " real + " << generated << " generated in " << seconds << " s\n";
    if (seconds > 0)
        text << "presented fps: " << (real + generated) / seconds << " (real " << real / seconds << ", generated "
             << generated / seconds << ")\n";
    text << "ideal interval: " << idealMs << " ms\n"
         << "interval ms: mean " << intervals.mean() << "  stddev " << intervals.stddev() << "  p50 "
         << intervals.percentile(50) << "  p99 " << intervals.percentile(99) << "  max " << intervals.max() << "\n"
         << "late presentations (> 1.5x ideal): " << late << " of " << intervals.count() << "\n"
         << "real-frame interval ms: mean " << realIntervals.mean() << "  stddev " << realIntervals.stddev()
         << "  max " << realIntervals.max() << "\n"
         << "presentation work ms: mean " << work.mean() << "  p99 " << work.percentile(99) << "\n"
         << "worker GPU ms: FG mean " << fgMs.mean() << "  optical flow mean " << flowMs.mean() << "\n";
    if (underruns)
        text << "NOTE: decoder underruns mean ffmpeg could not deliver frames in real time; cadence figures "
                "include those gaps. Lower --width/--height or use a faster codec.\n";
    QFile summaryFile(QDir(o.outDir).filePath("summary.txt"));
    if (summaryFile.open(QIODevice::WriteOnly | QIODevice::Text))
        summaryFile.write(summary.toUtf8());
    qInfo().noquote() << summary;
    qInfo().noquote() << "[lab] results in" << QDir(o.outDir).absolutePath();
    if (!result && o.vsr && !vsrSeen)
    {
        qCritical() << "[lab] FAILED: VSR was requested but never became active (runtimes/nvngx_vsr.dll missing or rejected?)";
        result = 1;
    }
    if (!result && o.frameGeneration && !generated)
    {
        qCritical().noquote() << "[lab] FAILED: frame generation never presented a generated frame. Last status:" << lastStatus;
        result = 1;
    }
    return result;
#endif
}
} // namespace videolab
