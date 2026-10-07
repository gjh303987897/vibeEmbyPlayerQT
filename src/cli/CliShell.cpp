#include "cli/CliShell.h"
#include "cli/CliRuntime.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
QString scalar(const QJsonValue& value)
{
    if (value.isString()) return value.toString();
    if (value.isDouble()) return QString::number(value.toDouble(),'g',15);
    if (value.isBool()) return value.toBool() ? "true" : "false";
    return QString::fromUtf8(value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact) :
                             QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
}
std::expected<QStringList,Cli::Error> pipelines(const QString& line)
{
    QStringList parts;
    QChar quote; int start = 0, nesting = 0;
    for (int i = 0; i < line.size(); ++i) {
        const auto c = line[i];
        if (!quote.isNull()) {
            if (c == quote) { if (i + 1 < line.size() && line[i + 1] == quote) ++i; else quote = {}; }
        } else if (c == '"' || c == '\'') quote = c;
        else if (c == '(') ++nesting;
        else if (c == ')') --nesting;
        else if (c == '|' && nesting == 0) { parts += line.mid(start,i - start); start = i + 1; }
    }
    if (!quote.isNull() || nesting != 0) return std::unexpected(Cli::Error {Cli::ExitCode::Usage,"Unclosed quote or substitution"});
    parts += line.mid(start);
    for (const auto& part : parts) if (part.trimmed().isEmpty()) return std::unexpected(Cli::Error {Cli::ExitCode::Usage,"Empty pipeline stage"});
    return parts;
}
}

std::expected<QString,Cli::Error> Cli::Shell::expandVariables(const QString& line, int depth)
{
    if (depth > 8) return std::unexpected(Error {ExitCode::Usage,"Command substitution nesting limit exceeded"});
    QString output;
    QChar quote;
    for (int i = 0; i < line.size(); ++i) {
        const auto c = line[i];
        if ((c == '"' || c == '\'') && (quote.isNull() || quote == c)) {
            if (quote == c && i + 1 < line.size() && line[i + 1] == c) { output += c; output += c; ++i; continue; }
            quote = quote.isNull() ? c : QChar(); output += c; continue;
        }
        if (c != '$' || quote == '\'') { output += c; continue; }
        QString value;
        if (i + 1 < line.size() && line[i + 1] == '(') {
            const int start = i + 2;
            int level = 1; QChar inner;
            i += 2;
            for (; i < line.size(); ++i) {
                const auto ch = line[i];
                if (!inner.isNull()) { if (ch == inner) inner = {}; }
                else if (ch == '"' || ch == '\'') inner = ch;
                else if (ch == '(') ++level;
                else if (ch == ')' && --level == 0) break;
            }
            if (level) return std::unexpected(Error {ExitCode::Usage,"Unclosed command substitution"});
            const auto result = evaluate(line.mid(start,i - start),depth + 1).second;
            if (result.code != ExitCode::Success) return std::unexpected(Error {result.code,result.message});
            value = scalar(result.data);
        } else {
            int start = i + 1, end = start;
            if (start < line.size() && line[start] == '{') {
                start++; end = line.indexOf('}',start);
                if (end < 0) return std::unexpected(Error {ExitCode::Usage,"Unclosed environment variable"});
                i = end;
            } else {
                while (end < line.size() && (line[end].isLetterOrNumber() || line[end] == '_')) ++end;
                if (end == start) { output += c; continue; }
                i = end - 1;
            }
            const auto name = line.mid(start,end - start);
            if (!qEnvironmentVariableIsSet(name.toUtf8().constData()))
                return std::unexpected(Error {ExitCode::Usage,"Environment variable is not set: " + name});
            value = qEnvironmentVariable(name.toUtf8().constData());
        }
        // Expansions are literal argument text, preventing introduced operators or options.
        value.replace("\"","\"\"");
        output += quote == '"' ? value : '"' + value + '"';
    }
    return output;
}

QStringList Cli::Shell::complete(const QString& prefix)
{
    QStringList result;
    for (const auto& name : commandNames()) if (name.startsWith(prefix,Qt::CaseInsensitive)) result += name;
    const auto split = prefix.lastIndexOf(' ');
    if (split >= 0) {
        const auto base = prefix.left(split + 1);
        const auto token = prefix.mid(split + 1);
        Invocation list; list.command = "service list";
        const auto cards = m_engine.execute(list);
        for (const auto& card : cards.data.toArray())
            if (card.toObject().value("name").toString().startsWith(token,Qt::CaseInsensitive))
                result += base + '"' + card.toObject().value("name").toString().replace("\"","\"\"") + '"';
        const QFileInfo info(token.isEmpty() ? "." : token);
        for (const auto& file : QDir(info.path()).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,QDir::Name))
            if (file.fileName().startsWith(token.isEmpty() ? QString() : info.fileName(),Qt::CaseInsensitive))
                result += base + '"' + QDir(info.path()).filePath(file.fileName()) + (file.isDir() ? "/" : "") + '"';
    }
    result.removeDuplicates(); return result;
}

std::pair<Cli::Invocation,Cli::Result> Cli::Shell::evaluate(const QString& input, int depth)
{
    Invocation invalid = m_defaults; invalid.command = "shell";
    auto stages = pipelines(input);
    if (!stages) return {invalid,failure(stages.error())};
    auto expanded = expandVariables(stages->front(),depth);
    if (!expanded) return {invalid,failure(expanded.error())};
    auto line = expanded->trimmed();
    bool background = false;
    if (line.endsWith('&')) { background = true; line.chop(1); }
    auto words = splitLine(line);
    if (words) words = m_engine.expand(*words);
    auto command = words ? parse(*words) : std::expected<Invocation,Error>(std::unexpected(words.error()));
    if (!command) return {invalid,failure(command.error())};
    if (background) command->flags.insert("background");
    for (const auto& option : {"password-env","pin-env","s3-secret-env","s3-session-token-env"})
        if (!command->has(option) && m_defaults.has(option)) command->options[option] = m_defaults.option(option);
    if (m_defaults.has("json")) command->flags.insert("json");
    if (command->command == "exit") return {*command,success(QJsonValue())};
    if (command->command == "shell" || command->command == "gui" || command->command == "daemon run" || command->has("data-dir"))
        return {*command,failure(ExitCode::Usage,"Nested shell, GUI launch, daemon host and profile changes are unavailable in shell")};
    Result result;
    if (command->command == "history") {
        if (command->has("clear")) { m_history.clear(); result = success(QStringLiteral("History cleared")); }
        else result = success(QJsonArray::fromStringList(m_history));
    } else if (command->command == "complete") result = success(QJsonArray::fromStringList(complete(command->arguments.value(0))));
    else result = m_engine.execute(*command);
    for (const auto& raw : stages->mid(1)) {
        if (result.code != ExitCode::Success) break;
        auto expression = expandVariables(raw,depth);
        auto filter = expression ? splitLine(*expression) : std::expected<QStringList,Error>(std::unexpected(expression.error()));
        if (!filter || filter->isEmpty()) return {*command,filter ? failure(ExitCode::Usage,"Empty filter") : failure(filter.error())};
        const auto operation = filter->front();
        if (operation == "count" && filter->size() == 1) {
            result = success(result.data.isArray() ? result.data.toArray().size() : result.data.toObject().size());
        } else if (operation == "get" && filter->size() == 2 && result.data.isObject()) {
            const auto object = result.data.toObject();
            if (!object.contains((*filter)[1])) return {*command,failure(ExitCode::NotFound,"Result field not found")};
            result = success(object.value((*filter)[1]));
        } else if ((operation == "select" && filter->size() >= 2) || (operation == "where" && filter->size() == 3)) {
            QJsonArray rows = result.data.isArray() ? result.data.toArray() : QJsonArray {result.data};
            QJsonArray output;
            for (const auto& row : rows) {
                if (!row.isObject()) return {*command,failure(ExitCode::Usage,"This filter requires object rows")};
                const auto object = row.toObject();
                if (operation == "where") {
                    if (scalar(object.value((*filter)[1])) == (*filter)[2]) output += object;
                } else {
                    QJsonObject selected;
                    for (const auto& key : filter->mid(1)) {
                        if (!object.contains(key)) return {*command,failure(ExitCode::NotFound,"Result field not found")};
                        selected[key] = object.value(key);
                    }
                    output += selected;
                }
            }
            result = success(output);
        } else return {*command,failure(ExitCode::Usage,"Pipeline filters: select FIELD..., where FIELD VALUE, get FIELD, count")};
    }
    return {*command,result};
}
std::pair<Cli::Invocation,Cli::Result> Cli::Shell::execute(const QString& line)
{
    if (!line.trimmed().startsWith("history")) {
        m_history += line;
        if (m_history.size() > 1000) m_history.removeFirst();
    }
    return evaluate(line,0);
}
