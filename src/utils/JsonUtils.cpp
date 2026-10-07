#include "utils/JsonUtils.h"

#include <QJsonValue>
#include <limits>
#include <optional>
#include <cmath>

namespace {
std::optional<qint64> integerValue(const QJsonValue& value)
{
    if (!value.isDouble()) return std::nullopt;
    const auto low = value.toInteger(std::numeric_limits<qint64>::min());
    const auto high = value.toInteger(std::numeric_limits<qint64>::max());
    if (low != high) return std::nullopt;
    return low;
}
}

QString jsonString(const QJsonObject& object, const QString& key)
{
    const auto value = object.value(key);
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        if (const auto integer = integerValue(value)) return QString::number(*integer);
        return QString::number(value.toDouble(), 'g', std::numeric_limits<double>::max_digits10);
    }
    return {};
}

QString jsonStringAny(const QJsonObject& object, std::initializer_list<QString> keys)
{
    for (const auto& key : keys) {
        const auto value = jsonString(object, key);
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

int jsonIntAny(const QJsonObject& object, std::initializer_list<QString> keys, int fallback)
{
    for (const auto& key : keys) {
        const auto value = object.value(key);
        if (value.isDouble()) {
            if (const auto integer = integerValue(value); integer &&
                *integer >= std::numeric_limits<int>::min() && *integer <= std::numeric_limits<int>::max())
                return static_cast<int>(*integer);
        }
        if (value.isString()) {
            bool ok = false;
            const auto parsed = value.toString().toInt(&ok);
            if (ok) {
                return parsed;
            }
        }
    }
    return fallback;
}

qint64 jsonInt64Any(const QJsonObject& object, std::initializer_list<QString> keys, qint64 fallback)
{
    for (const auto& key : keys) {
        const auto value = object.value(key);
        if (value.isDouble()) {
            if (const auto integer = integerValue(value)) return *integer;
        }
        if (value.isString()) {
            bool ok = false;
            const auto parsed = value.toString().toLongLong(&ok);
            if (ok) {
                return parsed;
            }
        }
    }
    return fallback;
}

double jsonDoubleAny(const QJsonObject& object, std::initializer_list<QString> keys, double fallback)
{
    for (const auto& key : keys) {
        const auto value = object.value(key);
        if (value.isDouble()) {
            return value.toDouble();
        }
        if (value.isString()) {
            bool ok = false;
            const auto parsed = value.toString().toDouble(&ok);
            if (ok) {
                return parsed;
            }
        }
    }
    return fallback;
}

bool jsonBoolAny(const QJsonObject& object, std::initializer_list<QString> keys, bool fallback)
{
    for (const auto& key : keys) {
        const auto value = object.value(key);
        if (value.isBool()) {
            return value.toBool();
        }
    }
    return fallback;
}
