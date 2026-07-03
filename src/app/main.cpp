#include "app/TrayController.h"
#include "app/WindowAppearanceController.h"
#include "utils/AppLogger.h"
#include "viewmodels/AppViewModel.h"

#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QTimer>
#include <QUrl>
#include <QWindow>

namespace {
QIcon iconForTheme(const QString& effectiveTheme)
{
    const auto path = effectiveTheme == QStringLiteral("light")
        ? QStringLiteral(":/app/icons/icon_black.png")
        : QStringLiteral(":/app/icons/icon_white.png");
    return QIcon(path);
}

void applyApplicationIcon(const QString& effectiveTheme, TrayController& trayController)
{
    const auto icon = iconForTheme(effectiveTheme);
    if (icon.isNull()) {
        AppLogger::warning(QStringLiteral("app"), QStringLiteral("Application icon resource is missing"));
        return;
    }

    QApplication::setWindowIcon(icon);
    trayController.setIcon(icon);
    for (auto* window : QGuiApplication::topLevelWindows()) {
        if (window) {
            window->setIcon(icon);
        }
    }
}
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("vibePlayerQT"));
    QApplication::setOrganizationName(QStringLiteral("vibePlayerQT"));
    QApplication::setQuitOnLastWindowClosed(true);

    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    AppViewModel appViewModel;
    TrayController trayController;
    WindowAppearanceController windowAppearanceController;
    applyApplicationIcon(appViewModel.effectiveTheme(), trayController);
    trayController.setMinimizeToTray(appViewModel.minimizeToTray());

    QObject::connect(&appViewModel, &AppViewModel::minimizeToTrayChanged, &trayController, [&]() {
        trayController.setMinimizeToTray(appViewModel.minimizeToTray());
    });
    QObject::connect(&appViewModel, &AppViewModel::effectiveThemeChanged, &trayController, [&]() {
        applyApplicationIcon(appViewModel.effectiveTheme(), trayController);
    });

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appViewModel"), &appViewModel);
    engine.rootContext()->setContextProperty(QStringLiteral("trayController"), &trayController);
    engine.rootContext()->setContextProperty(QStringLiteral("windowAppearanceController"), &windowAppearanceController);

    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app, [](const QList<QQmlError>& warnings) {
        for (const auto& warning : warnings) {
            AppLogger::warning(QStringLiteral("qml"), warning.toString());
        }
    });
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, []() {
        AppLogger::warning(QStringLiteral("qml"), QStringLiteral("QML root object creation failed"));
        QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("VibePlayer"), QStringLiteral("Main"));
    const auto testVideo = qEnvironmentVariable("VIBEPLAYER_TEST_VIDEO");
    if (!testVideo.isEmpty()) {
        QTimer::singleShot(300, &appViewModel, [&appViewModel, testVideo]() {
            appViewModel.openLocalPlaybackForVerification(QUrl::fromLocalFile(testVideo));
        });
    }
    return app.exec();
}
