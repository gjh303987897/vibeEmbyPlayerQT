#include "cli/CliOutput.h"
#include "utils/ErrorPresentation.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <numeric>

namespace {
QString safeText(QString text)
{
    for (auto& ch : text) {
        const auto category = ch.category();
        if (category == QChar::Other_Control || category == QChar::Separator_Line ||
            category == QChar::Separator_Paragraph ||
            (category == QChar::Other_Format && ch.unicode() != 0x200c && ch.unicode() != 0x200d))
            ch = QLatin1Char(' ');
    }
    return text;
}

int clusterWidth(QStringView cluster)
{
    int width = 0;
    for (const auto cp : cluster.toUcs4()) {
        const auto category = QChar::category(cp);
        if (category == QChar::Mark_NonSpacing || category == QChar::Mark_Enclosing ||
            category == QChar::Other_Format) continue;
        // Terminal cell widths for CJK/full-width characters and common emoji.
        const bool wide = cp >= 0x1100 && (cp <= 0x115f || cp == 0x2329 || cp == 0x232a ||
            (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
            (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
            (cp >= 0xfe10 && cp <= 0xfe19) || (cp >= 0xfe30 && cp <= 0xfe6f) ||
            (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) ||
            (cp >= 0x1f1e6 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd));
        width = std::max(width, wide ? 2 : 1);
    }
    if (cluster.contains(QChar(0xfe0f))) width = std::max(width, 2);
    return width;
}

int textWidth(const QString& text)
{
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    int width = 0;
    qsizetype start = 0, end;
    while ((end = finder.toNextBoundary()) != -1) {
        width += clusterWidth(QStringView(text).mid(start, end - start));
        start = end;
    }
    return width;
}

QStringList wrap(const QString& text, int columns)
{
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    QStringList lines;
    QString line;
    int width = 0;
    qsizetype start = 0, end;
    while ((end = finder.toNextBoundary()) != -1) {
        const auto cluster = QStringView(text).mid(start, end - start);
        const auto cells = clusterWidth(cluster);
        if (width + cells > columns && !line.isEmpty()) {
            lines += line; line.clear(); width = 0;
        }
        line += cluster; width += cells; start = end;
    }
    lines += line;
    return lines;
}

QString label(const QString& key)
{
    static const QMap<QString, QString> labels {
        {"id","ID"}, {"name","Name"}, {"type","Type"}, {"address","Address"},
        {"username","Username"}, {"hasSavedSession","Saved session"},
        {"hasSavedCredentials","Saved credentials"}, {"private","Private"},
        {"autoLogin","Auto login"}, {"trustSelfSigned","Trust self-signed"},
        {"directory","Type"}, {"size","Size"}, {"digest","Digest"}, {"file","File"},
        {"modifiedAt","Modified"}, {"originalName","Original name"}, {"legacyName","Legacy name"},
        {"backedUp","Backed up"}, {"retainedDirectory","Retained directory"},
        {"sessionSaved","Session saved"}, {"credentialsSaved","Credentials saved"},
    };
    if (labels.contains(key)) return labels.value(key);
    if (key.contains('/')) return safeText(key); // Configuration keys remain directly usable.
    auto text = safeText(key);
    if (!text.isEmpty()) text[0] = text[0].toUpper();
    return text;
}

QString display(const QJsonValue& value, const QString& key = {}, const QJsonObject& row = {})
{
    if (value.isNull() || value.isUndefined()) return QStringLiteral("-");
    if (key == "directory" && value.isBool()) return value.toBool() ? "Folder" : "File";
    if (key == "hasSavedSession" || key == "hasSavedCredentials") return value.toBool() ? "Saved" : "None";
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) {
        if (key == "size") {
            if (row.value("directory").toBool() || value.toDouble() < 0) return QStringLiteral("-");
            auto bytes = value.toDouble();
            const QStringList units {"B","KiB","MiB","GiB","TiB"};
            int unit = 0;
            while (bytes >= 1024 && unit + 1 < units.size()) { bytes /= 1024; ++unit; }
            return QString::number(bytes, 'f', unit == 0 ? 0 : 2) + ' ' + units[unit];
        }
        return QString::number(value.toDouble(), 'g', 16);
    }
    if (value.isString()) return value.toString().isEmpty() ? QStringLiteral("-") : safeText(value.toString());
    QStringList parts;
    if (value.isArray()) for (const auto& part : value.toArray()) parts += display(part);
    return parts.isEmpty() ? QStringLiteral("-") : parts.join(", ");
}

void field(QString& output, const QString& name, const QString& value, int width, int labelWidth, int indent = 0)
{
    const auto prefix = QString(indent, ' ') + name + QString(std::max(0, labelWidth - textWidth(name)), ' ') + " : ";
    if (textWidth(prefix) + 2 > width) {
        for (const auto& line : wrap(name + " :",width - indent)) output += QString(indent,' ') + line + '\n';
        for (const auto& line : wrap(value,width - indent - 2)) output += QString(indent + 2,' ') + line + '\n';
        return;
    }
    const auto lines = wrap(value, std::max(2, width - textWidth(prefix)));
    for (qsizetype i = 0; i < lines.size(); ++i)
        output += (i == 0 ? prefix : QString(textWidth(prefix), ' ')) + lines[i] + '\n';
}

QStringList orderedKeys(const QJsonObject& object)
{
    QStringList result;
    const QStringList preferred {"id","name","type","address","username","hasSavedSession","hasSavedCredentials",
        "connected","sessionSaved","credentialsSaved","completed","succeeded","imported","duplicates","failed",
        "exported","backedUp","uploaded","target","retainedDirectory","digest","originalName","container","version",
        "index","state","source","output","remote","directory","size","modifiedAt","error"};
    for (const auto& key : preferred) if (object.contains(key)) result += key;
    for (const auto& key : object.keys()) if (!result.contains(key)) result += key;
    return result;
}

void object(QString& output, const QJsonObject& values, int width);
void table(QString& output, const QJsonArray& rows, const QString& command, int width)
{
    if (rows.isEmpty()) { output += "No entries.\n"; return; }
    if (!std::ranges::all_of(rows, [](const QJsonValue& row) { return row.isObject(); })) {
        for (const auto& row : rows) {
            if (row.isObject()) object(output, row.toObject(), width);
            else if (row.isArray()) table(output, row.toArray(), {}, width);
            else for (const auto& line : wrap(display(row), width)) output += line + '\n';
        }
        return;
    }
    QJsonObject combined;
    for (const auto& row : rows) for (const auto& key : row.toObject().keys()) combined.insert(key, {});
    auto keys = orderedKeys(combined);
    QStringList details;
    if (command == "service list") {
        keys = {"name","type","hasSavedSession","hasSavedCredentials"};
        details = {"id","address"};
    } else if (command == "tssl list") {
        keys = {"file","size","modifiedAt"};
        details = {"digest","file"};
    } else if (keys.removeOne("digest")) details += "digest";
    QList<int> widths, minimum;
    for (const auto& key : keys) {
        const auto header = textWidth(label(key));
        minimum += std::max(header, 4);
        auto natural = minimum.last();
        for (const auto& row : rows) {
            const auto values = row.toObject();
            auto cell = display(values.value(key), key, values);
            if (command == "tssl list" && key == "file") cell = QFileInfo(cell).fileName();
            natural = std::max(natural, textWidth(cell));
        }
        widths += natural;
    }
    const auto totalWidth = [&] { return std::accumulate(widths.begin(), widths.end(), 0) + 2 * (widths.size() - 1); };
    while (totalWidth() > width) {
        qsizetype shrink = -1;
        for (qsizetype i = 0; i < widths.size(); ++i)
            if (widths[i] > minimum[i] && (shrink < 0 || widths[i] > widths[shrink])) shrink = i;
        if (shrink < 0) break;
        --widths[shrink];
    }
    if (keys.isEmpty() || totalWidth() > width) {
        for (qsizetype i = 0; i < rows.size(); ++i) {
            output += QStringLiteral("[%1]\n").arg(i + 1);
            object(output, rows[i].toObject(), width);
            if (i + 1 < rows.size()) output += '\n';
        }
        return;
    }
    QStringList headers, rules;
    for (qsizetype i = 0; i < keys.size(); ++i) {
        headers += label(keys[i]) + QString(widths[i] - textWidth(label(keys[i])), ' ');
        rules += QString(widths[i], '-');
    }
    output += headers.join("  ").trimmed() + '\n' + rules.join("  ") + '\n';
    int detailWidth = 0;
    for (const auto& key : details) detailWidth = std::max(detailWidth, textWidth(key == "file" ? "Path" : label(key)));
    for (qsizetype rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const auto values = rows[rowIndex].toObject();
        QList<QStringList> cells;
        qsizetype height = 1;
        for (qsizetype i = 0; i < keys.size(); ++i) {
            auto cell = display(values.value(keys[i]), keys[i], values);
            if (command == "tssl list" && keys[i] == "file") cell = QFileInfo(cell).fileName();
            cells += wrap(cell, widths[i]); height = std::max(height, cells.last().size());
        }
        for (qsizetype line = 0; line < height; ++line) {
            QStringList parts;
            for (qsizetype i = 0; i < keys.size(); ++i) {
                const auto cell = cells[i].value(line);
                parts += cell + QString(std::max(0, widths[i] - textWidth(cell)), ' ');
            }
            auto lineText = parts.join("  ");
            while (lineText.endsWith(' ')) lineText.chop(1);
            output += lineText + '\n';
        }
        for (const auto& key : details) if (values.contains(key))
            field(output, key == "file" ? "Path" : label(key), display(values.value(key),key), width, detailWidth, 2);
        if ((!details.isEmpty() || height > 1) && rowIndex + 1 < rows.size()) output += '\n';
    }
}

bool nestedArray(const QString& key, const QJsonValue& value)
{
    return value.isArray() && (key == "items" || std::ranges::any_of(value.toArray(),
        [](const QJsonValue& item) { return item.isObject() || item.isArray(); }));
}

void object(QString& output, const QJsonObject& values, int width)
{
    const auto keys = orderedKeys(values);
    int labelWidth = 0;
    for (const auto& key : keys) labelWidth = std::max(labelWidth, textWidth(label(key)));
    for (const auto& key : keys) {
        const auto value = values.value(key);
        if (!value.isObject() && !nestedArray(key,value))
            field(output, label(key), display(value,key,values), width, labelWidth);
    }
    for (const auto& key : keys) {
        const auto value = values.value(key);
        if (value.isObject()) {
            output += '\n' + wrap(label(key),width).join('\n') + '\n';
            object(output, value.toObject(), width);
        } else if (nestedArray(key,value)) {
            output += '\n' + wrap(label(key) + QStringLiteral(" (%1)").arg(value.toArray().size()),width).join('\n') + '\n';
            table(output, value.toArray(), {}, width);
        }
    }
}

QJsonValue sanitized(const QJsonValue& value)
{
    if (value.isArray()) {
        QJsonArray result;
        for (const auto& item : value.toArray()) result += sanitized(item);
        return result;
    }
    if (value.isObject()) {
        auto result = value.toObject();
        for (auto it = result.begin(); it != result.end(); ++it)
            it.value() = (it.key() == "error" || it.key() == "message")
                ? QJsonValue(ErrorPresentation::sanitizeDetails(it.value().toString())) : sanitized(it.value());
        return result;
    }
    return value;
}

QString title(const QString& command)
{
    static const QMap<QString,QString> titles {
        {"service list","Services"}, {"service show","Service"}, {"tssl list","TSSL packages"},
        {"webdav ls","Directory entries"}, {"webdav stat","Entry"}, {"config list","Configuration"},
        {"video pack","Video packaging"}, {"video restore","Video restore"},
    };
    return titles.value(command, command);
}
}

Cli::RenderedOutput Cli::renderResult(const Invocation& invocation, const Result& result, int columns)
{
    RenderedOutput output;
    const auto message = ErrorPresentation::sanitizeDetails(result.message);
    const auto data = result.data.isUndefined() ? QJsonValue(QJsonValue::Null) : sanitized(result.data);
    if (invocation.has("json")) {
        const QJsonObject envelope {{"schemaVersion",1}, {"command",invocation.command},
            {"ok",result.code == ExitCode::Success}, {"data",data},
            {"error",result.code == ExitCode::Success ? QJsonValue(QJsonValue::Null) :
                QJsonValue(QJsonObject {{"code",static_cast<int>(result.code)}, {"message",message}})}};
        output.standardOutput = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact)) + '\n';
        if (result.code != ExitCode::Success) output.standardError = message + '\n';
        return output;
    }
    columns = std::clamp(columns, 20, 240);
    if (data.isArray()) {
        output.standardOutput = wrap(title(invocation.command) + QStringLiteral(" (%1)").arg(data.toArray().size()),columns).join('\n') + "\n\n";
        table(output.standardOutput, data.toArray(), invocation.command, columns);
    } else if (data.isObject()) {
        output.standardOutput = wrap(title(invocation.command),columns).join('\n') + "\n\n";
        object(output.standardOutput, data.toObject(), columns);
    } else if (!data.isNull()) {
        const auto text = invocation.command == "help" ? data.toString() : display(data);
        for (const auto& line : text.split('\n'))
            for (const auto& wrapped : wrap(line,columns)) output.standardOutput += wrapped + '\n';
    }
    if (result.code != ExitCode::Success) {
        const QMap<ExitCode,QString> reasons {{ExitCode::Failed,"execution failed"}, {ExitCode::Usage,"invalid arguments"},
            {ExitCode::Authentication,"authentication failed"}, {ExitCode::NotFound,"not found"},
            {ExitCode::Conflict,"conflict"}, {ExitCode::Partial,"partial failure"}, {ExitCode::Canceled,"canceled"}};
        const auto heading = QStringLiteral("%1 [%2: %3]").arg(safeText(invocation.command))
            .arg(static_cast<int>(result.code)).arg(reasons.value(result.code));
        output.standardError = wrap(heading,columns).join('\n') + '\n';
        for (const auto& line : wrap(safeText(message),columns - 2)) output.standardError += "  " + line + '\n';
    }
    return output;
}
