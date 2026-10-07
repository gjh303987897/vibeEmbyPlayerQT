#pragma once
#include "cli/CliCommand.h"
#include <QJsonObject>
#include <functional>
class QLocalServer;

namespace Cli {
QString socketName(const QString& kind, const QString& profile);
Result rpc(const QString& kind, const QString& profile, const QJsonObject& request, bool start = false);
QJsonObject resultObject(const Result& result);
void serveRequests(QLocalServer& server, std::function<Result(const QJsonObject&)> handler);
}
