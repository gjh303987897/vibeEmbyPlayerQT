#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

class QQuickWindow;

class WindowAppearanceController final : public QObject {
    Q_OBJECT

public:
    explicit WindowAppearanceController(QObject* parent = nullptr);

    Q_INVOKABLE void attachWindow(QObject* window);
    Q_INVOKABLE void applyTheme(const QString& effectiveTheme);

private:
    void apply();

    QPointer<QQuickWindow> m_window;
    QString m_effectiveTheme { QStringLiteral("dark") };
};
