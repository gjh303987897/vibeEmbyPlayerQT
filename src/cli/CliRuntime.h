#pragma once
#include "cli/CliCommand.h"
#include "cli/CliConsole.h"
#include "network/NetworkError.h"
#include <QEventLoop>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QTimer>
#include <QPointer>
#include <QThread>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>

namespace Cli {
inline Result failure(ExitCode code, const QString& message, QJsonValue data = QJsonValue(QJsonValue::Null))
{ return {code, data, message}; }
inline Result failure(const Error& error) { return failure(error.code, error.message); }
inline Result success(QJsonValue data) { return {ExitCode::Success, data, {}}; }
inline Result networkFailure(const NetworkError& error)
{
    auto code = error.httpStatus == 401 || error.httpStatus == 403 ? ExitCode::Authentication :
        error.httpStatus == 404 ? ExitCode::NotFound :
        (error.httpStatus == 405 || error.httpStatus == 409 || error.httpStatus == 412) ? ExitCode::Conflict : ExitCode::Failed;
    if (error.kind == NetworkErrorKind::Canceled || CliConsole::pollCancellation()) code = ExitCode::Canceled;
    // Reply diagnostics can include credentials or signed URLs. Output the status and safe kind.
    return failure(code, error.httpStatus ? QStringLiteral("HTTP %1").arg(error.httpStatus) :
                   code == ExitCode::Canceled ? QStringLiteral("Canceled") : QStringLiteral("Network request failed"));
}

template<typename ResultType, typename Start, typename Cancel>
ResultType await(Start start, Cancel cancel)
{
    QEventLoop loop;
    QTimer timer;
    bool cancelSent = false;
    auto result = std::make_shared<std::optional<ResultType>>();
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        if (!cancelSent && CliConsole::pollCancellation()) { cancelSent = true; cancel(); }
    });
    timer.start(50);
    start([result, loop = QPointer<QEventLoop>(&loop)](ResultType value) {
        if (!loop || *result) return;
        *result = std::move(value);
        loop->quit();
    });
    if (!*result) loop.exec();
    if (!*result) {
        CliConsole::canceled.store(true);
        if (!cancelSent) cancel();
        if (!*result) {
            if constexpr (requires { typename ResultType::error_type; }) {
                using ErrorType = typename ResultType::error_type;
                if constexpr (std::is_same_v<ErrorType, QString>)
                    return std::unexpected(QStringLiteral("Canceled"));
                else if constexpr (std::is_same_v<ErrorType, NetworkError>)
                    return std::unexpected(NetworkError {NetworkErrorKind::Canceled, QStringLiteral("Canceled")});
                else return std::unexpected(Error {ExitCode::Canceled, QStringLiteral("Canceled")});
            } else if constexpr (std::is_same_v<ResultType, Result>) {
                return failure(ExitCode::Canceled, QStringLiteral("Canceled"));
            } else if constexpr (std::is_same_v<ResultType, std::pair<bool, QString>>) {
                return {false, QStringLiteral("Canceled")};
            } else if constexpr (std::is_same_v<ResultType, bool>) {
                return false;
            } else {
                ResultType interrupted {};
                if constexpr (requires { interrupted.canceled = true; }) interrupted.canceled = true;
                return interrupted;
            }
        }
    }
    return std::move(**result);
}

template<typename T>
T awaitFuture(QFuture<T> future)
{
    QEventLoop loop;
    QFutureWatcher<T> watcher;
    QTimer timer;
    QObject::connect(&watcher, &QFutureWatcher<T>::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [] {
        // Workers check the shared cancellation flag at block boundaries.
        // Keep processing events while their cleanup completes.
        if (CliConsole::pollCancellation()) CliConsole::transferPaused.store(false);
    });
    timer.start(50);
    watcher.setFuture(future);
    if (!future.isFinished()) loop.exec();
    if (!future.isFinished()) {
        // Application quit can stop nested exec immediately. The command's
        // inputs must still outlive a worker capturing them by reference.
        CliConsole::canceled.store(true);
        CliConsole::transferPaused.store(false);
        while (!future.isFinished()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
    }
    return future.takeResult();
}
}
