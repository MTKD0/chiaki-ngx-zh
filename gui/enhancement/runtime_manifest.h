// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

namespace chiaki_enhancement
{
// RTX Video SDK 1.1 / NVIDIA-signed nvngx_vsr.dll 1.6.0.0. Supplied separately.
inline QString verifyVsrRuntime(const QString &runtime)
{
    QFile file(QDir(runtime).filePath(QStringLiteral("nvngx_vsr.dll")));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
        return QCoreApplication::translate("EnhancementRuntime", "VSR runtime is missing. Copy NVIDIA RTX Video SDK nvngx_vsr.dll 1.6.0.0 into runtimes/ and restart the stream.");
    if (hash.result().toHex() != "c3d88eea5ff7a548edefa66414cf6e77464d0947277c904f324dd23abf58a1ed")
        return QCoreApplication::translate("EnhancementRuntime", "VSR runtime does not match the tested NVIDIA 1.6.0.0 release; ordinary scaling retained.");
    return {};
}
// Return an actionable error, or an empty string after validating all files
// used by this request. Optional packages are not prerequisites for each other.
inline QString verifyRuntimeManifest(const QString &runtime, bool neural, bool frameGeneration, bool sm86 = false)
{
    QFile manifest(QDir(runtime).filePath(QStringLiteral("runtime-manifest.json")));
    if (!manifest.open(QIODevice::ReadOnly))
        return QCoreApplication::translate("EnhancementRuntime", "Optional runtime package is missing; see the release README for runtimes/");
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(manifest.readAll(), &parseError);
    const auto object = document.object();
    const int schema = object.value(QStringLiteral("schema")).toInt();
    if (parseError.error != QJsonParseError::NoError || (schema != 1 && schema != 2))
        return QCoreApplication::translate("EnhancementRuntime", "Optional runtime manifest is invalid");
    QJsonObject files;
    if (schema == 1)
        files = object.value(QStringLiteral("files")).toObject();
    else
    {
        const auto packages = object.value(QStringLiteral("packages")).toObject();
        auto addPackage = [&](const QString &name, const QString &entry) {
            const auto package = packages.value(name).toObject();
            if (package.isEmpty() || !package.contains(entry))
                return false;
            for (auto file = package.begin(); file != package.end(); ++file)
            {
                // A route-specific package can narrow a shared runtime pin.
                // SM86 is added last and pins its backend's matching stock DLL.
                files.insert(file.key(), file.value());
            }
            return true;
        };
        if (neural && !addPackage(QStringLiteral("neural"), QStringLiteral("nvngx_dlssnr.dll")))
            return QCoreApplication::translate("EnhancementRuntime", "NR runtime is not pinned in this release; see the release README");
        if (frameGeneration &&
            !addPackage(QStringLiteral("frame_generation"), QStringLiteral("nvngx_dlssg.dll")))
            return QCoreApplication::translate("EnhancementRuntime", "FG runtime is not pinned in this release; see the release README");
        if (frameGeneration && sm86 &&
            (!packages.value(QStringLiteral("sm86")).toObject().contains(QStringLiteral("nvngx_dlssg.dll")) ||
             !addPackage(QStringLiteral("sm86"), QStringLiteral("sm86-proxy/version.dll"))))
            return QCoreApplication::translate("EnhancementRuntime", "Installed SM86 proxy is not pinned in this release");
    }
    if (files.isEmpty())
        return QCoreApplication::translate("EnhancementRuntime", "Optional runtime manifest contains no required files");
    const QString root = QDir(runtime).canonicalPath() + QStringLiteral("/");
    for (auto file = files.begin(); file != files.end(); ++file)
    {
        QFileInfo info(QDir(runtime).filePath(file.key()));
        // Check the lexical path first so a missing DLL reports a missing file,
        // rather than an invalid canonical path. Canonical checks block links.
        if (QDir::isAbsolutePath(file.key()) ||
            !info.absoluteFilePath().startsWith(root, Qt::CaseInsensitive) ||
            file.key().split('/').contains(QStringLiteral("..")) ||
            file.key().split('\\').contains(QStringLiteral("..")))
            return QCoreApplication::translate("EnhancementRuntime", "Optional runtime manifest contains an invalid path");
        if (!info.exists())
            return QCoreApplication::translate("EnhancementRuntime", "Required runtime file is missing: %1. See the release README.").arg(file.key());
        if (!info.canonicalFilePath().startsWith(root, Qt::CaseInsensitive))
            return QCoreApplication::translate("EnhancementRuntime", "Optional runtime manifest contains an invalid path");
        QJsonArray accepted;
        if (file.value().isString())
            accepted.append(file.value());
        else if (schema == 2 && file.value().isArray())
            accepted = file.value().toArray();
        if (accepted.isEmpty())
            return QCoreApplication::translate("EnhancementRuntime", "Optional runtime manifest has an invalid hash entry");
        for (const auto &pin : accepted)
            if (!pin.isString() || pin.toString().size() != 64)
                return QCoreApplication::translate("EnhancementRuntime", "Optional runtime manifest has an invalid hash entry");
        QFile input(info.absoluteFilePath());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!input.open(QIODevice::ReadOnly) || !hash.addData(&input))
            return QCoreApplication::translate("EnhancementRuntime", "Cannot read optional runtime file: %1").arg(file.key());
        const auto digest = QString::fromLatin1(hash.result().toHex());
        bool matched = false;
        for (const auto &pin : accepted)
            matched |= digest.compare(pin.toString(), Qt::CaseInsensitive) == 0;
        if (!matched)
            return QCoreApplication::translate("EnhancementRuntime", "Optional runtime file does not match this release: %1. Use a listed DLL version.").arg(file.key());
        qInfo().nospace() << "[RTX][Runtime] file=" << file.key() << " sha256=" << digest;
    }
    return {};
}
}
