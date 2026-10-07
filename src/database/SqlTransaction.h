#pragma once

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <expected>
#include <utility>

// Keep an unsuccessful commit from leaving the shared connection in a transaction.
class SqlTransaction final {
public:
    explicit SqlTransaction(QSqlDatabase database) : m_database(std::move(database)) {}
    SqlTransaction(const SqlTransaction&) = delete;
    SqlTransaction& operator=(const SqlTransaction&) = delete;
    ~SqlTransaction() { rollback(); }

    std::expected<void, QString> begin()
    {
        QSqlQuery query(m_database);
        if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
            return std::unexpected(query.lastError().text());
        }
        m_active = true;
        return {};
    }

    std::expected<void, QString> commit()
    {
        QSqlQuery query(m_database);
        if (!query.exec(QStringLiteral("COMMIT"))) {
            const auto error = query.lastError().text();
            rollback();
            return std::unexpected(error);
        }
        m_active = false;
        return {};
    }

private:
    void rollback()
    {
        if (!std::exchange(m_active, false)) return;
        QSqlQuery query(m_database);
        if (!query.exec(QStringLiteral("ROLLBACK"))) {
            // SQLite can already have rolled back after an I/O failure. Closing
            // also guarantees cleanup if the rollback itself failed.
            m_database.close();
        }
    }
    QSqlDatabase m_database;
    bool m_active = false;
};
