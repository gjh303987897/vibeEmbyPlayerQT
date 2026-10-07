#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QSharedPointer>
#include <QTemporaryDir>

// One-shot window snapshot for the theme-switch dissolve (VIBEDOCS/ThemeTransition.md).
//
// Both QML-side grab paths are unusable here: Item.grabToImage refuses to run on
// the window's internal contentItem ("item has no QML engine"), and the C++ offscreen
// capture must use QQuickWindow::grabWindow to avoid black DirectFlip readback.
// Image encoding and writing run on a worker; QML receives a file URL to fade out.
class ThemeSnapshotController final : public QObject {
    Q_OBJECT

public:
    explicit ThemeSnapshotController(QObject* parent = nullptr);

    // Grabs the window containing `item`. Returns the request token (results and
    // failures always arrive asynchronously via ready/failed on the next turn).
    Q_INVOKABLE int grabContent(QObject* item);

    // TEMP DIAG: forwards QML telemetry into the app log (VIBEPLAYER_LOG_FILE).
    Q_INVOKABLE void log(const QString& msg);

signals:
    void ready(int token, const QUrl& url);
    void failed(int token, const QString& reason);

private:
    void failAsync(int token, const QString& reason);

    QString m_lastSnapshotPath;
    QString m_olderSnapshotPath;
    int m_nextToken = 1;
    QSharedPointer<QTemporaryDir> m_snapshotDirectory;
};
