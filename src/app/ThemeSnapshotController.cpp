#include "app/ThemeSnapshotController.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QFutureWatcher>
#include <QtConcurrentRun>

#include "utils/AppLogger.h"

ThemeSnapshotController::ThemeSnapshotController(QObject* parent)
    : QObject(parent)
    , m_snapshotDirectory(QSharedPointer<QTemporaryDir>::create())
{
}

int ThemeSnapshotController::grabContent(QObject* object)
{
    auto* item = qobject_cast<QQuickItem*>(object);
    QQuickWindow* window = item ? item->window() : nullptr;
    if (!window) {
        failAsync(-1, QStringLiteral("target item is not part of a window"));
        return -1;
    }

    // Capture through the scene graph. See the note below on why the platform
    // screen grab (QScreen::grabWindow) is unusable here.
    const int token = m_nextToken++;
    // Scene-graph capture, NOT QScreen::grabWindow: the platform BitBlt readback
    // returns a pitch-black image whenever DWM promotes the window to a
    // DirectFlip scan-out surface (borderless/maximised/unoccluded). Every grab
    // was a black frame on such a window - the dissolve "cover" was literally a
    // black sheet, which is the mechanism behind both the dark->light black hole
    // and the light->dark hard cut. QQuickWindow::grabWindow() re-renders the
    // live scene graph and cannot be affected by DWM presentation mode.
    const QImage image = window->grabWindow();
    if (image.isNull()) {
        failAsync(token, QStringLiteral("scene-graph grab returned a null image"));
        return token;
    }
    if (!m_snapshotDirectory->isValid()) {
        failAsync(token, QStringLiteral("snapshot directory is unavailable"));
        return token;
    }
    const auto directory = m_snapshotDirectory;
    const auto path = directory->filePath(QStringLiteral("snapshot-%1.jpg").arg(token));
    auto* watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, token, path] {
        const auto saved = watcher->result();
        watcher->deleteLater();
        if (token != m_nextToken - 1) {
            QFile::remove(path);
            return;
        }
        if (!saved) {
            failAsync(token, QStringLiteral("failed to save snapshot"));
            return;
        }
        // Keep the previous generation while an in-flight dissolve displays it.
        if (!m_olderSnapshotPath.isEmpty() && m_olderSnapshotPath != m_lastSnapshotPath)
            QFile::remove(m_olderSnapshotPath);
        m_olderSnapshotPath = m_lastSnapshotPath;
        m_lastSnapshotPath = path;

        emit ready(token, QUrl::fromLocalFile(path));
    });
    watcher->setFuture(QtConcurrent::run([image, path, directory] { return image.save(path, "JPEG", 90); }));
    return token;
}

void ThemeSnapshotController::log(const QString& msg)
{
    AppLogger::info(QStringLiteral("theme"), msg);
}

void ThemeSnapshotController::failAsync(int token, const QString& reason)
{
    AppLogger::warning(QStringLiteral("theme"), QStringLiteral("snapshot failed: %1").arg(reason));
    QTimer::singleShot(0, this, [this, token, reason]() {
        emit failed(token, reason);
    });
}
