#pragma once

#include "models/ServiceCard.h"
#include <QObject>
#include <QString>
#include <vector>

// Asynchronous read-only snapshots. Only the latest requested snapshot is
// published; concurrent refresh requests share a single in-flight worker.
class ServiceCardLoader final : public QObject {
    Q_OBJECT
public:
    using Cards = std::vector<ServiceCard>;
    explicit ServiceCardLoader(QObject* parent = nullptr, QString databasePath = {});
    void refresh();
signals:
    void ready(const Cards& cards);
    void failed(const QString& message);
private:
    void schedule();
    void start();
    QString m_databasePath;
    quint64 m_generation {};
    bool m_running {};
    bool m_scheduled {};
};
