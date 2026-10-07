#pragma once

#include <QNetworkReply>
#include <QPointer>
#include <QTcpSocket>
#include <functional>
#include <algorithm>

// Bridges a sequential HTTP reply to a slow local consumer without retaining
// the entire response. The reply remains alive until its buffered tail drains.
class BoundedReplyWriter final : public QObject {
public:
    static constexpr qint64 maximumBufferedBytes = 256 * 1024;
    BoundedReplyWriter(QNetworkReply* reply, QTcpSocket* socket,
                       std::function<void()> sendHeaders,
                       std::function<void(qint64)> received)
        : QObject(reply), m_reply(reply), m_socket(socket),
          m_sendHeaders(std::move(sendHeaders)), m_received(std::move(received))
    {
        reply->setReadBufferSize(maximumBufferedBytes);
        connect(reply, &QNetworkReply::readyRead, this, [this] { pump(); });
        connect(reply, &QNetworkReply::finished, this, [this] { m_finished = true; pump(); });
        connect(socket, &QTcpSocket::bytesWritten, this, [this] { pump(); });
        connect(socket, &QTcpSocket::disconnected, this, [this] {
            m_closed = true;
            if (m_reply->isRunning()) m_reply->abort();
            m_reply->deleteLater();
        });
    }

private:
    void pump()
    {
        if (m_closed) return;
        if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
            m_closed = true;
            if (m_reply->isRunning()) m_reply->abort();
            m_reply->deleteLater();
            return;
        }
        m_sendHeaders();
        while (m_reply->isOpen() && m_reply->bytesAvailable() > 0) {
            const auto available = maximumBufferedBytes - m_socket->bytesToWrite();
            if (available <= 0) break;
            const auto chunk = m_reply->read(std::min<qint64>(available, 64 * 1024));
            if (chunk.isEmpty()) break;
            m_received(chunk.size());
            if (m_socket->write(chunk) != chunk.size()) {
                m_socket->abort();
                return;
            }
        }
        m_reply->setProperty("downstreamBlocked", m_socket->bytesToWrite() >= maximumBufferedBytes);
        if (m_finished && (!m_reply->isOpen() || m_reply->bytesAvailable() == 0)) {
            m_closed = true;
            m_socket->disconnectFromHost();
            m_reply->deleteLater();
        }
    }

    QNetworkReply* m_reply;
    QPointer<QTcpSocket> m_socket;
    std::function<void()> m_sendHeaders;
    std::function<void(qint64)> m_received;
    bool m_finished { false };
    bool m_closed { false };
};
