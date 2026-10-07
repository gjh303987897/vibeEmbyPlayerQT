#include "services/management/WorkflowSettings.h"
#include "services/encryptedhls/EncryptedHlsBatchPackager.h"
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <cmath>

namespace {
const QVariantMap& defaults()
{
    static const QVariantMap values {
        {"desktop/minimizeToTray",true},
        {"appearance/themeMode","dark"}, {"appearance/languageMode","system"},
        {"appearance/embyHomeLayout","trendy"}, {"appearance/jellyfinHomeLayout","trendy"},
        {"appearance/playerLayout","trendy"}, {"appearance/serviceCardSizePercent",100},
        {"appearance/pageTransitionsEnabled",true}, {"history/retentionDays",30},
        {"webdav/showM3u8sIdentifier",true}, {"webdav/showM3u8sSourceFileName",true},
        {"updates/channel","stable"}, {"updates/automaticCheck",true},
        {"player/volume",100}, {"player/speed","1.0"}, {"player/subtitleFile",""},
        {"webdav/defaultDownloadDirectory", ""},
        {"m3u8s/outputDirectory", ""}, {"m3u8s/ffmpegExecutable", ""},
        {"m3u8s/outputMode", "local"}, {"m3u8s/webDavServiceId", ""}, {"m3u8s/webDavPath", ""},
        {"m3u8s/fallbackDirectory", ""}, {"m3u8s/temporaryDirectory", ""},
        {"m3u8s/keepSuccessfulLocal", false}, {"m3u8s/videoEncoding", "h264"},
        {"m3u8s/autoVideoCodecs", QStringList {"h264", "h265"}}, {"m3u8s/autoVideoTarget", "h264"},
        {"m3u8s/audioEncoding", "aac"}, {"m3u8s/videoQuality", "balanced"},
        {"m3u8s/containerFormat", "m3u8sp"}, {"m3u8s/parallelJobs", 1},
        {"encryptedHlsRestore/outputDirectory", ""}, {"encryptedHlsRestore/format", "original"},
        {"tsslBackup/target", "none"}, {"tsslBackup/localPath", ""}, {"tsslBackup/webDavServiceId", ""},
        {"tsslBackup/webDavPath", "vibePlayerQT/tssl"}, {"tsslBackup/s3Endpoint", ""},
        {"tsslBackup/s3Bucket", ""}, {"tsslBackup/s3Region", "us-east-1"},
        {"tsslBackup/s3Prefix", "vibePlayerQT/tssl"}, {"tsslBackup/s3AccessKey", ""},
    };
    return values;
}
}

QVariantMap WorkflowSettings::snapshot()
{
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("vibePlayerQT"), QStringLiteral("vibePlayerQT"));
    if (QSettings::defaultFormat() == QSettings::IniFormat) settings.setFallbacksEnabled(false);
    settings.sync();
    auto values = defaults();
    for (auto it = values.begin(); it != values.end(); ++it) {
        auto value = settings.value(it.key(),it.value());
        // INI scalar values are read as strings; preserve the public schema types.
        if (it.value().metaType().id() == QMetaType::Int) value = value.toInt();
        else if (it.value().metaType().id() == QMetaType::Bool) value = value.toBool();
        else if (it.value().metaType().id() == QMetaType::QStringList) value = value.toStringList();
        it.value() = value;
    }
    return values;
}

std::expected<void, QString> WorkflowSettings::validate(const QString& key, QVariant& value)
{
    if (!defaults().contains(key)) return std::unexpected(QStringLiteral("Unknown or unsupported configuration key"));
    static const QMap<QString, QStringList> choices {
        {"appearance/themeMode",{"dark","light","system"}},
        {"appearance/languageMode",{"system","zh_CN","en_US"}},
        {"appearance/embyHomeLayout",{"trendy","traditional"}},
        {"appearance/jellyfinHomeLayout",{"trendy","traditional"}},
        {"appearance/playerLayout",{"trendy","traditional"}},
        {"updates/channel",{"stable","beta"}},
        {"m3u8s/outputMode", {"local", "webdav"}}, {"m3u8s/videoEncoding", {"copy", "h264", "h265", "auto"}},
        {"m3u8s/autoVideoTarget", {"h264", "h265"}}, {"m3u8s/audioEncoding", {"copy", "aac"}},
        {"m3u8s/videoQuality", {"high", "balanced", "compact"}},
        {"m3u8s/containerFormat", {"m3u8s", "m3u8sp"}},
        {"encryptedHlsRestore/format", {"original", "mkv", "mp4"}},
        {"tsslBackup/target", {"none", "local", "webdav", "s3"}},
    };
    if (choices.contains(key) && !choices.value(key).contains(value.toString()))
        return std::unexpected(QStringLiteral("Invalid value for %1; expected %2").arg(key, choices.value(key).join(", ")));
    if (key == QStringLiteral("m3u8s/parallelJobs")) {
        bool ok = false;
        const auto number = value.toInt(&ok);
        if (!ok || number < 1 || number > EncryptedHlsBatchPackager::maximumParallelJobs())
            return std::unexpected(QStringLiteral("Invalid parallel job count"));
        value = number;
    } else if (defaults().value(key).metaType().id() == QMetaType::Bool) {
        const auto text = value.toString().toLower();
        if (text != "true" && text != "false") return std::unexpected(QStringLiteral("Expected true or false"));
        value = text == "true";
    } else if (defaults().value(key).metaType().id() == QMetaType::Int) {
        bool ok = false; const auto number = value.toInt(&ok);
        const int minimum = key == "appearance/serviceCardSizePercent" ? 50 : key == "history/retentionDays" ? 1 : 0;
        const int maximum = key == "appearance/serviceCardSizePercent" ? 150 : key == "history/retentionDays" ? 3650 : 100;
        if (!ok || number < minimum || number > maximum) return std::unexpected(QStringLiteral("Value outside the supported range"));
        value = number;
    } else if (key == "player/speed") {
        bool ok = false; const auto speed = value.toDouble(&ok);
        if (!ok || !std::isfinite(speed) || speed < 0.5 || speed > 5.0)
            return std::unexpected(QStringLiteral("Speed must be between 0.5 and 5"));
        value = QString::number(speed);
    } else if (key == "player/subtitleFile" && !value.toString().isEmpty()) {
        const QFileInfo subtitle(value.toString());
        if (!subtitle.isFile()) return std::unexpected(QStringLiteral("Subtitle file does not exist"));
        value = subtitle.absoluteFilePath();
    } else if (key == QStringLiteral("m3u8s/autoVideoCodecs")) {
        const auto codecs = value.metaType().id() == QMetaType::QStringList
            ? value.toStringList() : value.toString().split(',', Qt::SkipEmptyParts);
        if (codecs.isEmpty()) return std::unexpected(QStringLiteral("At least one codec is required"));
        value = codecs;
    } else if (key.endsWith("Directory") || key == QStringLiteral("tsslBackup/localPath") ||
               key == QStringLiteral("m3u8s/ffmpegExecutable")) {
        if (!value.toString().isEmpty()) value = QFileInfo(value.toString()).absoluteFilePath();
    } else if (key == QStringLiteral("tsslBackup/s3Endpoint") && !value.toString().isEmpty()) {
        const QUrl url(value.toString());
        if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
            (url.scheme() != "http" && url.scheme() != "https"))
            return std::unexpected(QStringLiteral("Invalid S3 HTTP(S) endpoint"));
    }
    return {};
}

std::expected<void, QString> WorkflowSettings::set(const QString& key, const QString& text)
{
    QVariant value(text);
    if (auto result = validate(key, value); !result) return result;
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("vibePlayerQT"), QStringLiteral("vibePlayerQT"));
    if (QSettings::defaultFormat() == QSettings::IniFormat) settings.setFallbacksEnabled(false);
    settings.setValue(key, value);
    settings.sync();
    if (settings.status() != QSettings::NoError) return std::unexpected(QStringLiteral("Unable to save configuration"));
    return {};
}

void WorkflowSettings::applyPackaging(const QVariantMap& values, EncryptedHlsPackageRequest& request)
{
    const auto video = values.value("m3u8s/videoEncoding").toString();
    request.videoEncoding = video == "copy" ? EncryptedHlsVideoEncoding::Copy :
        video == "h265" ? EncryptedHlsVideoEncoding::H265 :
        video == "auto" ? EncryptedHlsVideoEncoding::Auto : EncryptedHlsVideoEncoding::H264;
    request.autoCopyVideoCodecs = values.value("m3u8s/autoVideoCodecs").toStringList();
    request.autoFallbackVideoEncoding = values.value("m3u8s/autoVideoTarget") == "h265"
        ? EncryptedHlsVideoEncoding::H265 : EncryptedHlsVideoEncoding::H264;
    request.audioEncoding = values.value("m3u8s/audioEncoding") == "copy"
        ? EncryptedHlsAudioEncoding::Copy : EncryptedHlsAudioEncoding::Aac;
    const auto quality = values.value("m3u8s/videoQuality").toString();
    request.videoQuality = quality == "high" ? EncryptedHlsVideoQuality::High :
        quality == "compact" ? EncryptedHlsVideoQuality::Compact : EncryptedHlsVideoQuality::Balanced;
    request.containerFormat = values.value("m3u8s/containerFormat") == "m3u8s"
        ? EncryptedHlsContainerFormat::DirectoryM3u8s : EncryptedHlsContainerFormat::TarM3u8sp;
    request.temporaryDirectory = values.value("m3u8s/temporaryDirectory").toString();
}

QString WorkflowSettings::defaultOutputDirectory()
{
    auto root = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (root.isEmpty()) root = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QDir(root).filePath(QStringLiteral("vibePlayerQT/M3U8S"));
}
