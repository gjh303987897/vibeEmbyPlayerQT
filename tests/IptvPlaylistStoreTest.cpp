#include "services/iptv/IptvPlaylistStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

namespace {
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

class IptvPlaylistStoreTest final : public QObject {
    Q_OBJECT

private slots:
    void keepsManagedCopyAfterSourceIsRemoved();
    void safelyReimportsManagedCopy();
    void atomicallyReplacesExistingCopy();
    void rejectsUnsupportedFiles();
};

void IptvPlaylistStoreTest::keepsManagedCopyAfterSourceIsRemoved()
{
    QTemporaryDir sourceDirectory;
    QTemporaryDir storageDirectory;
    QVERIFY(sourceDirectory.isValid());
    QVERIFY(storageDirectory.isValid());

    const auto sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("channels.m3u8"));
    const QByteArray contents = "#EXTM3U\nhttps://example.com/live.m3u8\n";
    QVERIFY(writeFile(sourcePath, contents));

    const auto imported = IptvPlaylistStore::importFile(sourcePath,
                                                        QStringLiteral("service-one"),
                                                        storageDirectory.path());
    QVERIFY(imported.has_value());
    QVERIFY(QFileInfo(*imported).absoluteFilePath().startsWith(QFileInfo(storageDirectory.path()).absoluteFilePath()));
    QVERIFY(QFile::remove(sourcePath));

    QFile managed(*imported);
    QVERIFY(managed.open(QIODevice::ReadOnly));
    QCOMPARE(managed.readAll(), contents);
}

void IptvPlaylistStoreTest::safelyReimportsManagedCopy()
{
    QTemporaryDir sourceDirectory;
    QTemporaryDir storageDirectory;
    QVERIFY(sourceDirectory.isValid());
    QVERIFY(storageDirectory.isValid());

    const auto sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("channels.m3u"));
    const QByteArray contents = "#EXTM3U\nhttps://example.com/channel.ts\n";
    QVERIFY(writeFile(sourcePath, contents));

    const auto firstImport = IptvPlaylistStore::importFile(sourcePath,
                                                           QStringLiteral("service-two"),
                                                           storageDirectory.path());
    QVERIFY(firstImport.has_value());
    const auto secondImport = IptvPlaylistStore::importFile(*firstImport,
                                                            QStringLiteral("service-two"),
                                                            storageDirectory.path());
    QVERIFY(secondImport.has_value());
    QCOMPARE(*secondImport, *firstImport);

    QFile managed(*secondImport);
    QVERIFY(managed.open(QIODevice::ReadOnly));
    QCOMPARE(managed.readAll(), contents);
}

void IptvPlaylistStoreTest::atomicallyReplacesExistingCopy()
{
    QTemporaryDir sourceDirectory;
    QTemporaryDir storageDirectory;
    QVERIFY(sourceDirectory.isValid());
    QVERIFY(storageDirectory.isValid());

    const auto sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("channels.m3u8"));
    QVERIFY(writeFile(sourcePath, QByteArrayLiteral("#EXTM3U\nold\n")));
    const auto firstImport = IptvPlaylistStore::importFile(sourcePath,
                                                           QStringLiteral("service-three"),
                                                           storageDirectory.path());
    QVERIFY(firstImport.has_value());

    QVERIFY(writeFile(sourcePath, QByteArrayLiteral("#EXTM3U\nnew\n")));
    const auto secondImport = IptvPlaylistStore::importFile(sourcePath,
                                                            QStringLiteral("service-three"),
                                                            storageDirectory.path());
    QVERIFY(secondImport.has_value());
    QCOMPARE(*secondImport, *firstImport);

    QFile managed(*secondImport);
    QVERIFY(managed.open(QIODevice::ReadOnly));
    QCOMPARE(managed.readAll(), QByteArrayLiteral("#EXTM3U\nnew\n"));
}

void IptvPlaylistStoreTest::rejectsUnsupportedFiles()
{
    QTemporaryDir sourceDirectory;
    QTemporaryDir storageDirectory;
    QVERIFY(sourceDirectory.isValid());
    QVERIFY(storageDirectory.isValid());

    const auto sourcePath = QDir(sourceDirectory.path()).filePath(QStringLiteral("channels.txt"));
    QVERIFY(writeFile(sourcePath, QByteArrayLiteral("#EXTM3U\n")));
    QVERIFY(!IptvPlaylistStore::importFile(sourcePath,
                                          QStringLiteral("service-four"),
                                          storageDirectory.path()).has_value());
}

QTEST_MAIN(IptvPlaylistStoreTest)
#include "IptvPlaylistStoreTest.moc"
