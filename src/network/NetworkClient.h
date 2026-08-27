#pragma once

#include "network/NetworkError.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

#include <expected>
#include <functional>

struct NetworkResponse {
    int statusCode { 0 };
    QByteArray body;
    QHash<QByteArray, QByteArray> headers;
};

using NetworkResult = std::expected<NetworkResponse, NetworkError>;
using NetworkCallback = std::function<void(NetworkResult)>;

class NetworkClient final : public QObject {
    Q_OBJECT

public:
    explicit NetworkClient(QObject* parent = nullptr);

    void preconnect(const QUrl& url, bool allowSelfSigned);
    void get(const QUrl& url, const QHash<QByteArray, QByteArray>& headers, bool allowSelfSigned, NetworkCallback callback);
    void postJson(const QUrl& url, const QHash<QByteArray, QByteArray>& headers, const QJsonObject& body, bool allowSelfSigned, NetworkCallback callback);

signals:
    void networkTrafficSample(qint64 bytesReceived, qint64 bytesSent);

private:
    void send(QNetworkAccessManager::Operation operation,
              const QUrl& url,
              const QHash<QByteArray, QByteArray>& headers,
              const QByteArray& body,
              bool allowSelfSigned,
              NetworkCallback callback);

    QNetworkAccessManager m_manager;
};
