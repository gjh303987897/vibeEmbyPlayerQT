#include "utils/NetworkTrafficCoalescer.h"

#include <algorithm>

namespace {
constexpr int coalesceIntervalMs = 200;
}

NetworkTrafficCoalescer::NetworkTrafficCoalescer(QObject* parent)
    : QObject(parent)
{
    m_timer.setInterval(coalesceIntervalMs);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &NetworkTrafficCoalescer::flush);
}

void NetworkTrafficCoalescer::record(const QString& serviceId,
                                     const QString& serviceName,
                                     const QString& serviceType,
                                     qint64 bytesReceived,
                                     qint64 bytesSent)
{
    if (serviceId.isEmpty() || (bytesReceived <= 0 && bytesSent <= 0)) {
        return;
    }
    auto& pending = m_pending[serviceId];
    // Names and types come from immutable per-request server snapshots; the
    // first sample of a window fixes them for the flush.
    if (pending.serviceName.isEmpty() && !serviceName.isEmpty()) {
        pending.serviceName = serviceName;
    }
    if (pending.serviceType.isEmpty() && !serviceType.isEmpty()) {
        pending.serviceType = serviceType;
    }
    pending.bytesReceived += std::max<qint64>(0, bytesReceived);
    pending.bytesSent += std::max<qint64>(0, bytesSent);
    if (!m_timer.isActive()) {
        m_timer.start();
    }
}

void NetworkTrafficCoalescer::flush()
{
    if (m_pending.isEmpty()) {
        return;
    }
    const auto pending = std::exchange(m_pending, {});
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        emit flushed(it.key(), it.value().serviceName, it.value().serviceType,
                     it.value().bytesReceived, it.value().bytesSent);
    }
}
