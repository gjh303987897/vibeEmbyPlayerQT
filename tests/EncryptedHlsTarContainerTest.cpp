#include "services/encryptedhls/EncryptedHlsTarContainer.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

namespace {
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

class EncryptedHlsTarContainerTest final : public QObject {
    Q_OBJECT

private slots:
    void createsIndexedTarAndReadsEntries();
    void supportsPaxLongPaths();
    void rejectsUnsafeIndexEntry();
};

void EncryptedHlsTarContainerTest::createsIndexedTarAndReadsEntries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("index.m3u8s")), QByteArrayLiteral("#EXTM3U\n")));
    QVERIFY(QDir().mkpath(temporary.filePath(QStringLiteral("segments"))));
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("segments/000001.ts")), QByteArrayLiteral("encrypted-segment")));

    const auto archivePath = temporary.filePath(QStringLiteral("movie.m3u8sp"));
    const auto built = EncryptedHlsTarContainer::build(temporary.path(), archivePath, QStringLiteral("index.m3u8s"));
    if (!built) QFAIL(qPrintable(built.error()));
    QVERIFY(QFileInfo(archivePath).isFile());
    QCOMPARE(built->containerLength, QFileInfo(archivePath).size());
    QVERIFY(!built->sha256.isEmpty());

    QFile rawArchive(archivePath);
    QVERIFY(rawArchive.open(QIODevice::ReadOnly));
    const auto firstHeader = rawArchive.read(512);
    QCOMPARE(firstHeader.size(), 512);
    QCOMPARE(firstHeader.at(154), '\0');
    QCOMPARE(firstHeader.at(155), ' ');

    const auto index = EncryptedHlsTarContainer::readIndex(archivePath);
    if (!index) QFAIL(qPrintable(index.error()));
    QCOMPARE(index->manifestPath, QStringLiteral("index.m3u8s"));
    QVERIFY(index->entry(QStringLiteral("index.m3u8s")) != nullptr);
    QVERIFY(index->entry(QStringLiteral("segments/000001.ts")) != nullptr);

    const auto manifest = EncryptedHlsTarContainer::readEntry(archivePath, *index, QStringLiteral("index.m3u8s"), 1024);
    if (!manifest) QFAIL(qPrintable(manifest.error()));
    QCOMPARE(*manifest, QByteArrayLiteral("#EXTM3U\n"));
    const auto segment = EncryptedHlsTarContainer::readEntry(archivePath, *index, QStringLiteral("segments/000001.ts"), 1024);
    if (!segment) QFAIL(qPrintable(segment.error()));
    QCOMPARE(*segment, QByteArrayLiteral("encrypted-segment"));

    const auto tarExecutable = QStandardPaths::findExecutable(QStringLiteral("tar"));
    if (!tarExecutable.isEmpty()) {
        QProcess tar;
        // Run against the file name from inside its own directory: the MSYS build of GNU tar that a
        // git-bash shell puts on PATH reads "D:/path/archive.tar" as user D: on host path, fails with
        // exit 128 and turns this portability check into a false local failure. bsdtar (the tar on a
        // clean Windows PATH) and GNU tar on Linux/macOS both accept the relative form.
        const auto archiveInfo = QFileInfo(archivePath);
        tar.setWorkingDirectory(archiveInfo.absolutePath());
        tar.start(tarExecutable, { QStringLiteral("-tf"), archiveInfo.fileName() });
        QVERIFY(tar.waitForFinished(5000));
        QCOMPARE(tar.exitStatus(), QProcess::NormalExit);
        QCOMPARE(tar.exitCode(), 0);
        const auto listing = QString::fromLocal8Bit(tar.readAllStandardOutput());
        QVERIFY(listing.contains(QStringLiteral(".vibe/index.cbor")));
        QVERIFY(listing.contains(QStringLiteral("segments/000001.ts")));
    }
}

void EncryptedHlsTarContainerTest::supportsPaxLongPaths()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("index.m3u8s")), QByteArrayLiteral("#EXTM3U\n")));
    const auto directory = QString(70, QLatin1Char('a')) + QLatin1Char('/') + QString(70, QLatin1Char('b'));
    QVERIFY(QDir().mkpath(temporary.filePath(directory)));
    const auto path = directory + QStringLiteral("/subtitle.vtt");
    QVERIFY(writeFile(temporary.filePath(path), QByteArrayLiteral("WEBVTT\n")));
    const auto archivePath = temporary.filePath(QStringLiteral("long.m3u8sp"));
    const auto built = EncryptedHlsTarContainer::build(temporary.path(), archivePath, QStringLiteral("index.m3u8s"));
    if (!built) QFAIL(qPrintable(built.error()));
    const auto bytes = EncryptedHlsTarContainer::readEntry(archivePath, *built, path, 1024);
    if (!bytes) QFAIL(qPrintable(bytes.error()));
    QCOMPARE(*bytes, QByteArrayLiteral("WEBVTT\n"));
}

void EncryptedHlsTarContainerTest::rejectsUnsafeIndexEntry()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("index.m3u8s")), QByteArrayLiteral("#EXTM3U\n")));
    const auto archivePath = temporary.filePath(QStringLiteral("movie.m3u8sp"));
    const auto built = EncryptedHlsTarContainer::build(temporary.path(), archivePath, QStringLiteral("index.m3u8s"));
    QVERIFY(built.has_value());
    const auto safe = EncryptedHlsTarContainer::readEntry(archivePath, *built, QStringLiteral("../secret.ts"), 1024);
    QVERIFY(!safe.has_value());
}

QTEST_GUILESS_MAIN(EncryptedHlsTarContainerTest)
#include "EncryptedHlsTarContainerTest.moc"
