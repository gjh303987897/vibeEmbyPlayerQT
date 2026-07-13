#include "services/webdav/WebDavDownloadPlanner.h"

#include <QDir>
#include <QFileInfo>
#include <QQueue>
#include <QSet>

#include <algorithm>
#include <limits>
#include <memory>

namespace {
constexpr auto maxConcurrentDirectoryRequests = 4;

struct PendingDirectory {
    QUrl remoteUrl;
    QString localPath;
};

struct PlanningState {
    QQueue<PendingDirectory> pendingDirectories;
    QSet<QString> visitedRemoteDirectories;
    QSet<QString> reservedLocalPaths;
    WebDavDownloadPlan plan;
    int activeRequests { 0 };
    bool finished { false };
    std::function<void(WebDavDownloadPlanResult)> callback;
};

QUrl ensureDirectoryUrl(QUrl url)
{
    auto path = url.path();
    if (!path.endsWith(QLatin1Char('/'))) {
        path.append(QLatin1Char('/'));
        url.setPath(path);
    }
    return url;
}

QString remoteDirectoryKey(const QUrl& url)
{
    return ensureDirectoryUrl(url).adjusted(QUrl::NormalizePathSegments).toString(QUrl::FullyEncoded);
}

QString localPathKey(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

QString safeLocalName(QString name)
{
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    name = name.section(QLatin1Char('/'), -1).trimmed();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        name = QStringLiteral("unnamed");
    }

    static const QString invalidCharacters = QStringLiteral("<>:\"/\\|?*");
    for (auto& character : name) {
        if (character.unicode() < 0x20 || invalidCharacters.contains(character)) {
            character = QLatin1Char('_');
        }
    }
    while (name.endsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char('.'))) {
        name.chop(1);
    }
    return name.isEmpty() ? QStringLiteral("unnamed") : name;
}

QString reserveLocalPath(const QString& directory,
                         const QString& rawName,
                         bool directoryEntry,
                         QSet<QString>& reservedPaths)
{
    const auto safeName = safeLocalName(rawName);
    const QFileInfo original(QDir(directory).filePath(safeName));

    auto reserve = [&reservedPaths](const QString& path) {
        const auto key = localPathKey(path);
        if (reservedPaths.contains(key) || QFileInfo::exists(path)) {
            return false;
        }
        reservedPaths.insert(key);
        return true;
    };

    if (reserve(original.absoluteFilePath())) {
        return original.absoluteFilePath();
    }

    const auto base = directoryEntry ? safeName : original.completeBaseName();
    const auto suffix = directoryEntry ? QString {} : original.suffix();
    for (auto index = 1; index < 10000; ++index) {
        const auto candidateName = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(base).arg(index)
            : QStringLiteral("%1 (%2).%3").arg(base).arg(index).arg(suffix);
        const auto candidate = QFileInfo(QDir(directory).filePath(candidateName)).absoluteFilePath();
        if (reserve(candidate)) {
            return candidate;
        }
    }
    return original.absoluteFilePath();
}
}

WebDavDownloadPlanner::WebDavDownloadPlanner(WebDavClient& client)
    : m_client(client)
{
}

void WebDavDownloadPlanner::buildPlan(const ServerConfig& server,
                                      const QString& password,
                                      const WebDavItem& rootItem,
                                      const QString& targetPath,
                                      std::function<void(WebDavDownloadPlanResult)> callback)
{
    if (!rootItem.directory) {
        WebDavDownloadPlan plan;
        plan.files.push_back(WebDavDownloadEntry {
            .remoteUrl = rootItem.url,
            .localPath = targetPath,
            .bytesTotal = rootItem.size,
        });
        plan.bytesTotal = std::max<qint64>(0, rootItem.size);
        plan.sizeComplete = rootItem.size >= 0;
        callback(std::move(plan));
        return;
    }

    auto state = std::make_shared<PlanningState>();
    state->callback = std::move(callback);
    state->plan.directories.push_back(targetPath);
    state->reservedLocalPaths.insert(localPathKey(targetPath));
    state->pendingDirectories.enqueue(PendingDirectory {
        .remoteUrl = ensureDirectoryUrl(rootItem.url),
        .localPath = targetPath,
    });
    state->visitedRemoteDirectories.insert(remoteDirectoryKey(rootItem.url));

    auto finishSuccess = [state]() mutable {
        if (state->finished) {
            return;
        }
        state->finished = true;
        auto done = std::move(state->callback);
        done(std::move(state->plan));
    };
    auto finishError = [state](NetworkError error) mutable {
        if (state->finished) {
            return;
        }
        state->finished = true;
        auto done = std::move(state->callback);
        done(std::unexpected(std::move(error)));
    };

    auto pump = std::make_shared<std::function<void()>>();
    const std::weak_ptr<std::function<void()>> weakPump = pump;
    *pump = [this, server, password, state, weakPump, finishSuccess, finishError]() mutable {
        const auto currentPump = weakPump.lock();
        if (!currentPump || state->finished) {
            return;
        }

        while (state->activeRequests < maxConcurrentDirectoryRequests && !state->pendingDirectories.isEmpty()) {
            const auto pending = state->pendingDirectories.dequeue();
            ++state->activeRequests;
            m_client.listDirectory(server,
                                   password,
                                   pending.remoteUrl,
                                   [state, currentPump, pending, finishError](WebDavListResult result) mutable {
                if (state->finished) {
                    return;
                }
                --state->activeRequests;
                if (!result) {
                    finishError(result.error());
                    return;
                }

                for (const auto& child : *result) {
                    const auto childPath = reserveLocalPath(pending.localPath,
                                                            child.name,
                                                            child.directory,
                                                            state->reservedLocalPaths);
                    if (child.directory) {
                        const auto directoryKey = remoteDirectoryKey(child.url);
                        if (state->visitedRemoteDirectories.contains(directoryKey)) {
                            continue;
                        }
                        state->visitedRemoteDirectories.insert(directoryKey);
                        state->plan.directories.push_back(childPath);
                        state->pendingDirectories.enqueue(PendingDirectory {
                            .remoteUrl = ensureDirectoryUrl(child.url),
                            .localPath = childPath,
                        });
                        continue;
                    }

                    state->plan.files.push_back(WebDavDownloadEntry {
                        .remoteUrl = child.url,
                        .localPath = childPath,
                        .bytesTotal = child.size,
                    });
                    if (child.size < 0 ||
                        state->plan.bytesTotal > std::numeric_limits<qint64>::max() - child.size) {
                        state->plan.sizeComplete = false;
                    } else {
                        state->plan.bytesTotal += child.size;
                    }
                }
                (*currentPump)();
            });
        }

        if (state->activeRequests == 0 && state->pendingDirectories.isEmpty()) {
            finishSuccess();
        }
    };
    (*pump)();
}
