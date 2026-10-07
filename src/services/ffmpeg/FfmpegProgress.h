#pragma once
#include <QByteArray>
#include <optional>

namespace FfmpegProgress {
struct Field { QByteArray key; QByteArray value; };
// FFmpeg -progress emits key=value lines. Ignore malformed diagnostics safely.
inline std::optional<Field> parseLine(QByteArray line)
{
    line = line.trimmed();
    const auto separator = line.indexOf('=');
    if (separator <= 0) return std::nullopt;
    return Field {line.first(separator), line.mid(separator + 1)};
}
}
