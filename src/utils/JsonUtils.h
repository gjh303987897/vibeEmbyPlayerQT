#pragma once

#include <QJsonObject>
#include <QtGlobal>
#include <QString>

#include <initializer_list>

QString jsonString(const QJsonObject& object, const QString& key);
QString jsonStringAny(const QJsonObject& object, std::initializer_list<QString> keys);
int jsonIntAny(const QJsonObject& object, std::initializer_list<QString> keys, int fallback = 0);
qint64 jsonInt64Any(const QJsonObject& object, std::initializer_list<QString> keys, qint64 fallback = 0);
double jsonDoubleAny(const QJsonObject& object, std::initializer_list<QString> keys, double fallback = 0.0);
bool jsonBoolAny(const QJsonObject& object, std::initializer_list<QString> keys, bool fallback = false);
