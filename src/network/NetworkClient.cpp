#include "network/NetworkClient.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <utility>

namespace {
constexpr auto requestTimeoutMs = 30000;

NetworkError makeReplyError(QNetworkReply* reply)
{
    const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode >= 400) {
        return {
            .kind = NetworkErrorKind::Http,
            .message = QStringLiteral("HTTP %1").arg(statusCode),
            .httpStatus = statusCode,
        };
    }

    return {
        .kind = NetworkErrorKind::Transport,
        .message = reply->errorString(),
        .httpStatus = statusCode,
    };
}
}

NetworkClient::NetworkClient(QObject* parent)
    : QObject(parent)
{
}

void NetworkClient::get(const QUrl& url,
                        const QHash<QByteArray, QByteArray>& headers,
                        bool allowSelfSigned,
                        NetworkCallback callback)
{
    send(QNetworkAccessManager::GetOperation, url, headers, {}, allowSelfSigned, std::move(callback));
}

void NetworkClient::postJson(const QUrl& url,
                             const QHash<QByteArray, QByteArray>& headers,
                             const QJsonObject& body,
                             bool allowSelfSigned,
                             NetworkCallback callback)
{
    send(QNetworkAccessManager::PostOperation,
         url,
         headers,
         QJsonDocument(body).toJson(QJsonDocument::Compact),
         allowSelfSigned,
         std::move(callback));
}

void NetworkClient::send(QNetworkAccessManager::Operation operation,
                         const QUrl& url,
                         const QHash<QByteArray, QByteArray>& headers,
                         const QByteArray& body,
                         bool allowSelfSigned,
                         NetworkCallback callback)
{
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
        callback(std::unexpected(NetworkError {
            .kind = NetworkErrorKind::InvalidUrl,
            .message = QStringLiteral("Invalid server URL"),
        }));
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT/0.1"));
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        request.setRawHeader(it.key(), it.value());
    }

    QNetworkReply* reply = nullptr;
    switch (operation) {
    case QNetworkAccessManager::GetOperation:
        reply = m_manager.get(request);
        break;
    case QNetworkAccessManager::PostOperation:
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        reply = m_manager.post(request, body);
        break;
    default:
        callback(std::unexpected(NetworkError {
            .kind = NetworkErrorKind::Transport,
            .message = QStringLiteral("Unsupported network operation"),
        }));
        return;
    }

    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, reply, [reply]() {
        reply->abort();
    });
    timeout->start(requestTimeoutMs);

    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [this, reply, allowSelfSigned](const QList<QSslError>& errors) {
        if (!allowSelfSigned) {
            return;
        }

        bool accepted = false;
        QEventLoop loop;
        const auto host = reply->url().host();
        emit certificateConfirmationRequired(host, errors, [&loop, &accepted](bool userAccepted) {
            accepted = userAccepted;
            loop.quit();
        });
        loop.exec();

        if (accepted) {
            reply->ignoreSslErrors();
        }
    });

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback = std::move(callback)]() mutable {
        const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto body = reply->readAll();

        if (reply->error() != QNetworkReply::NoError) {
            const auto kind = reply->error() == QNetworkReply::OperationCanceledError
                ? NetworkErrorKind::Timeout
                : NetworkErrorKind::Transport;

            auto error = makeReplyError(reply);
            if (kind == NetworkErrorKind::Timeout) {
                error.kind = NetworkErrorKind::Timeout;
                error.message = QStringLiteral("Network request timed out");
            }
            callback(std::unexpected(error));
            reply->deleteLater();
            return;
        }

        if (statusCode >= 400) {
            callback(std::unexpected(NetworkError {
                .kind = NetworkErrorKind::Http,
                .message = QStringLiteral("HTTP %1").arg(statusCode),
                .httpStatus = statusCode,
            }));
            reply->deleteLater();
            return;
        }

        NetworkResponse response;
        response.statusCode = statusCode;
        response.body = body;

        const auto rawHeaders = reply->rawHeaderPairs();
        for (const auto& header : rawHeaders) {
            response.headers.insert(header.first, header.second);
        }

        callback(std::move(response));
        reply->deleteLater();
    });
}
