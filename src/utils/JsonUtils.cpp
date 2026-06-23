#include "utils/JsonUtils.h"

#include <QJsonValue>

QString jsonString(const QJsonObject& object, const QString& key)
{
    const auto value = object.value(key);
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble());
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
            return value.toInt();
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
            return static_cast<qint64>(value.toDouble());
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
