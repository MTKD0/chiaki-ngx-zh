// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once
#include <QHostAddress>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <cstdint>
#include <memory>

// Recordings are decoded and encoded by the user's own ffmpeg executable. The FFmpeg bundled
// with Chiaki only carries the stream decoders: no MP4/MKV demuxers, no encoders.
namespace videolab
{
struct VideoInfo
{
    int width = 0, height = 0;
    double fps = 0, duration = 0;
    QString rate; // exact rational from the container, e.g. 60000/1001
    bool variableRate = false;
    QString error;
};

inline double parseRate(const QString &text)
{
    const auto parts = text.trimmed().split('/');
    bool okay = false;
    const double numerator = parts.value(0).toDouble(&okay);
    if (!okay)
        return 0;
    if (parts.size() < 2)
        return numerator;
    const double denominator = parts[1].toDouble(&okay);
    return okay && denominator > 0 ? numerator / denominator : 0;
}

inline VideoInfo probeVideo(const QString &ffprobe, const QString &file)
{
    VideoInfo info;
    QProcess process;
    process.start(ffprobe, {"-v", "error", "-select_streams", "v:0", "-show_entries",
                            "stream=width,height,avg_frame_rate,r_frame_rate:format=duration", "-of",
                            "default=noprint_wrappers=1", file});
    if (!process.waitForStarted(10000))
    {
        info.error = QStringLiteral("cannot start %1: %2").arg(ffprobe, process.errorString());
        return info;
    }
    process.waitForFinished(60000);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode())
    {
        info.error = QStringLiteral("ffprobe failed: %1").arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
        return info;
    }
    double average = 0, nominal = 0;
    QString averageText, nominalText;
    for (const auto &line : QString::fromUtf8(process.readAllStandardOutput()).split('\n'))
    {
        const int at = line.indexOf('=');
        if (at < 0)
            continue;
        const QString key = line.left(at).trimmed(), value = line.mid(at + 1).trimmed();
        if (key == "width")
            info.width = value.toInt();
        else if (key == "height")
            info.height = value.toInt();
        else if (key == "avg_frame_rate")
            average = parseRate(averageText = value);
        else if (key == "r_frame_rate")
            nominal = parseRate(nominalText = value);
        else if (key == "duration")
            info.duration = value.toDouble();
    }
    info.fps = average > 0 ? average : nominal;
    info.rate = average > 0 ? averageText : nominalText;
    info.variableRate = average > 0 && nominal > 0 && qAbs(average - nominal) > nominal * 0.01;
    if (info.width <= 0 || info.height <= 0 || info.fps <= 0)
        info.error = QStringLiteral("no usable video stream in %1").arg(file);
    return info;
}

// ffmpeg sends raw RGBA to a loopback socket. QProcess would buffer its stdout without limit
// (8 MB per 1080p frame); a socket read buffer makes ffmpeg wait for the consumer instead.
class RawVideoReader
{
  public:
    ~RawVideoReader() { stop(); }
    bool start(const QString &ffmpeg, const QString &file, int width, int height, const QString &rate,
               double startSeconds, double durationSeconds, QString *error)
    {
        frameBytes = qint64(width) * height * 4;
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0))
        {
            *error = QStringLiteral("cannot open a loopback port: %1").arg(server.errorString());
            return false;
        }
        QStringList arguments{"-v", "error", "-nostdin"};
        if (startSeconds > 0)
            arguments << "-ss" << QString::number(startSeconds, 'f', 3);
        arguments << "-i" << file;
        if (durationSeconds > 0)
            arguments << "-t" << QString::number(durationSeconds, 'f', 3);
        arguments << "-an" << "-sn" << "-vf"
                  << QStringLiteral("fps=%1,scale=%2:%3:flags=bicubic").arg(rate).arg(width).arg(height)
                  << "-pix_fmt" << "rgba" << "-f" << "rawvideo"
                  << QStringLiteral("tcp://127.0.0.1:%1").arg(server.serverPort());
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(ffmpeg, arguments);
        if (!process.waitForStarted(10000))
        {
            *error = QStringLiteral("cannot start %1: %2").arg(ffmpeg, process.errorString());
            return false;
        }
        // Poll in short steps: a decoder that exits early must not cost the whole timeout.
        for (int waited = 0; !server.hasPendingConnections() && waited < 30000; waited += 100)
        {
            if (server.waitForNewConnection(100))
                break;
            if (process.state() == QProcess::NotRunning)
                break;
        }
        socket.reset(server.nextPendingConnection());
        if (!socket)
        {
            process.waitForFinished(1000);
            *error = QStringLiteral("ffmpeg did not deliver video: %1").arg(output());
            stop();
            return false;
        }
        socket->setParent(nullptr);
        socket->setReadBufferSize(frameBytes * 4);
        return true;
    }
    // Blocks until one whole frame is copied to `destination`. False at end of video or on error.
    bool read(uint8_t *destination, int timeoutMs = 30000)
    {
        if (!socket)
            return false;
        while (socket->bytesAvailable() < frameBytes)
        {
            if (socket->state() != QAbstractSocket::ConnectedState)
                return false;
            if (!socket->waitForReadyRead(timeoutMs) && socket->bytesAvailable() < frameBytes)
            {
                stalled = socket->state() == QAbstractSocket::ConnectedState;
                return false;
            }
        }
        return socket->read(reinterpret_cast<char *>(destination), frameBytes) == frameBytes;
    }
    bool frameReady() const { return socket && socket->bytesAvailable() >= frameBytes; }
    bool finished() const
    {
        return !socket || (socket->state() != QAbstractSocket::ConnectedState && socket->bytesAvailable() < frameBytes);
    }
    bool timedOut() const { return stalled; }
    QString output() { return QString::fromLocal8Bit(process.readAll()).trimmed(); }
    void stop()
    {
        socket.reset();
        if (process.state() != QProcess::NotRunning)
        {
            process.kill();
            process.waitForFinished(2000);
        }
    }

  private:
    QProcess process;
    std::unique_ptr<QTcpSocket> socket;
    qint64 frameBytes = 0;
    bool stalled = false;
};

// Pipes raw RGBA into an ffmpeg encoder so a run can be inspected frame by frame afterwards.
class RawVideoWriter
{
  public:
    ~RawVideoWriter()
    {
        if (process.state() != QProcess::NotRunning)
        {
            process.kill();
            process.waitForFinished(2000);
        }
    }
    bool start(const QString &ffmpeg, const QString &file, int width, int height, double fps,
               const QStringList &encoder, QString *error)
    {
        frameBytes = qint64(width) * height * 4;
        QStringList arguments{"-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba", "-s",
                              QStringLiteral("%1x%2").arg(width).arg(height), "-r", QString::number(fps, 'f', 6),
                              "-i", "-"};
        arguments << encoder << file;
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.setStandardOutputFile(QProcess::nullDevice());
        process.start(ffmpeg, arguments);
        if (!process.waitForStarted(10000))
        {
            *error = QStringLiteral("cannot start %1: %2").arg(ffmpeg, process.errorString());
            return false;
        }
        return true;
    }
    bool active() const { return frameBytes > 0; }
    bool write(const uint8_t *frame)
    {
        if (process.state() != QProcess::Running)
            return false;
        if (process.write(reinterpret_cast<const char *>(frame), frameBytes) != frameBytes)
            return false;
        while (process.bytesToWrite() > frameBytes * 2)
            if (!process.waitForBytesWritten(30000))
                return false;
        return true;
    }
    bool finish(QString *error)
    {
        while (process.state() == QProcess::Running && process.bytesToWrite() > 0)
            if (!process.waitForBytesWritten(30000))
                break;
        process.closeWriteChannel();
        const bool exited = process.waitForFinished(300000);
        frameBytes = 0;
        if (exited && process.exitStatus() == QProcess::NormalExit && !process.exitCode())
            return true;
        *error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (error->isEmpty())
            *error = QStringLiteral("encoder did not finish");
        return false;
    }

  private:
    QProcess process;
    qint64 frameBytes = 0;
};
} // namespace videolab
