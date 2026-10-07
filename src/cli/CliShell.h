#pragma once
#include "cli/CliEngine.h"
#include <functional>
#include <QElapsedTimer>
namespace Cli {
class Shell final {
public:
    Shell(Engine& engine, Invocation defaults) : m_engine(engine), m_defaults(std::move(defaults)) {}
    std::pair<Invocation,Result> execute(const QString& line);
    QStringList complete(const QString& prefix);
    const QStringList& history() const { return m_history; }
private:
    std::expected<QString,Error> expandVariables(const QString& line, int depth);
    std::pair<Invocation,Result> evaluate(const QString& line, int depth);
    Engine& m_engine;
    Invocation m_defaults;
    QStringList m_history;
    QStringList m_serviceCompletionNames;
    QElapsedTimer m_serviceCompletionAge;
    bool m_serviceCompletionPrivate {false};
};
}
