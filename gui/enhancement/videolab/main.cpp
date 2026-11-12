// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Frame generation video lab: runs a recorded game clip through the enhancement pipeline
// without a console, a PSN session or the Chiaki GUI.
//
//   quality   every skipped source frame is the ground truth for a generated frame
//   realtime  paced playback into a window, with presentation timing
//   dry-run   the same measurements with a CPU cross-fade instead of the GPU worker
#include "backend.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QTextStream>
#include <cstring>

using namespace videolab;

namespace
{
struct WorstFrame
{
    double psnr = 0;
    uint64_t realFrame = 0;
    unsigned phase = 0;
    std::vector<uint8_t> generated, truth;
};

class QualitySession
{
  public:
    QualitySession(const LabOptions &options, const VideoInfo &video, std::unique_ptr<Backend> generator)
        : o(options), sourceFps(options.fps > 0 ? options.fps : video.fps), realFps(sourceFps / options.decimate),
          pixels(size_t(options.width) * options.height), backend(std::move(generator)), previous(pixels * 4),
          current(pixels * 4), scratch(pixels * 4), between(options.decimate - 1, std::vector<uint8_t>(pixels * 4)),
          phasePsnr(options.multiplier), phaseSsim(options.multiplier), phaseBlend(options.multiplier),
          phaseRepeat(options.multiplier), phaseTile(options.multiplier)
    {
        rate = o.fps > 0 ? QString::number(o.fps, 'f', 6) : video.rate;
    }

    int run()
    {
        QString error;
        if (!QDir().mkpath(o.outDir))
            return fatal(QStringLiteral("cannot create %1").arg(o.outDir));
        csvFile.setFileName(QDir(o.outDir).filePath("frames.csv"));
        if (!csvFile.open(QIODevice::WriteOnly | QIODevice::Text))
            return fatal(QStringLiteral("cannot write %1").arg(csvFile.fileName()));
        csv.setDevice(&csvFile);
        csv.setRealNumberNotation(QTextStream::FixedNotation);
        csv.setRealNumberPrecision(4);
        csv << "real_frame,phase,truth_source_frame,psnr_fg,ssim_fg,worst_tile_psnr,worst_tile_x,worst_tile_y,"
               "psnr_blend,psnr_repeat,gain_vs_blend,motion_psnr\n";
        if (!reader.start(o.ffmpeg, o.input, o.width, o.height, rate, o.start, o.duration, &error))
            return fatal(error);
        if (!o.record.isEmpty())
        {
            const double outputFps = realFps * (o.frameGeneration ? o.multiplier : 1);
            if (!writer.start(o.ffmpeg, o.record, o.width, o.height, outputFps, o.encoder, &error))
                return fatal(error);
        }
        backend->ready = [this] { QTimer::singleShot(0, &context, [this] { submitNext(); }); };
        backend->output = [this](uint64_t id, unsigned phase, const uint8_t *rgba) { onOutput(id, phase, rgba); };
        backend->frameDone = [this](uint64_t) {
            previous.swap(current);
            QTimer::singleShot(0, &context, [this] { submitNext(); });
        };
        backend->failed = [this](const QString &message) { stop(message); };
        backend->status = [](const QString &message) { qInfo().noquote() << "[lab] pipeline:" << message; };
        watchdog.setSingleShot(true);
        watchdog.setInterval(90000);
        QObject::connect(&watchdog, &QTimer::timeout, &context,
                         [this] { stop(QStringLiteral("no progress for 90 s")); });
        watchdog.start();
        elapsed.start();
        backend->start();
        if (!finished) // A generator that fails while starting has already asked to stop.
            QCoreApplication::exec();
        // Never tear the generator down from inside one of its own callbacks.
        backend->stop();
        reader.stop();
        if (writer.active() && !writer.finish(&error) && failure.isEmpty())
            failure = QStringLiteral("recording failed: %1").arg(error);
        csv.flush();
        csvFile.close();
        writeWorst();
        if (failure.isEmpty() && o.frameGeneration && realFrames > 1 && !generatedFrames)
            failure = QStringLiteral("frame generation never produced a generated frame");
        if (failure.isEmpty() && !realFrames)
            failure = QStringLiteral("the clip produced no frames");
        writeSummary();
        if (!failure.isEmpty())
        {
            qCritical().noquote() << "[lab] FAILED:" << failure;
            return 1;
        }
        return 0;
    }

  private:
    int fatal(const QString &message)
    {
        qCritical().noquote() << "[lab] FAILED:" << message;
        return 1;
    }
    void stop(const QString &message = QString())
    {
        if (finished)
            return;
        finished = true;
        failure = message;
        QCoreApplication::quit();
    }
    void submitNext()
    {
        if (finished)
            return;
        if (o.maxRealFrames && realFrames >= uint64_t(o.maxRealFrames))
            return stop();
        // Skipped source frames are kept: they are what the generator is asked to reproduce.
        const bool first = submitted == 0;
        bool okay = true;
        for (size_t n = 0; okay && !first && n < between.size(); ++n)
            okay = reader.read(between[n].data());
        okay = okay && reader.read(current.data());
        if (!okay)
            return stop(reader.timedOut() ? QStringLiteral("ffmpeg stopped delivering frames: %1").arg(reader.output())
                                          : QString());
        ++submitted;
        generatedForCurrent = 0;
        motionPsnr = first ? 0 : psnr(mseRgb(previous.data(), current.data(), pixels));
        // Gaps above 250 ms make the pipeline reset FG history, as after a stream stall.
        const uint64_t timestampUs = uint64_t(double(submitted) * std::min(1e6 / realFps, 100000.0));
        if (!backend->submit(submitted, timestampUs, current.data()))
            stop(QStringLiteral("the generator refused frame %1").arg(submitted));
    }
    void onOutput(uint64_t id, unsigned phase, const uint8_t *rgba)
    {
        if (finished || id != submitted)
            return;
        watchdog.start();
        if (phase)
        {
            ++generatedFrames;
            ++generatedForCurrent;
            score(phase, rgba);
            record(rgba);
            return;
        }
        ++realFrames;
        bridgePsnr.add(psnr(mseRgb(current.data(), rgba, pixels)));
        record(rgba);
        // Keep the recording in step with the source while FG warms up or is reset.
        for (unsigned n = generatedForCurrent + 1; o.frameGeneration && n < o.multiplier; ++n)
            record(rgba);
        if (o.frameGeneration && submitted > 1 && !generatedForCurrent)
            ++ungenerated;
        if (realFrames % 30 == 0)
            qInfo().noquote() << QStringLiteral("[lab] %1 real frames, %2 generated, mean PSNR %3 dB")
                                     .arg(realFrames).arg(generatedFrames).arg(allPsnr.mean(), 0, 'f', 2);
    }
    void score(unsigned phase, const uint8_t *generated)
    {
        const int index = truthIndex(phase, o.multiplier, o.decimate);
        csv << submitted << ',' << phase << ',';
        if (index < 0)
        {
            csv << ",,,,,,,,," << motionPsnr << '\n';
            return;
        }
        const uint8_t *truth = between[size_t(index)].data();
        const double fg = psnr(mseRgb(generated, truth, pixels));
        const double ssim = ssimRgba(generated, truth, o.width, o.height);
        const TileResult tile = worstTile(generated, truth, o.width, o.height);
        blendRgba(previous.data(), current.data(), double(phase) / o.multiplier, scratch.data(), pixels);
        const double blend = psnr(mseRgb(scratch.data(), truth, pixels));
        const double repeat = psnr(mseRgb(previous.data(), truth, pixels));
        // Real frame k is source frame (k - 1) * decimate, counted from the start of the clip.
        const uint64_t source = (submitted - 2) * o.decimate + uint64_t(index) + 1;
        csv << source << ',' << fg << ',' << ssim << ',' << tile.worstPsnr << ',' << tile.x << ',' << tile.y << ','
            << blend << ',' << repeat << ',' << fg - blend << ',' << motionPsnr << '\n';
        allPsnr.add(fg);
        allSsim.add(ssim);
        allBlend.add(blend);
        allTile.add(tile.worstPsnr);
        phasePsnr[phase].add(fg);
        phaseSsim[phase].add(ssim);
        phaseBlend[phase].add(blend);
        phaseRepeat[phase].add(repeat);
        phaseTile[phase].add(tile.worstPsnr);
        keepWorst(fg, phase, generated, truth);
    }
    void keepWorst(double value, unsigned phase, const uint8_t *generated, const uint8_t *truth)
    {
        if (o.worst <= 0)
            return;
        size_t slot = worst.size();
        if (worst.size() < size_t(o.worst))
            worst.emplace_back();
        else
        {
            slot = 0;
            for (size_t n = 1; n < worst.size(); ++n)
                if (worst[n].psnr > worst[slot].psnr)
                    slot = n;
            if (worst[slot].psnr <= value)
                return;
        }
        auto &entry = worst[slot];
        entry.psnr = value;
        entry.realFrame = submitted;
        entry.phase = phase;
        entry.generated.assign(generated, generated + pixels * 4);
        entry.truth.assign(truth, truth + pixels * 4);
    }
    void record(const uint8_t *rgba)
    {
        if (writer.active() && !writer.write(rgba))
            stop(QStringLiteral("recording stopped accepting frames"));
    }
    void writeWorst()
    {
        std::sort(worst.begin(), worst.end(), [](const WorstFrame &a, const WorstFrame &b) { return a.psnr < b.psnr; });
        int rank = 0;
        for (const auto &entry : worst)
        {
            const QString base = QDir(o.outDir).filePath(QStringLiteral("worst_%1_real%2_phase%3_%4dB")
                .arg(++rank, 2, 10, QChar('0')).arg(entry.realFrame, 6, 10, QChar('0')).arg(entry.phase)
                .arg(entry.psnr, 0, 'f', 1));
            QImage(entry.generated.data(), o.width, o.height, o.width * 4, QImage::Format_RGBA8888)
                .save(base + "_generated.png");
            QImage(entry.truth.data(), o.width, o.height, o.width * 4, QImage::Format_RGBA8888)
                .save(base + "_truth.png");
            // Absolute error, amplified so that small ghosting is visible.
            std::vector<uint8_t> difference(pixels * 4);
            for (size_t n = 0; n < pixels * 4; n += 4)
            {
                for (size_t k = 0; k < 3; ++k)
                    difference[n + k] = uint8_t(std::min(255, 4 * std::abs(int(entry.generated[n + k]) - int(entry.truth[n + k]))));
                difference[n + 3] = 255;
            }
            QImage(difference.data(), o.width, o.height, o.width * 4, QImage::Format_RGBA8888)
                .save(base + "_error_x4.png");
        }
    }
    void writeSummary()
    {
        QString summary;
        QTextStream text(&summary);
        text.setRealNumberNotation(QTextStream::FixedNotation);
        text.setRealNumberPrecision(2);
        const double seconds = elapsed.elapsed() / 1000.0;
        text << "mode: " << o.mode << "\nbackend: " << backend->name() << "\nclip: " << o.input << "\nlabel: "
             << o.label << "\n"
             << "size: " << o.width << 'x' << o.height << "  source fps: " << sourceFps << "  decimate: " << o.decimate
             << "  real fps fed to FG: " << realFps << "\n"
             << "frame generation: "
             << (o.frameGeneration ? QStringLiteral("%1x -> %2 fps").arg(o.multiplier).arg(realFps * o.multiplier, 0, 'f', 2)
                                   : QStringLiteral("off"))
             << "  NR: " << (o.neural ? "on" : "off") << "\n"
             << "real frames: " << realFrames << "  generated frames: " << generatedFrames
             << "  real frames without generated output: " << ungenerated << "\n";
        if (allPsnr.count())
        {
            text << "\nGenerated frames against the skipped source frames (" << allPsnr.count() << " scored):\n"
                 << "  PSNR dB        mean " << allPsnr.mean() << "  p5 " << allPsnr.percentile(5) << "  min "
                 << allPsnr.min() << "\n";
            text.setRealNumberPrecision(4);
            text << "  SSIM           mean " << allSsim.mean() << "  p5 " << allSsim.percentile(5) << "  min "
                 << allSsim.min() << "\n";
            text.setRealNumberPrecision(2);
            text << "  worst 64px tile PSNR dB   mean " << allTile.mean() << "  min " << allTile.min() << "\n"
                 << "  cross-fade baseline PSNR dB   mean " << allBlend.mean() << "\n"
                 << "  gain over cross-fade dB       mean " << allPsnr.mean() - allBlend.mean() << "\n";
            for (unsigned phase = 1; phase < o.multiplier; ++phase)
                if (phasePsnr[phase].count())
                {
                    text << "  phase " << phase << '/' << o.multiplier << ": PSNR " << phasePsnr[phase].mean() << " (p5 "
                         << phasePsnr[phase].percentile(5) << ")  SSIM ";
                    text.setRealNumberPrecision(4);
                    text << phaseSsim[phase].mean();
                    text.setRealNumberPrecision(2);
                    text << "  worst tile " << phaseTile[phase].mean() << "  cross-fade " << phaseBlend[phase].mean()
                         << "  repeat-previous " << phaseRepeat[phase].mean() << "\n";
                }
        }
        else if (o.frameGeneration)
            text << "\nNo ground truth: with --decimate " << o.decimate << " and " << o.multiplier
                 << "x no generated frame coincides with a skipped source frame.\n";
        if (bridgePsnr.count())
            text << "\nreal frame round trip PSNR dB: min " << bridgePsnr.min() << " (100 = bit exact)\n";
        if (backend->fgMs.count() || backend->flowMs.count())
            text << "worker GPU ms: FG mean " << backend->fgMs.mean() << "  optical flow mean "
                 << backend->flowMs.mean() << "  NR mean " << backend->nrMs.mean() << "\n";
        text << "elapsed: " << seconds << " s\n";
        if (!failure.isEmpty())
            text << "FAILED: " << failure << "\n";
        QFile file(QDir(o.outDir).filePath("summary.txt"));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
            file.write(summary.toUtf8());
        qInfo().noquote() << "\n" + summary;
        qInfo().noquote() << "[lab] results in" << QDir(o.outDir).absolutePath();
        appendHistory();
    }
    // One line per run, so successive builds of the worker can be compared in a spreadsheet.
    void appendHistory()
    {
        if (o.history.isEmpty())
            return;
        QFile file(o.history);
        const bool header = !file.exists() || !file.size();
        if (!file.open(QIODevice::Append | QIODevice::Text))
        {
            qWarning().noquote() << "[lab] cannot append to" << o.history;
            return;
        }
        QTextStream out(&file);
        out.setRealNumberNotation(QTextStream::FixedNotation);
        out.setRealNumberPrecision(4);
        if (header)
            out << "time,label,clip,backend,width,height,real_fps,multiplier,decimate,real_frames,generated_frames,"
                   "ungenerated_real_frames,psnr_mean,psnr_p5,psnr_min,ssim_mean,worst_tile_mean,blend_psnr_mean,"
                   "gain_vs_blend,fg_gpu_ms,flow_ms,status\n";
        const auto quoted = [](QString value) { return '"' + value.replace('"', "\"\"") + '"'; };
        out << QDateTime::currentDateTime().toString(Qt::ISODate) << ',' << quoted(o.label) << ','
            << quoted(QFileInfo(o.input).fileName()) << ',' << backend->name() << ',' << o.width << ',' << o.height << ','
            << realFps << ',' << (o.frameGeneration ? o.multiplier : 1) << ',' << o.decimate << ',' << realFrames << ','
            << generatedFrames << ',' << ungenerated << ',' << allPsnr.mean() << ',' << allPsnr.percentile(5) << ','
            << allPsnr.min() << ',' << allSsim.mean() << ',' << allTile.mean() << ',' << allBlend.mean() << ','
            << allPsnr.mean() - allBlend.mean() << ',' << backend->fgMs.mean() << ',' << backend->flowMs.mean() << ','
            << quoted(failure.isEmpty() ? QStringLiteral("ok") : failure) << '\n';
    }

    const LabOptions o;
    double sourceFps, realFps;
    QString rate;
    size_t pixels;
    std::unique_ptr<Backend> backend;
    QObject context;
    RawVideoReader reader;
    RawVideoWriter writer;
    QFile csvFile;
    QTextStream csv;
    QTimer watchdog;
    QElapsedTimer elapsed;
    std::vector<uint8_t> previous, current, scratch;
    std::vector<std::vector<uint8_t>> between;
    std::vector<Series> phasePsnr, phaseSsim, phaseBlend, phaseRepeat, phaseTile;
    Series allPsnr, allSsim, allBlend, allTile, bridgePsnr;
    std::vector<WorstFrame> worst;
    uint64_t submitted = 0, realFrames = 0, generatedFrames = 0, ungenerated = 0;
    unsigned generatedForCurrent = 0;
    double motionPsnr = 0;
    bool finished = false;
    QString failure;
};

bool parseOptions(const QStringList &arguments, LabOptions &o)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Runs a recorded game clip through Chiaki's frame generation pipeline and measures the result.\n"
        "Record at twice (2x), three times (3x) or four times (4x) the frame rate you want to simulate:\n"
        "the frames in between are the ground truth for the generated ones.");
    parser.addHelpOption();
    parser.addPositionalArgument("video", "Recording to analyse (anything your ffmpeg can decode).");
    const auto add = [&](const char *name, const char *description, const char *value = nullptr,
                         const QString &fallback = QString()) {
        parser.addOption(value ? QCommandLineOption(name, description, value, fallback) : QCommandLineOption(name, description));
    };
    add("mode", "quality (default), realtime or dry-run.", "mode", "quality");
    add("multiplier", "Frame generation multiplier: 2, 3 or 4.", "n", "2");
    add("no-fg", "Leave frame generation off (pipeline pass-through baseline).");
    add("decimate", "Feed every Nth source frame as a real frame. Default: the multiplier in quality and dry-run "
                    "mode, 1 in realtime mode.", "n");
    add("width", "Processing width. Default: the recording's.", "pixels");
    add("height", "Processing height. Default: the recording's.", "pixels");
    add("fps", "Treat the recording as this constant frame rate. Default: the recording's.", "fps");
    add("start", "Skip this many seconds of the recording.", "seconds");
    add("duration", "Use only this many seconds of the recording.", "seconds");
    add("frames", "Stop after this many real frames.", "n");
    add("nr", "Also enable DLSS neural rendering.");
    add("percent", "NR render percentage, 50-100.", "n", "65");
    add("sharpness", "NR reconstruction sharpness, 0-100.", "n", "0");
    add("no-fsr", "Disable FSR reconstruction after NR.");
    add("out", "Result directory. Default: videolab-out/<time>-<mode>.", "dir");
    add("record", "quality/dry-run: encode every output frame, in display order, to this file.", "file");
    add("encoder", "ffmpeg output options for --record.", "options",
        "-c:v libx264 -preset veryfast -crf 14 -pix_fmt yuv420p -vf pad=ceil(iw/2)*2:ceil(ih/2)*2");
    add("worst", "Save the N worst generated frames with their ground truth as PNG.", "n", "8");
    add("label", "Free text stored with the results, e.g. the change being tested.", "text");
    add("history", "Append one summary line per run to this CSV file.", "file");
    add("ffmpeg", "ffmpeg executable.", "path", "ffmpeg");
    add("ffprobe", "ffprobe executable.", "path", "ffprobe");
    add("stage", "Chiaki build directory holding enhancement/nvngx.dll and runtimes/.", "dir");
    add("worker", "Enhancement worker executable (overrides --stage).", "path");
    add("runtime-dir", "NVIDIA runtime directory (overrides --stage).", "dir");
    add("vsync", "realtime: present with FIFO instead of IMMEDIATE.");
    add("window", "realtime: window size.", "WxH", "1280x720");
    parser.process(arguments);

    if (parser.positionalArguments().size() != 1)
    {
        qCritical().noquote() << parser.helpText();
        return false;
    }
    o.input = parser.positionalArguments().first();
    o.mode = parser.value("mode");
    if (o.mode != "quality" && o.mode != "realtime" && o.mode != "dry-run")
    {
        qCritical() << "--mode must be quality, realtime or dry-run";
        return false;
    }
    o.frameGeneration = !parser.isSet("no-fg");
    o.multiplier = parser.value("multiplier").toUInt();
    if (o.multiplier < 2 || o.multiplier > 4)
    {
        qCritical() << "--multiplier must be 2, 3 or 4";
        return false;
    }
    o.decimate = parser.isSet("decimate") ? parser.value("decimate").toUInt()
                 : (o.mode == "realtime" || !o.frameGeneration) ? 1u : o.multiplier;
    if (o.decimate < 1 || o.decimate > 16)
    {
        qCritical() << "--decimate must be between 1 and 16";
        return false;
    }
    o.width = parser.value("width").toInt();
    o.height = parser.value("height").toInt();
    o.fps = parser.value("fps").toDouble();
    o.start = parser.value("start").toDouble();
    o.duration = parser.value("duration").toDouble();
    o.maxRealFrames = parser.value("frames").toInt();
    o.neural = parser.isSet("nr");
    o.fsr = !parser.isSet("no-fsr");
    o.percent = qBound(50, parser.value("percent").toInt(), 100);
    o.sharpness = qBound(0, parser.value("sharpness").toInt(), 100);
    o.worst = qBound(0, parser.value("worst").toInt(), 64);
    o.label = parser.value("label");
    o.history = parser.value("history");
    o.record = parser.value("record");
    o.encoder = parser.value("encoder").split(' ', Qt::SkipEmptyParts);
    o.ffmpeg = parser.value("ffmpeg");
    o.ffprobe = parser.value("ffprobe");
    o.vsync = parser.isSet("vsync");
    const auto window = parser.value("window").split('x');
    if (window.size() == 2 && window[0].toInt() > 0 && window[1].toInt() > 0)
    {
        o.windowWidth = window[0].toInt();
        o.windowHeight = window[1].toInt();
    }
    o.outDir = parser.isSet("out") ? parser.value("out")
        : QStringLiteral("videolab-out/%1-%2").arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"), o.mode);
    if (o.mode == "realtime" && !o.record.isEmpty())
    {
        qCritical() << "--record reads every frame back from the GPU and would disturb realtime pacing; use it in quality mode";
        return false;
    }
    // The pipeline locates its worker and runtimes through these variables.
    if (parser.isSet("stage"))
    {
        const QDir stage(parser.value("stage"));
        qputenv("CHIAKI_ENHANCEMENT_WORKER", stage.absoluteFilePath("enhancement/nvngx.dll").toLocal8Bit());
        qputenv("CHIAKI_ENHANCEMENT_RUNTIME_DIR", stage.absoluteFilePath("runtimes").toLocal8Bit());
    }
    if (parser.isSet("worker"))
        qputenv("CHIAKI_ENHANCEMENT_WORKER", QFileInfo(parser.value("worker")).absoluteFilePath().toLocal8Bit());
    if (parser.isSet("runtime-dir"))
        qputenv("CHIAKI_ENHANCEMENT_RUNTIME_DIR", QFileInfo(parser.value("runtime-dir")).absoluteFilePath().toLocal8Bit());
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    // Only realtime playback needs a window system.
    bool realtime = false;
    for (int n = 1; n < argc; ++n)
        realtime |= !std::strcmp(argv[n], "realtime") || !std::strcmp(argv[n], "--mode=realtime");
    std::unique_ptr<QCoreApplication> app(realtime ? new QGuiApplication(argc, argv) : new QCoreApplication(argc, argv));
    QCoreApplication::setApplicationName("enhancement-video-lab");

    LabOptions options;
    if (!parseOptions(app->arguments(), options))
        return 2;
    if (!QFileInfo::exists(options.input))
    {
        qCritical().noquote() << "[lab] no such file:" << options.input;
        return 2;
    }
    VideoInfo video = probeVideo(options.ffprobe, options.input);
    if (!video.error.isEmpty() && (options.width <= 0 || options.height <= 0 || options.fps <= 0))
    {
        qCritical().noquote() << "[lab]" << video.error;
        qCritical() << "[lab] install ffmpeg/ffprobe (or pass --ffmpeg/--ffprobe), or give --width, --height and --fps";
        return 2;
    }
    if (options.width <= 0)
        options.width = video.width;
    if (options.height <= 0)
        options.height = video.height;
    const double sourceFps = options.fps > 0 ? options.fps : video.fps;
    qInfo().noquote() << QStringLiteral("[lab] %1: %2x%3 @ %4 fps, processing %5x%6, 1 real frame per %7 source frames (%8 fps)%9")
        .arg(QFileInfo(options.input).fileName()).arg(video.width).arg(video.height).arg(video.fps, 0, 'f', 3)
        .arg(options.width).arg(options.height).arg(options.decimate).arg(sourceFps / options.decimate, 0, 'f', 3)
        .arg(options.frameGeneration ? QStringLiteral(", FG %1x").arg(options.multiplier) : QStringLiteral(", FG off"));
    if (video.variableRate && options.fps <= 0)
        qWarning() << "[lab] the recording has a variable frame rate; duplicated or dropped frames will distort the "
                      "ground truth. Record with a constant frame rate.";
    if (options.mode != "realtime" && options.frameGeneration && options.decimate == 1)
        qWarning() << "[lab] --decimate 1 leaves no skipped frames: output is produced but cannot be scored";

    if (options.mode == "realtime")
        return runRealtime(options, video);
    std::unique_ptr<Backend> backend;
    if (options.mode == "dry-run")
        backend.reset(new BlendBackend(options.width, options.height, options.frameGeneration ? options.multiplier : 1));
    else
        backend = createPipelineBackend(options);
    return QualitySession(options, video, std::move(backend)).run();
}
