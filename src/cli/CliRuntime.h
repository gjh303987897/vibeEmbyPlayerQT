#pragma once
#include "cli/CliCommand.h"
#include "cli/CliConsole.h"
#include "network/NetworkError.h"
#include <QEventLoop>
#include <QFutureWatcher>
#include <QTimer>
#include <functional>
#include <optional>

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
    if (CliConsole::pollCancellation()) code = ExitCode::Canceled;
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
    std::optional<ResultType> result;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        if (!cancelSent && CliConsole::pollCancellation()) { cancelSent = true; cancel(); }
    });
    timer.start(50);
    start([&](ResultType value) { result = std::move(value); loop.quit(); });
    if (!result) loop.exec();
    return std::move(*result);
}

template<typename T>
T awaitFuture(QFuture<T> future)
{
    return await<T>([future = std::move(future)](auto complete) mutable {
        auto* watcher = new QFutureWatcher<T>();
        QObject::connect(watcher, &QFutureWatcher<T>::finished, watcher, [watcher, complete]() {
            auto value = watcher->future().takeResult();
            watcher->deleteLater();
            complete(std::move(value));
        });
        watcher->setFuture(future);
    }, [] {});
}
}
