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

#include "utils/AppLogger.h"

namespace {
QString snapshotPathFor(int token)
{
    return QDir::temp().filePath(QStringLiteral("vibeplayer-theme-snapshot-%1.jpg").arg(token));
}
}

ThemeSnapshotController::ThemeSnapshotController(QObject* parent)
    : QObject(parent)
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
    const QString path = snapshotPathFor(token);
    if (!image.save(path, "JPEG", 90)) {
        failAsync(token, QStringLiteral("failed to save snapshot to %1").arg(path));
        return token;
    }
    AppLogger::info(QStringLiteral("theme"), QStringLiteral("snapshot %1 captured (%2x%3)")
                        .arg(token).arg(image.width()).arg(image.height()));

    // Keep the previous generation on disk: an in-flight dissolve may still be
    // displaying it, and deleting the file its Image points at can blank the
    // cover mid-fade (observed as a hard cut to the new background). Only the
    // generation before that is safe to drop.
    if (!m_olderSnapshotPath.isEmpty() && m_olderSnapshotPath != m_lastSnapshotPath)
        QFile::remove(m_olderSnapshotPath);
    m_olderSnapshotPath = m_lastSnapshotPath;
    m_lastSnapshotPath = path;

    // Emit asynchronously so callers finish their current turn first.
    QTimer::singleShot(0, this, [this, token, path]() {
        emit ready(token, QUrl::fromLocalFile(path));
    });
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
