#include "services/smb/SmbClient.h"
#include <QDir>
#include <QUrl>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winnetwk.h>
#endif

std::expected<QString,QString> SmbClient::path(const ServerConfig& server, const QString& virtualPath)
{
    const QUrl url(server.baseUrl);
    if (url.scheme() != "smb" || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        url.hasQuery() || url.hasFragment() || url.port() != -1)
        return std::unexpected(QStringLiteral("Invalid SMB service URL"));
    const auto rootParts = url.path().split('/',Qt::SkipEmptyParts);
    if (rootParts.isEmpty()) return std::unexpected(QStringLiteral("An SMB share is required"));
    static const QRegularExpression device("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$",
                                           QRegularExpression::CaseInsensitiveOption);
    const auto validSegment = [&](const QString& part) {
        return !part.isEmpty() && !part.endsWith('.') && !part.endsWith(' ') &&
            !device.match(part).hasMatch() && std::ranges::none_of(part,[](QChar ch) {
                return ch.unicode() < 32 || QStringLiteral("\\:*?\"<>|").contains(ch);
            });
    };
    for (const auto& part : rootParts)
        if (part == "." || part == ".." || !validSegment(part))
            return std::unexpected(QStringLiteral("Invalid SMB root path"));
    QStringList parts;
    for (const auto& part : virtualPath.split('/',Qt::SkipEmptyParts)) {
        if (part == ".") continue;
        if (part == "..") {
            if (parts.isEmpty()) return std::unexpected(QStringLiteral("Path escapes the SMB service root"));
            parts.removeLast();
        } else {
            if (!validSegment(part))
                return std::unexpected(QStringLiteral("Invalid SMB path segment"));
            parts += part;
        }
    }
#ifdef Q_OS_WIN
    const auto root = "//" + url.host() + url.path();
    return QDir::toNativeSeparators(QDir::cleanPath(root + '/' + parts.join('/')));
#else
    // Native SMB URLs require a platform backend; an OS-mounted share is usable via ordinary file commands.
    return std::unexpected(QStringLiteral("Direct SMB URLs currently require Windows; mount the share on this platform"));
#endif
}

bool SmbClient::hasLinkedAncestor(const QString& path)
{
    auto current = QFileInfo(path).absoluteFilePath();
    while (true) {
        const QFileInfo info(current);
        if (info.isSymLink() || info.isJunction()) return true;
        const auto parent = info.absolutePath();
        if (parent == current) return false;
        current = parent;
    }
}

std::expected<void,QString> SmbClient::connect(const ServerConfig& server, const QString& password)
{
#ifdef Q_OS_WIN
    const QUrl url(server.baseUrl);
    auto share = QStringLiteral("\\\\") + url.host() + '\\' + url.path().split('/',Qt::SkipEmptyParts).value(0);
    NETRESOURCEW resource {};
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = reinterpret_cast<LPWSTR>(share.data());
    const auto result = WNetAddConnection2W(&resource,reinterpret_cast<LPCWSTR>(password.utf16()),
        server.username.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(server.username.utf16()),CONNECT_TEMPORARY);
    if (result != NO_ERROR) return std::unexpected(QStringLiteral("SMB connection failed (Windows error %1)").arg(result));
    return {};
#else
    Q_UNUSED(server); Q_UNUSED(password);
    return std::unexpected(QStringLiteral("Direct SMB authentication currently requires Windows"));
#endif
}

std::expected<void,QString> SmbClient::move(const QString& source, const QString& destination, bool overwrite)
{
    if (hasLinkedAncestor(source) || hasLinkedAncestor(destination) ||
        (QFileInfo(destination).isDir() && overwrite))
        return std::unexpected(QStringLiteral("SMB move does not replace directories or follow links"));
#ifdef Q_OS_WIN
    const auto from = QDir::toNativeSeparators(source), to = QDir::toNativeSeparators(destination);
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()),reinterpret_cast<LPCWSTR>(to.utf16()),
                    MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)))
        return std::unexpected(QStringLiteral("SMB move failed (Windows error %1)").arg(GetLastError()));
    return {};
#else
    Q_UNUSED(overwrite);
    return QDir().rename(source,destination) ? std::expected<void,QString>() :
           std::unexpected(QStringLiteral("SMB move failed"));
#endif
}
