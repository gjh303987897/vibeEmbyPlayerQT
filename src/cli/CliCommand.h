#pragma once
#include <QJsonValue>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <expected>

namespace Cli {
enum class ExitCode { Success = 0, Failed = 1, Usage = 2, Authentication = 3,
                      NotFound = 4, Conflict = 5, Partial = 6, Canceled = 130 };
struct Error { ExitCode code; QString message; };
struct Invocation {
    QString command;
    QStringList arguments;
    QMap<QString, QString> options;
    QSet<QString> flags;
    QString option(const QString& name, const QString& fallback = {}) const { return options.value(name, fallback); }
    bool has(const QString& name) const { return flags.contains(name) || options.contains(name); }
};
struct Result {
    ExitCode code { ExitCode::Success };
    QJsonValue data;
    QString message;
};
std::expected<Invocation, Error> parse(const QStringList& arguments);
std::expected<QStringList, Error> splitLine(const QString& line);
QString help(const QString& command = {});
QStringList commandNames();
void writeResult(const Invocation& invocation, const Result& result);
}
