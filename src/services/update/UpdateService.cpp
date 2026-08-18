#include "services/update/UpdateService.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>

#include <algorithm>

namespace {
const QUrl releasesUrl(QStringLiteral("https://api.github.com/repos/gjh303987897/vibeEmbyPlayerQT/releases?per_page=100"));

bool isNumericIdentifier(const QString& value)
{
    if (value.isEmpty()) return false;
    for (const auto character : value) {
        if (!character.isDigit()) return false;
    }
    return true;
}

int compareIdentifier(const QString& left, const QString& right)
{
    const bool leftNumeric = isNumericIdentifier(left);
    const bool rightNumeric = isNumericIdentifier(right);
    if (leftNumeric && rightNumeric) {
        const auto leftNumber = left.toULongLong();
        const auto rightNumber = right.toULongLong();
        return leftNumber < rightNumber ? -1 : leftNumber > rightNumber ? 1 : 0;
    }
    if (leftNumeric != rightNumeric) return leftNumeric ? -1 : 1;
    return left < right ? -1 : left > right ? 1 : 0;
}

QString platformToken()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

QString architectureToken()
{
    const auto arch = QSysInfo::currentCpuArchitecture().toLower();
    return arch == QStringLiteral("arm64") || arch == QStringLiteral("aarch64")
        ? QStringLiteral("arm64") : QStringLiteral("x86_64");
}

bool isPackageAsset(const QString& name)
{
    const auto lower = name.toLower();
    return !lower.endsWith(QStringLiteral(".sha256"))
        && (lower.endsWith(QStringLiteral(".exe")) || lower.endsWith(QStringLiteral(".zip"))
            || lower.endsWith(QStringLiteral(".dmg")) || lower.endsWith(QStringLiteral(".appimage"))
            || lower.endsWith(QStringLiteral(".deb")) || lower.endsWith(QStringLiteral(".rpm"))
            || lower.endsWith(QStringLiteral(".flatpak")) || lower.endsWith(QStringLiteral(".tar.gz")));
}
}

QString SemVersion::toString() const
{
    if (!valid) return {};
    auto result = QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
    if (!prerelease.isEmpty()) result += QLatin1Char('-') + prerelease.join(QLatin1Char('.'));
    return result;
}

UpdateService::UpdateService(QObject* parent) : QObject(parent) { }

std::optional<SemVersion> UpdateService::parseVersion(const QString& value)
{
    auto text = value.trimmed();
    if (text.startsWith(QLatin1Char('v'))) text.remove(0, 1);
    const auto buildParts = text.split(QLatin1Char('+'));
    if (buildParts.size() > 2) return std::nullopt;
    const auto versionParts = buildParts.front().split(QLatin1Char('-'));
    if (versionParts.size() > 2) return std::nullopt;
    const auto core = versionParts.front().split(QLatin1Char('.'));
    if (core.size() != 3) return std::nullopt;
    bool majorOk = false, minorOk = false, patchOk = false;
    SemVersion result;
    result.major = core[0].toInt(&majorOk);
    result.minor = core[1].toInt(&minorOk);
    result.patch = core[2].toInt(&patchOk);
    if (!majorOk || !minorOk || !patchOk || result.major < 0 || result.minor < 0 || result.patch < 0
        || (core[0].size() > 1 && core[0].startsWith('0'))
        || (core[1].size() > 1 && core[1].startsWith('0'))
        || (core[2].size() > 1 && core[2].startsWith('0'))) return std::nullopt;
    if (versionParts.size() == 2) {
        result.prerelease = versionParts[1].split(QLatin1Char('.'));
        for (const auto& identifier : result.prerelease) {
            if (identifier.isEmpty()
                || !QRegularExpression(QStringLiteral("^[0-9A-Za-z-]+$")).match(identifier).hasMatch()
                || (identifier.size() > 1 && identifier.startsWith('0') && isNumericIdentifier(identifier))) return std::nullopt;
        }
    }
    result.valid = true;
    return result;
}

int UpdateService::compareVersions(const SemVersion& left, const SemVersion& right)
{
    if (left.major != right.major) return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor) return left.minor < right.minor ? -1 : 1;
    if (left.patch != right.patch) return left.patch < right.patch ? -1 : 1;
    if (left.prerelease.isEmpty() && right.prerelease.isEmpty()) return 0;
    if (left.prerelease.isEmpty()) return 1;
    if (right.prerelease.isEmpty()) return -1;
    const auto count = std::min(left.prerelease.size(), right.prerelease.size());
    for (int i = 0; i < count; ++i) {
        const auto comparison = compareIdentifier(left.prerelease[i], right.prerelease[i]);
        if (comparison != 0) return comparison;
    }
    return left.prerelease.size() < right.prerelease.size() ? -1 : left.prerelease.size() > right.prerelease.size() ? 1 : 0;
}

bool UpdateService::channelAccepts(const SemVersion& version, UpdateChannel channel)
{
    const auto classified = classifyVersion(version);
    return classified && *classified == channel;
}

std::optional<UpdateChannel> UpdateService::classifyVersion(const SemVersion& version)
{
    if (!version.valid) return std::nullopt;
    if (version.prerelease.size() != 1 && version.prerelease.size() != 2) return std::nullopt;
    const auto& label = version.prerelease.front();
    const auto channel = label == QStringLiteral("alpha") ? UpdateChannel::Alpha
        : label == QStringLiteral("beta") ? UpdateChannel::Beta
        : label == QStringLiteral("stable") ? UpdateChannel::Stable : UpdateChannel::Alpha;
    if (label != QStringLiteral("alpha") && label != QStringLiteral("beta") && label != QStringLiteral("stable")) return std::nullopt;
    if (channel == UpdateChannel::Stable && version.prerelease.size() != 1) return std::nullopt;
    if (version.prerelease.size() == 2 && !isNumericIdentifier(version.prerelease[1])) return std::nullopt;
    return channel;
}

std::optional<UpdateChannel> UpdateService::channelFromString(const QString& value)
{
    const auto lower = value.trimmed().toLower();
    if (lower == QStringLiteral("stable")) return UpdateChannel::Stable;
    if (lower == QStringLiteral("beta")) return UpdateChannel::Beta;
    if (lower == QStringLiteral("alpha")) return UpdateChannel::Alpha;
    return std::nullopt;
}

QString UpdateService::channelToString(UpdateChannel channel)
{
    return channel == UpdateChannel::Stable ? QStringLiteral("stable")
        : channel == UpdateChannel::Beta ? QStringLiteral("beta") : QStringLiteral("alpha");
}

std::optional<QString> UpdateService::parseChecksum(const QByteArray& content, const QString& fileName)
{
    const auto line = QString::fromUtf8(content).trimmed();
    const auto match = QRegularExpression(QStringLiteral("^([0-9A-Fa-f]{64})\\s{2}(.+)$")).match(line);
    if (!match.hasMatch() || match.captured(2).trimmed() != fileName) return std::nullopt;
    return match.captured(1).toLower();
}

std::optional<UpdateRelease> UpdateService::parseReleases(const QByteArray& content,
                                                           UpdateChannel channel,
                                                           const SemVersion& currentVersion)
{
    const auto document = QJsonDocument::fromJson(content);
    if (!document.isArray()) return std::nullopt;
    std::optional<UpdateRelease> best;
    for (const auto& value : document.array()) {
        const auto object = value.toObject();
        if (object.value(QStringLiteral("draft")).toBool()) continue;
        const auto tagName = object.value(QStringLiteral("tag_name")).toString();
        if (!tagName.startsWith(QLatin1Char('v'))) continue;
        const auto parsed = parseVersion(tagName);
        if (!parsed || !channelAccepts(*parsed, channel)
            || (currentVersion.valid && compareVersions(*parsed, currentVersion) <= 0)) continue;
        if (best && compareVersions(*parsed, best->version) <= 0) continue;
        UpdateRelease release;
        release.version = *parsed;
        release.tagName = tagName;
        release.notes = object.value(QStringLiteral("body")).toString();
        release.publishedAt = QDateTime::fromString(object.value(QStringLiteral("published_at")).toString(), Qt::ISODate);
        QHash<QString, QUrl> checksums;
        QList<UpdateAsset> allAssets;
        for (const auto& assetValue : object.value(QStringLiteral("assets")).toArray()) {
            const auto assetObject = assetValue.toObject();
            UpdateAsset asset;
            asset.name = assetObject.value(QStringLiteral("name")).toString();
            asset.downloadUrl = QUrl(assetObject.value(QStringLiteral("browser_download_url")).toString());
            asset.size = assetObject.value(QStringLiteral("size")).toInteger();
            if (asset.name.isEmpty() || !asset.downloadUrl.isValid()) continue;
            allAssets.append(asset);
            if (asset.name.endsWith(QStringLiteral(".sha256"))) checksums.insert(asset.name, asset.downloadUrl);
        }
        for (auto asset : allAssets) {
            if (!isPackageAsset(asset.name)) continue;
            asset.checksumName = asset.name + QStringLiteral(".sha256");
            asset.checksumUrl = checksums.value(asset.checksumName);
            asset.checksumAvailable = asset.checksumUrl.isValid();
            release.assets.append(asset);
        }
        release.assets = selectPlatformAssets(release.assets);
        best = release;
    }
    return best;
}

QList<UpdateAsset> UpdateService::selectPlatformAssets(const QList<UpdateAsset>& assets)
{
    const auto prefix = platformToken() + QLatin1Char('-') + architectureToken();
    const auto priority = [](const QString& name) {
        const auto lower = name.toLower();
        if (platformToken() == QStringLiteral("windows")) {
            if (lower.endsWith(QStringLiteral("-installer.exe"))) return 0;
            if (lower.endsWith(QStringLiteral(".zip"))) return 1;
            if (lower.endsWith(QStringLiteral(".msi"))) return 2;
        } else if (platformToken() == QStringLiteral("macos")) {
            if (lower.endsWith(QStringLiteral(".dmg"))) return 0;
            if (lower.endsWith(QStringLiteral(".zip"))) return 1;
        } else {
            if (lower.endsWith(QStringLiteral(".appimage"))) return 0;
            if (lower.endsWith(QStringLiteral(".deb"))) return 1;
            if (lower.endsWith(QStringLiteral(".rpm"))) return 2;
            if (lower.endsWith(QStringLiteral(".flatpak"))) return 3;
        }
        return 99;
    };
    QList<UpdateAsset> result;
    for (auto asset : assets) {
        if (!asset.name.contains(prefix, Qt::CaseInsensitive)) continue;
        const auto lower = asset.name.toLower();
        asset.preferred = priority(asset.name) < 99;
        result.append(asset);
    }
    std::stable_sort(result.begin(), result.end(), [](const UpdateAsset& left, const UpdateAsset& right) {
        const auto rank = [](const UpdateAsset& asset) {
            const auto lower = asset.name.toLower();
            if (lower.endsWith(QStringLiteral("-installer.exe"))) return 0;
            if (lower.endsWith(QStringLiteral(".appimage")) || lower.endsWith(QStringLiteral(".dmg"))) return 0;
            if (lower.endsWith(QStringLiteral(".zip"))) return 1;
            if (lower.endsWith(QStringLiteral(".deb"))) return 1;
            if (lower.endsWith(QStringLiteral(".rpm"))) return 2;
            if (lower.endsWith(QStringLiteral(".msi"))) return 2;
            if (lower.endsWith(QStringLiteral(".flatpak"))) return 3;
            return 99;
        };
        return rank(left) < rank(right);
    });
    return result;
}

void UpdateService::check(UpdateChannel channel, const SemVersion& currentVersion, const QByteArray& etag)
{
    if (m_checkReply) m_checkReply->abort();
    QNetworkRequest request(releasesUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT-update-checker"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    if (!etag.isEmpty()) request.setRawHeader("If-None-Match", etag);
    m_checkReply = m_manager.get(request);
    connect(m_checkReply, &QNetworkReply::finished, this, [this, channel, currentVersion]() {
        auto* reply = m_checkReply;
        m_checkReply = nullptr;
        UpdateCheckResult result;
        const auto responseEtag = reply->rawHeader("ETag");
        result.notModified = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 304;
        if (reply->error() == QNetworkReply::NoError || result.notModified) {
            result.success = true;
            if (!result.notModified) result.release = parseReleases(reply->readAll(), channel, currentVersion);
        } else {
            result.error = reply->errorString();
        }
        reply->deleteLater();
        emit checkFinished(result, responseEtag);
    });
}

void UpdateService::download(const UpdateAsset& asset)
{
    cancelDownload();
    if (!asset.downloadUrl.isValid() || !asset.checksumAvailable) {
        emit downloadFailed(QStringLiteral("Missing or invalid SHA-256 sidecar"));
        return;
    }
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/vibePlayerQT-updates");
    QDir().mkpath(directory);
    m_downloadPath = directory + QLatin1Char('/') + QFileInfo(asset.name).fileName() + QStringLiteral(".part");
    m_downloadAsset = asset;
    m_downloadFile.setFileName(m_downloadPath);
    if (!m_downloadFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit downloadFailed(QStringLiteral("Unable to create temporary update file"));
        return;
    }
    QNetworkRequest request(asset.downloadUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT-update-downloader"));
    request.setRawHeader("Accept", "application/octet-stream");
    m_downloadReply = m_manager.get(request);
    emit downloadStateChanged(true);
    connect(m_downloadReply, &QNetworkReply::readyRead, this, [this]() { m_downloadFile.write(m_downloadReply->readAll()); });
    connect(m_downloadReply, &QNetworkReply::downloadProgress, this, &UpdateService::downloadProgress);
    connect(m_downloadReply, &QNetworkReply::finished, this, [this]() {
        if (!m_downloadReply) return;
        auto* reply = m_downloadReply;
        m_downloadReply = nullptr;
        m_downloadFile.write(reply->readAll());
        m_downloadFile.flush();
        m_downloadFile.close();
        const auto error = reply->errorString();
        const auto failed = reply->error() != QNetworkReply::NoError;
        reply->deleteLater();
        if (failed) { finishDownloadWithError(error); return; }
        if (m_downloadAsset.size > 0 && QFileInfo(m_downloadPath).size() != m_downloadAsset.size) {
            finishDownloadWithError(QStringLiteral("Downloaded file size does not match release metadata"));
            return;
        }
        downloadChecksum(m_downloadAsset, m_downloadPath);
    });
}

void UpdateService::downloadChecksum(const UpdateAsset& asset, const QString& packagePath)
{
    QNetworkRequest request(asset.checksumUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT-update-downloader"));
    m_checksumReply = m_manager.get(request);
    connect(m_checksumReply, &QNetworkReply::finished, this, [this, packagePath]() {
        if (!m_checksumReply) return;
        auto* reply = m_checksumReply;
        m_checksumReply = nullptr;
        const auto content = reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray {};
        const auto error = reply->errorString();
        reply->deleteLater();
        if (content.isEmpty()) { finishDownloadWithError(error.isEmpty() ? QStringLiteral("Unable to download SHA-256 sidecar") : error); return; }
        if (!verifyPackage(packagePath, content, m_downloadAsset.name)) { finishDownloadWithError(QStringLiteral("SHA-256 verification failed")); return; }
        const auto finalPath = packagePath.left(packagePath.size() - QStringLiteral(".part").size());
        QFile::remove(finalPath);
        if (!QFile::rename(packagePath, finalPath)) { finishDownloadWithError(QStringLiteral("Unable to finalize update file")); return; }
#if !defined(Q_OS_WIN)
        if (finalPath.endsWith(QStringLiteral(".AppImage"), Qt::CaseInsensitive)) {
            QFile::setPermissions(finalPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther);
        }
#endif
        emit downloadStateChanged(false);
        emit downloadFinished(finalPath);
    });
}

bool UpdateService::verifyPackage(const QString& packagePath, const QByteArray& checksumContent, const QString& fileName)
{
    const auto expected = parseChecksum(checksumContent, fileName);
    if (!expected) return false;
    QFile file(packagePath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) hash.addData(file.read(1024 * 1024));
    return QString::fromLatin1(hash.result().toHex()) == *expected;
}

void UpdateService::cancelDownload()
{
    const bool active = m_downloadReply || m_checksumReply || m_downloadFile.isOpen();
    if (m_downloadReply) m_downloadReply->abort();
    if (m_checksumReply) m_checksumReply->abort();
    if (m_downloadFile.isOpen()) m_downloadFile.close();
    if (!m_downloadPath.isEmpty()) QFile::remove(m_downloadPath);
    m_downloadReply = nullptr;
    m_checksumReply = nullptr;
    m_downloadPath.clear();
    if (active) emit downloadStateChanged(false);
}

void UpdateService::finishDownloadWithError(const QString& error)
{
    if (m_downloadFile.isOpen()) m_downloadFile.close();
    if (!m_downloadPath.isEmpty()) QFile::remove(m_downloadPath);
    m_downloadPath.clear();
    emit downloadStateChanged(false);
    emit downloadFailed(error);
}
