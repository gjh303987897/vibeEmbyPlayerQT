#include "database/SessionRepository.h"
#include "database/SessionTokenCipher.h"
#include "services/credentials/CredentialStore.h"
#include <QFile>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <wincred.h>
#endif

namespace {
QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
UserSession session(const QString& name = QStringLiteral("server"))
{
    return {.server = {.id = name, .name = name, .baseUrl = "https://example.test/", .username = "user",
                       .serviceType = ServiceType::Emby}, .userId = "user-id", .username = "user",
            .accessToken = QStringLiteral("test-token-中文-") + id(), .createdAt = QDateTime::currentDateTimeUtc()};
}
QString reference(const QString& connection, const QString& serverId = QStringLiteral("server"))
{
    QSqlQuery query(QSqlDatabase::database(connection));
    query.prepare("SELECT access_token FROM sessions WHERE server_id = :id LIMIT 1");
    query.bindValue(":id", serverId);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}
int pending(const QString& connection)
{
    QSqlQuery query(QSqlDatabase::database(connection));
    return query.exec("SELECT COUNT(*) FROM session_credential_cleanup") && query.next() ? query.value(0).toInt() : -1;
}
#ifdef Q_OS_WIN
QSet<QString> credentialTargets()
{
    DWORD count = 0;
    PCREDENTIALW* entries = nullptr;
    QSet<QString> names;
    if (CredEnumerateW(L"vibePlayerQT/Secret/Session/*", 0, &count, &entries)) {
        for (DWORD index = 0; index < count; ++index) names.insert(QString::fromWCharArray(entries[index]->TargetName));
        CredFree(entries);
    }
    return names;
}
#endif
}

class SessionCredentialsTest final : public QObject {
    Q_OBJECT
private slots:
    void nativeRoundTripRejectsMissingAndMalformedReferences()
    {
        const auto token = QStringLiteral("native-中文-secret-") + id();
        const auto saved = SessionTokenCipher::protect(token, SessionTokenCipher::Protection::SystemCredentials);
        QVERIFY2(saved.has_value(), qPrintable(saved ? QString() : saved.error()));
        const auto cleanup = qScopeGuard([&] { static_cast<void>(SessionTokenCipher::discard(*saved)); });
        QVERIFY(SessionTokenCipher::isCredentialReference(*saved));
        QCOMPARE(SessionTokenCipher::unprotect(*saved).value(), token);
        QVERIFY(!SessionTokenCipher::unprotect("keychain:v1:../OtherCredential"));
        QVERIFY(!SessionTokenCipher::discard("keychain:v1:../OtherCredential"));
        QVERIFY(SessionTokenCipher::discard(*saved));
        QVERIFY(!SessionTokenCipher::unprotect(*saved));
        QVERIFY(SessionTokenCipher::discard(*saved)); // Idempotent deletion.
    }
    void replacementAndReadOnlyAccess()
    {
        QTemporaryDir directory;
        const auto connection = id(), path = directory.filePath("db.sqlite");
        SessionRepository repository(connection, path, SessionRepository::OpenMode::ReadWrite,
                                     SessionTokenCipher::Protection::SystemCredentials);
        QVERIFY(repository.initialize());
        const auto cleanup = qScopeGuard([&] { static_cast<void>(repository.clearSession()); });
        auto first = session();
        QVERIFY(repository.saveSession(first));
        const auto original = reference(connection);
        QVERIFY(SessionTokenCipher::isCredentialReference(original));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains(first.accessToken.toUtf8())); file.close();
        SessionRepository reader(id(), path, SessionRepository::OpenMode::ReadOnly);
        auto loaded = reader.loadSession(first.server.id);
        QVERIFY(loaded && loaded->has_value()); QCOMPARE((**loaded).accessToken, first.accessToken);
        QVERIFY(!reader.saveSession(first));
        first.accessToken = "replacement-" + id();
        QVERIFY(repository.saveSession(first));
        QVERIFY(reference(connection) != original);
        QVERIFY(!SessionTokenCipher::unprotect(original));
        QCOMPARE(SessionTokenCipher::unprotect(reference(connection)).value(), first.accessToken);
        QCOMPARE(pending(connection), 0);
    }
    void failedSaveKeepsOldSessionAndDiscardsNewCredential()
    {
        QTemporaryDir directory;
        const auto connection = id();
        SessionRepository repository(connection, directory.filePath("db.sqlite"), SessionRepository::OpenMode::ReadWrite,
                                     SessionTokenCipher::Protection::SystemCredentials);
        QVERIFY(repository.initialize());
        const auto cleanup = qScopeGuard([&] { static_cast<void>(repository.clearSession()); });
        auto item = session(); QVERIFY(repository.saveSession(item));
        const auto original = reference(connection);
#ifdef Q_OS_WIN
        const auto names = credentialTargets();
#endif
        QSqlQuery query(QSqlDatabase::database(connection));
        QVERIFY(query.exec("CREATE TRIGGER reject_session BEFORE UPDATE ON sessions BEGIN SELECT RAISE(ABORT,'injected'); END"));
        item.accessToken = "new-uncommitted-" + id();
        QVERIFY(!repository.saveSession(item));
        QCOMPARE(reference(connection), original);
        QVERIFY(SessionTokenCipher::unprotect(original));
        QCOMPARE(pending(connection), 0);
#ifdef Q_OS_WIN
        QCOMPARE(credentialTargets(), names);
#endif
        QVERIFY(query.exec("DROP TRIGGER reject_session"));
    }
    void credentialFailureNeverWritesOrOverwritesPlaintext()
    {
        QTemporaryDir directory;
        const auto connection = id();
        SessionRepository repository(connection, directory.filePath("db.sqlite"), SessionRepository::OpenMode::ReadWrite,
                                     SessionTokenCipher::Protection::SystemCredentials);
        QVERIFY(repository.initialize());
        const auto cleanup = qScopeGuard([&] { static_cast<void>(repository.clearSession()); });
        auto item = session(); QVERIFY(repository.saveSession(item));
        const auto original = reference(connection);
        item.accessToken += QChar::Null;
        QVERIFY(!repository.saveSession(item));
        QCOMPARE(reference(connection), original);
        item.server.id = "new-server";
        QVERIFY(!repository.saveSession(item));
        QCOMPARE(repository.loadAllServiceCards()->size(), size_t(1));
    }
    void serverRemovalRollsBackThenCleansAllUserCredentials()
    {
        QTemporaryDir directory;
        const auto connection = id();
        SessionRepository repository(connection, directory.filePath("db.sqlite"), SessionRepository::OpenMode::ReadWrite,
                                     SessionTokenCipher::Protection::SystemCredentials);
        QVERIFY(repository.initialize());
        const auto cleanup = qScopeGuard([&] { static_cast<void>(repository.clearSession()); });
        auto item = session(); QVERIFY(repository.saveSession(item));
        const auto first = reference(connection);
        item.server.username = "another"; item.username = "another";
        QVERIFY(repository.saveSession(item));
        QSqlQuery query(QSqlDatabase::database(connection));
        QVERIFY(query.exec("SELECT access_token FROM sessions"));
        QStringList refs; while (query.next()) refs.append(query.value(0).toString()); query.finish();
        QCOMPARE(refs.size(), 2);
        QVERIFY(query.exec("CREATE TRIGGER reject_server BEFORE DELETE ON servers BEGIN SELECT RAISE(ABORT,'injected'); END"));
        QVERIFY(!repository.deleteServer(item.server.id, true));
        for (const auto& ref : refs) QVERIFY(SessionTokenCipher::unprotect(ref));
        QVERIFY(query.exec("DROP TRIGGER reject_server"));
        QVERIFY(repository.deleteServer(item.server.id, false)); // Hiding retains local sessions.
        QVERIFY(SessionTokenCipher::unprotect(first));
        QVERIFY(repository.deleteServer(item.server.id, true));
        for (const auto& ref : refs) QVERIFY(!SessionTokenCipher::unprotect(ref));
        QCOMPARE(pending(connection), 0);
    }
    void migrationIsAtomicAndCleansCredentialsOnFailure()
    {
        QTemporaryDir directory;
        const auto connection = id(), path = directory.filePath("db.sqlite");
        SessionRepository repository(connection, path, SessionRepository::OpenMode::ReadWrite,
                                     SessionTokenCipher::Protection::SystemCredentials);
        QVERIFY(repository.initialize());
        auto item = session(); QVERIFY(repository.saveServer(item.server));
        auto other = session("other"); QVERIFY(repository.saveServer(other.server));
        QSqlQuery query(QSqlDatabase::database(connection));
        query.prepare("INSERT INTO sessions (server_id,user_id,username,access_token,created_at) VALUES (:id,'user','user',:token,'2026-01-01')");
        query.bindValue(":id", item.server.id); query.bindValue(":token", "legacy-first-" + id()); QVERIFY(query.exec());
        const auto original = reference(connection);
        query.bindValue(":id", other.server.id); query.bindValue(":token", QString("invalid") + QChar::Null); QVERIFY(query.exec()); query.finish();
#ifdef Q_OS_WIN
        const auto names = credentialTargets();
#endif
        QVERIFY(!repository.initialize());
        QCOMPARE(reference(connection), original);
        QCOMPARE(pending(connection), 0);
#ifdef Q_OS_WIN
        QCOMPARE(credentialTargets(), names);
#endif
        QVERIFY(query.exec("DELETE FROM sessions WHERE server_id='other'"));
        QVERIFY(repository.initialize());
        const auto cleanup = qScopeGuard([&] { static_cast<void>(repository.clearSession()); });
        const auto migrated = reference(connection);
        QVERIFY(SessionTokenCipher::isCredentialReference(migrated));
        QCOMPARE(SessionTokenCipher::unprotect(migrated).value(), original);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(!file.readAll().contains(original.toUtf8()));
    }
    void durableCleanupRetainsReferencedEntriesAndRetriesOrphans()
    {
        QTemporaryDir directory;
        const auto connection = id();
        SessionRepository repository(connection, directory.filePath("db.sqlite"), SessionRepository::OpenMode::ReadWrite,
                                     SessionTokenCipher::Protection::SystemCredentials);
        QVERIFY(repository.initialize());
        const auto cleanup = qScopeGuard([&] { static_cast<void>(repository.clearSession()); });
        const auto item = session(); QVERIFY(repository.saveSession(item));
        const auto current = reference(connection);
        const auto orphan = SessionTokenCipher::protect("orphan-" + id(), SessionTokenCipher::Protection::SystemCredentials);
        QVERIFY(orphan);
        const auto clearOrphan = qScopeGuard([&] { static_cast<void>(SessionTokenCipher::discard(*orphan)); });
        QSqlQuery query(QSqlDatabase::database(connection));
        query.prepare("INSERT INTO session_credential_cleanup VALUES (:ref)");
        for (const auto& ref : {current, *orphan, QString("keychain:v1:invalid")}) {
            query.bindValue(":ref", ref); QVERIFY(query.exec());
        }
        query.finish();
        QVERIFY(repository.initialize());
        QVERIFY(SessionTokenCipher::unprotect(current));
        QVERIFY(!SessionTokenCipher::unprotect(*orphan));
        QCOMPARE(pending(connection), 0);
        QVERIFY(repository.clearServerSession(item.server.id));
        QVERIFY(!SessionTokenCipher::unprotect(current));
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().contains("--probe-unavailable")) {
        const auto result = SessionTokenCipher::protect("fake-ci-token", SessionTokenCipher::Protection::SystemCredentials);
        if (result) { static_cast<void>(SessionTokenCipher::discard(*result)); return 1; }
        return 0;
    }
    SessionCredentialsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "SessionCredentialsTest.moc"
