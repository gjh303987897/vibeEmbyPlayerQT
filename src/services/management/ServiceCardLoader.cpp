#include "services/management/ServiceCardLoader.h"
#include "database/SessionRepository.h"
#include <QFutureWatcher>
#include <QTimer>
#include <QUuid>
#include <QtConcurrentRun>
#include <expected>
#include <utility>

ServiceCardLoader::ServiceCardLoader(QObject* parent, QString databasePath)
    : QObject(parent), m_databasePath(std::move(databasePath))
{
    qRegisterMetaType<Cards>();
}

void ServiceCardLoader::refresh()
{
    ++m_generation;
    schedule();
}

void ServiceCardLoader::schedule()
{
    if (m_running || m_scheduled) return;
    m_scheduled = true;
    QTimer::singleShot(0, this, &ServiceCardLoader::start);
}

void ServiceCardLoader::start()
{
    m_scheduled = false;
    m_running = true;
    const auto generation = m_generation;
    using Result = std::expected<Cards, QString>;
    auto* watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        auto result = watcher->future().takeResult();
        watcher->deleteLater();
        m_running = false;
        if (generation != m_generation) {
            schedule();
            return;
        }
        if (result) emit ready(*result);
        else emit failed(result.error());
    });
    // Each worker creates and destroys its own connection on the same thread.
    // It captures no loader/VM pointer and never performs initialization writes.
    watcher->setFuture(QtConcurrent::run([path = m_databasePath] {
        SessionRepository repository(QStringLiteral("service_cards_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)),
                                     path, SessionRepository::OpenMode::ReadOnly);
        return repository.loadAllServiceCards();
    }));
}
