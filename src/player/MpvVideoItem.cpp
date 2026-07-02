#include "player/MpvVideoItem.h"

#include "utils/AppLogger.h"

#include <QQuickWindow>
#include <QTimer>
#include <QtMath>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef Q_OS_WIN
namespace {
constexpr auto mpvHostWindowClassName = L"VibePlayerMpvHostWindow";

LRESULT CALLBACK mpvHostWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void registerMpvHostWindowClass()
{
    static const bool registered = [] {
        WNDCLASSEXW windowClass {};
        windowClass.cbSize = sizeof(WNDCLASSEXW);
        windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        windowClass.lpfnWndProc = mpvHostWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.lpszClassName = mpvHostWindowClassName;
        return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    Q_UNUSED(registered);
}

HWND hwndFromId(quintptr id)
{
    return reinterpret_cast<HWND>(id);
}

void enableChildWindowClipping(HWND hwnd)
{
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const auto clippedStyle = style | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    if (clippedStyle != style) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, clippedStyle);
        SetWindowPos(hwnd,
                     nullptr,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

struct NativeVideoGeometry {
    QPoint topLeft;
    QSize size;
    QPoint logicalTopLeft;
    QSize logicalSize;
    qreal devicePixelRatio { 1.0 };
};

NativeVideoGeometry nativeVideoGeometry(HWND parentHwnd, QQuickWindow* window, QQuickItem* item)
{
    const auto dpr = qMax<qreal>(1.0, window ? window->devicePixelRatio() : 1.0);
    const auto sceneTopLeft = item->mapToScene(QPointF(0, 0));
    const auto logicalTopLeft = QPoint(qRound(sceneTopLeft.x()), qRound(sceneTopLeft.y()));
    const auto logicalSize = QSize(qMax(1, qRound(item->width())), qMax(1, qRound(item->height())));

    POINT nativeTopLeft {
        qRound(sceneTopLeft.x() * dpr),
        qRound(sceneTopLeft.y() * dpr),
    };
    if (!ClientToScreen(parentHwnd, &nativeTopLeft) && window) {
        const auto fallbackTopLeft = window->mapToGlobal(logicalTopLeft);
        nativeTopLeft.x = fallbackTopLeft.x();
        nativeTopLeft.y = fallbackTopLeft.y();
    }

    return {
        QPoint(nativeTopLeft.x, nativeTopLeft.y),
        QSize(qMax(1, qRound(item->width() * dpr)), qMax(1, qRound(item->height() * dpr))),
        logicalTopLeft,
        logicalSize,
        dpr,
    };
}

}
#endif

MpvVideoItem::MpvVideoItem(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, false);
    connect(&m_controller, &PlayerController::errorOccurred, this, &MpvVideoItem::errorOccurred);
    connect(&m_controller, &PlayerController::playbackStateChanged, this, &MpvVideoItem::playbackStateChanged);
    connect(&m_controller, &PlayerController::volumeChanged, this, &MpvVideoItem::volumeChanged);
    connect(&m_controller, &PlayerController::speedChanged, this, &MpvVideoItem::speedChanged);
    connect(&m_controller, &PlayerController::videoInfoChanged, this, &MpvVideoItem::videoInfoChanged);
    connect(&m_controller, &PlayerController::cacheStatsChanged, this, &MpvVideoItem::cacheStatsChanged);
    connect(&m_controller, &PlayerController::tracksChanged, this, &MpvVideoItem::tracksChanged);
    connect(&m_controller, &PlayerController::videoOutputChanged, this, &MpvVideoItem::refreshNativeWindow);
    connect(&m_controller, &PlayerController::playbackRestarted, this, &MpvVideoItem::playbackRestarted);
    connect(&m_controller, &PlayerController::playbackNetworkBytes, this, &MpvVideoItem::playbackNetworkBytes);
}

MpvVideoItem::~MpvVideoItem()
{
    stop();
    destroyNativeWindow();
}

QUrl MpvVideoItem::source() const
{
    return m_source;
}

void MpvVideoItem::setSource(const QUrl& value)
{
    if (m_source == value) {
        return;
    }
    m_source = value;
    emit sourceChanged();
    if (m_source.isEmpty()) {
        stop();
        return;
    }
    m_pendingPlay = true;
    play();
}

double MpvVideoItem::startPosition() const
{
    return m_startPosition;
}

void MpvVideoItem::setStartPosition(double value)
{
    const auto normalized = qMax(0.0, value);
    if (qFuzzyCompare(m_startPosition + 1.0, normalized + 1.0)) {
        return;
    }
    m_startPosition = normalized;
    emit startPositionChanged();
}

QString MpvVideoItem::httpUsername() const
{
    return m_httpUsername;
}

void MpvVideoItem::setHttpUsername(const QString& value)
{
    if (m_httpUsername == value) {
        return;
    }
    m_httpUsername = value;
    emit httpAuthChanged();
}

QString MpvVideoItem::httpPassword() const
{
    return m_httpPassword;
}

void MpvVideoItem::setHttpPassword(const QString& value)
{
    if (m_httpPassword == value) {
        return;
    }
    m_httpPassword = value;
    emit httpAuthChanged();
}

bool MpvVideoItem::allowInsecureTls() const
{
    return m_allowInsecureTls;
}

void MpvVideoItem::setAllowInsecureTls(bool value)
{
    if (m_allowInsecureTls == value) {
        return;
    }
    m_allowInsecureTls = value;
    emit httpAuthChanged();
}

bool MpvVideoItem::paused() const
{
    return m_controller.paused();
}

bool MpvVideoItem::loading() const
{
    return m_controller.loading();
}

bool MpvVideoItem::buffering() const
{
    return m_controller.buffering();
}

int MpvVideoItem::bufferingProgress() const
{
    return m_controller.bufferingProgress();
}

bool MpvVideoItem::seeking() const
{
    return m_controller.seeking();
}

double MpvVideoItem::position() const
{
    return m_controller.position();
}

double MpvVideoItem::duration() const
{
    return m_controller.duration();
}

int MpvVideoItem::volume() const
{
    return m_controller.volume();
}

double MpvVideoItem::speed() const
{
    return m_controller.speed();
}

QString MpvVideoItem::videoResolution() const
{
    return m_controller.videoResolution();
}

QString MpvVideoItem::videoCodec() const
{
    return m_controller.videoCodec();
}

QString MpvVideoItem::videoFrameRate() const
{
    return m_controller.videoFrameRate();
}

QString MpvVideoItem::videoBitrate() const
{
    return m_controller.videoBitrate();
}

double MpvVideoItem::cacheDurationSeconds() const
{
    return m_controller.cacheDurationSeconds();
}

TrackListModel* MpvVideoItem::subtitleTracks()
{
    return m_controller.subtitleTracks();
}

TrackListModel* MpvVideoItem::audioTracks()
{
    return m_controller.audioTracks();
}

void MpvVideoItem::play()
{
    if (m_source.isEmpty()) {
        return;
    }
    if (!window() || !isVisible() || width() <= 0 || height() <= 0) {
        m_pendingPlay = true;
        AppLogger::info(QStringLiteral("player"), QStringLiteral("Deferring playback until native video item is visible"));
        return;
    }
    ensureNativeWindow();
    if (!m_initialized) {
        m_pendingPlay = true;
        AppLogger::warning(QStringLiteral("player"), QStringLiteral("Deferring playback because native mpv window is not initialized"));
        return;
    }
    m_controller.playUrl(sourceString(), m_startPosition, m_httpUsername, m_httpPassword, m_allowInsecureTls);
    m_startPosition = 0.0;
    emit startPositionChanged();
    m_pendingPlay = false;
    scheduleNativeWindowRefresh();
}

void MpvVideoItem::pause()
{
    m_controller.pause();
}

void MpvVideoItem::resume()
{
    m_controller.resume();
}

void MpvVideoItem::togglePause()
{
    m_controller.togglePause();
}

void MpvVideoItem::stop()
{
    m_pendingPlay = false;
    hideNativeWindow();
    m_controller.stop();
    destroyNativeWindow();
}

void MpvVideoItem::seekRelative(double seconds)
{
    m_controller.seekRelative(seconds);
}

void MpvVideoItem::seekAbsolute(double seconds)
{
    m_controller.seekAbsolute(seconds);
}

void MpvVideoItem::setVolume(int volume)
{
    m_controller.setVolume(volume);
}

void MpvVideoItem::setSpeed(double speed)
{
    m_controller.setSpeed(speed);
}

void MpvVideoItem::selectSubtitleTrack(int row)
{
    m_controller.selectSubtitleTrack(row);
}

void MpvVideoItem::selectAudioTrack(int row)
{
    m_controller.selectAudioTrack(row);
}

void MpvVideoItem::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange) {
        if (value.window && !m_source.isEmpty()) {
            ensureNativeWindow();
            if (m_pendingPlay) {
                play();
            }
        } else {
            stop();
            destroyNativeWindow();
        }
    } else if (change == ItemVisibleHasChanged) {
        if (isVisible()) {
            syncWindowGeometry();
            if (!m_source.isEmpty() && m_pendingPlay) {
                play();
            }
        } else {
            stop();
        }
    }
}

void MpvVideoItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    syncWindowGeometry();
    if (!m_source.isEmpty() && m_pendingPlay) {
        play();
    }
}

void MpvVideoItem::ensureNativeWindow()
{
    auto* sceneWindow = window();
    if (!sceneWindow || !isVisible() || width() <= 0 || height() <= 0) {
        return;
    }

#ifdef Q_OS_WIN
    const auto parentWindowId = static_cast<quintptr>(sceneWindow->winId());
    auto parentHwnd = hwndFromId(parentWindowId);
    if (!parentHwnd) {
        return;
    }
    enableChildWindowClipping(parentHwnd);

    if (!m_nativeWindowId) {
        registerMpvHostWindowClass();
        const auto geometry = nativeVideoGeometry(parentHwnd, sceneWindow, this);
        auto hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOPARENTNOTIFY,
                                    mpvHostWindowClassName,
                                    L"",
                                    WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                    geometry.topLeft.x(),
                                    geometry.topLeft.y(),
                                    geometry.size.width(),
                                    geometry.size.height(),
                                    nullptr,
                                    nullptr,
                                    GetModuleHandleW(nullptr),
                                    nullptr);
        if (!hwnd) {
            AppLogger::warning(QStringLiteral("player"), QStringLiteral("Unable to create native mpv child HWND"));
            return;
        }
        m_nativeWindowId = reinterpret_cast<quintptr>(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(parentHwnd));
        AppLogger::info(QStringLiteral("player"),
                        QStringLiteral("Created native mpv popup HWND id=%1 owner=%2 geometry=%3,%4 %5x%6 logical=%7,%8 %9x%10 dpr=%11")
                            .arg(m_nativeWindowId)
                            .arg(parentWindowId)
                            .arg(geometry.topLeft.x())
                            .arg(geometry.topLeft.y())
                            .arg(geometry.size.width())
                            .arg(geometry.size.height())
                            .arg(geometry.logicalTopLeft.x())
                            .arg(geometry.logicalTopLeft.y())
                            .arg(geometry.logicalSize.width())
                            .arg(geometry.logicalSize.height())
                            .arg(geometry.devicePixelRatio, 0, 'f', 2));
    }

    auto hwnd = hwndFromId(m_nativeWindowId);
#else
    if (!m_nativeWindow) {
        m_nativeWindow = new QWindow(sceneWindow);
        m_nativeWindow->setFlags(Qt::SubWindow | Qt::FramelessWindowHint | Qt::WindowTransparentForInput);
        m_nativeWindow->create();
        m_nativeWindowId = static_cast<quintptr>(m_nativeWindow->winId());
        AppLogger::info(QStringLiteral("player"), QStringLiteral("Created native mpv child window"));
    }

    if (m_nativeWindow->parent() != sceneWindow) {
        m_nativeWindow->setParent(sceneWindow);
    }
#endif

    connect(sceneWindow, &QWindow::xChanged, this, &MpvVideoItem::syncWindowGeometry, Qt::UniqueConnection);
    connect(sceneWindow, &QWindow::yChanged, this, &MpvVideoItem::syncWindowGeometry, Qt::UniqueConnection);
    connect(sceneWindow, &QWindow::widthChanged, this, &MpvVideoItem::syncWindowGeometry, Qt::UniqueConnection);
    connect(sceneWindow, &QWindow::heightChanged, this, &MpvVideoItem::syncWindowGeometry, Qt::UniqueConnection);
    connect(sceneWindow, &QWindow::visibilityChanged, this, [this](QWindow::Visibility) {
        syncWindowGeometry();
    });

    if (!m_initialized) {
        AppLogger::info(QStringLiteral("player"), QStringLiteral("Initializing libmpv for native window id=%1").arg(m_nativeWindowId));
        m_initialized = m_controller.initialize(static_cast<qintptr>(m_nativeWindowId));
    }
    QTimer::singleShot(0, this, &MpvVideoItem::syncWindowGeometry);
}

void MpvVideoItem::destroyNativeWindow()
{
#ifdef Q_OS_WIN
    if (m_nativeWindowId) {
        DestroyWindow(hwndFromId(m_nativeWindowId));
        m_nativeWindowId = 0;
    }
#else
    if (m_nativeWindow) {
        m_nativeWindow->hide();
        delete m_nativeWindow;
        m_nativeWindow = nullptr;
    }
    m_nativeWindowId = 0;
#endif
    m_lastNativeGeometry = {};
    m_lastNativeDevicePixelRatio = 0.0;
    m_controller.shutdown();
    m_initialized = false;
}

void MpvVideoItem::hideNativeWindow()
{
#ifdef Q_OS_WIN
    if (m_nativeWindowId) {
        ShowWindow(hwndFromId(m_nativeWindowId), SW_HIDE);
    }
#else
    if (m_nativeWindow) {
        m_nativeWindow->hide();
    }
#endif
}

void MpvVideoItem::syncWindowGeometry()
{
    if (!m_nativeWindowId || !window()) {
        return;
    }

#ifdef Q_OS_WIN
    auto* sceneWindow = window();
    auto parentHwnd = hwndFromId(static_cast<quintptr>(sceneWindow->winId()));
    if (!parentHwnd) {
        return;
    }
    const auto geometry = nativeVideoGeometry(parentHwnd, sceneWindow, this);
    auto hwnd = hwndFromId(m_nativeWindowId);
    const auto flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_SHOWWINDOW;
    SetWindowPos(hwnd, nullptr, geometry.topLeft.x(), geometry.topLeft.y(), geometry.size.width(), geometry.size.height(), flags);
    RECT actualRect {};
    if (GetWindowRect(hwnd, &actualRect)) {
        const auto actualGeometry = QRect(actualRect.left,
                                          actualRect.top,
                                          actualRect.right - actualRect.left,
                                          actualRect.bottom - actualRect.top);
        if (actualGeometry != m_lastNativeGeometry
            || !qFuzzyCompare(m_lastNativeDevicePixelRatio + 1.0, geometry.devicePixelRatio + 1.0)) {
            m_lastNativeGeometry = actualGeometry;
            m_lastNativeDevicePixelRatio = geometry.devicePixelRatio;
            AppLogger::info(QStringLiteral("player"),
                            QStringLiteral("Synced native mpv popup HWND geometry=%1,%2 %3x%4 logical=%5,%6 %7x%8 dpr=%9")
                                .arg(actualRect.left)
                                .arg(actualRect.top)
                                .arg(actualRect.right - actualRect.left)
                                .arg(actualRect.bottom - actualRect.top)
                                .arg(geometry.logicalTopLeft.x())
                                .arg(geometry.logicalTopLeft.y())
                                .arg(geometry.logicalSize.width())
                                .arg(geometry.logicalSize.height())
                                .arg(geometry.devicePixelRatio, 0, 'f', 2));
        }
    }
    if (isVisible() && width() > 0 && height() > 0) {
        ShowWindow(hwnd, SW_SHOWNA);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else {
        ShowWindow(hwnd, SW_HIDE);
    }
#else
    const auto size = QSize(qMax(1, qRound(width())), qMax(1, qRound(height())));
    const auto topLeft = mapToScene(QPointF(0, 0)).toPoint();
    m_nativeWindow->setGeometry(QRect(topLeft, size));
    m_nativeWindow->setVisible(isVisible() && width() > 0 && height() > 0);
    if (m_nativeWindow->isVisible()) {
        m_nativeWindow->raise();
    }
#endif
}

void MpvVideoItem::refreshNativeWindow()
{
    if (!m_nativeWindowId || !isVisible()) {
        return;
    }

    syncWindowGeometry();
#ifdef Q_OS_WIN
    auto hwnd = hwndFromId(m_nativeWindowId);
    SetWindowPos(hwnd,
                 nullptr,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd, nullptr, FALSE);
#else
    m_nativeWindow->hide();
    m_nativeWindow->show();
    m_nativeWindow->raise();
#endif
    AppLogger::info(QStringLiteral("player"), QStringLiteral("Refreshed native mpv child window"));
    emit nativeWindowUpdated();
    QTimer::singleShot(0, this, [this] {
        syncWindowGeometry();
        if (m_nativeWindowId && isVisible()) {
#ifdef Q_OS_WIN
            auto hwnd = hwndFromId(m_nativeWindowId);
            SetWindowPos(hwnd,
                         nullptr,
                         0,
                         0,
                         0,
                         0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(hwnd, nullptr, FALSE);
#else
            m_nativeWindow->show();
            m_nativeWindow->raise();
#endif
            emit nativeWindowUpdated();
        }
    });
}

void MpvVideoItem::scheduleNativeWindowRefresh()
{
    refreshNativeWindow();
    QTimer::singleShot(120, this, &MpvVideoItem::refreshNativeWindow);
    QTimer::singleShot(500, this, &MpvVideoItem::refreshNativeWindow);
}

QString MpvVideoItem::sourceString() const
{
    if (m_source.isLocalFile()) {
        return m_source.toLocalFile();
    }
    return m_source.toString(QUrl::FullyEncoded);
}
