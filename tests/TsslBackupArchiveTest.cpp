#include "services/backup/TsslBackupArchive.h"
#include "services/webdav/TsslStore.h"

#include <miniz.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <atomic>

namespace {
bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray {};
}

TsslPackage fixture(char digest)
{
    return TsslPackage {
        .identifier = QByteArray(TsslPackage::identifierLength, 'A'),
        .rootManifestDigest = QByteArray(32, digest),
        .segmentKeys = {{QStringLiteral("segments/0001.ts"), QByteArray(32, 'C')}},
    };
}

QString name(const TsslPackage& package)
{
    return QString::fromLatin1(package.rootManifestDigest.toHex()) + QStringLiteral(".tssl");
}

QByteArray manifest(const TsslPackage& package, const QByteArray& data)
{
    return QJsonDocument(QJsonObject {
        {QStringLiteral("format"), QStringLiteral("TSSL-BACKUP")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("count"), 1},
        {QStringLiteral("packages"), QJsonArray {QJsonObject {
            {QStringLiteral("file"), name(package)},
            {QStringLiteral("rootManifestSha256"), QString::fromLatin1(package.rootManifestDigest.toHex())},
            {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("size"), data.size()},
        }}},
    }).toJson(QJsonDocument::Compact);
}

QByteArray archiveBytes(const QList<QPair<QString, QByteArray>>& entries, mz_uint compressionLevel = 0)
{
    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return {};
    for (const auto& entry : entries) {
        const auto encodedName = entry.first.toUtf8();
        if (!mz_zip_writer_add_mem(&zip, encodedName.constData(), entry.second.constData(), entry.second.size(), compressionLevel)) {
            mz_zip_writer_end(&zip);
            return {};
        }
    }
    void* memory = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &memory, &size)) {
        mz_zip_writer_end(&zip);
        return {};
    }
    QByteArray result(static_cast<const char*>(memory), static_cast<qsizetype>(size));
    mz_free(memory);
    mz_zip_writer_end(&zip);
    return result;
}
}

class TsslBackupArchiveTest final : public QObject {
    Q_OBJECT
private slots:
    void roundTripPreservesPackagesAndSupportsUnicodePaths();
    void restoresStandardZipProducedByAnotherImplementation();
    void discoversOldPackagesAndRecognizedArchives();
    void creationRejectsInvalidDuplicateAndExcessiveInputs();
    void rejectsTraversalLinksDuplicateAndUnsupportedEntries();
    void rejectsManifestMismatchAndCorruptArchiveWithoutPublishing();
    void enforcesLimitsBeforeAllocatingOrExtracting();
    void cancellationCleansArchiveAndExtractedFiles();
    void extractionDoesNotOverwriteFiles();
};

void TsslBackupArchiveTest::roundTripPreservesPackagesAndSupportsUnicodePaths()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto source = directory.filePath(QStringLiteral("中文 密钥包"));
    QVERIFY(QDir().mkpath(source));
    QStringList files;
    for (const auto digest : {'B', 'D', 'E'}) {
        const auto package = fixture(digest);
        const auto path = QDir(source).filePath(name(package));
        QVERIFY(writeBytes(path, package.toJson()));
        files.append(path);
    }
    const auto archive = directory.filePath(QStringLiteral("备份 中文.zip"));
    QList<QPair<int, int>> compressionProgress;
    const auto created = TsslBackupArchive::create(files, archive, nullptr,
        [&compressionProgress](int current, int total) { compressionProgress.append({current, total}); });
    QVERIFY2(created.has_value(), created ? "" : qPrintable(created.error()));
    QCOMPARE(*created, 3);
    QCOMPARE(compressionProgress.first(), qMakePair(0, 3));
    QCOMPARE(compressionProgress.last(), qMakePair(3, 3));
    QVERIFY(QFileInfo(archive).size() < readBytes(files.first()).size());
    const auto extracted = TsslBackupArchive::extract(archive, directory.filePath(QStringLiteral("还原 目录")));
    QVERIFY2(extracted.has_value(), extracted ? "" : qPrintable(extracted.error()));
    QCOMPARE(extracted->size(), 3);
    for (qsizetype index = 0; index < files.size(); ++index) {
        QCOMPARE(QFileInfo(extracted->at(index)).fileName(), QFileInfo(files[index]).fileName());
        QCOMPARE(readBytes(extracted->at(index)), readBytes(files[index]));
        QVERIFY(TsslPackage::parse(readBytes(extracted->at(index))).has_value());
    }
    QVERIFY(!TsslBackupArchive::create(files, archive).has_value());
}

void TsslBackupArchiveTest::restoresStandardZipProducedByAnotherImplementation()
{
    // Generated once with .NET System.IO.Compression.ZipArchive, independent
    // of miniz. Keep the fixture embedded so portable tests need no .NET SDK.
    const auto bytes = QByteArray::fromBase64(QByteArrayLiteral(
        "UEsDBBQAAAAIAA6/TV3r8IGsqwAAABsRAABFAAAANDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0Mi50c3Ns7c2xCsIwEAbgd/nnVGuwHQIOouJiBambdAh6bYO2kSQKUvruYhFnwU3uO2747+CuQ2ldowMU9nm+gcCdnDe2hZIC+lJZZ0LdQGG+yiOZpNF6kUHAnKgNpjTkXivGGGOMMcYYY4z9PQg4a0OmW1OSD3mtZZJCYSp/Kwg075se6lAIeKoaaofU4apDDfWZjeM4noyCh8CZHlDYxdvld53N0BcCjry9uSMNz/onUEsDBBQAAAAIAA6/TV2ZhChtpAAAADwBAAANAAAAbWFuaWZlc3QuanNvbq3MsQrCMBRA0X95c5WmL2lNNnVUQahO4vDSJBqsjTTRwdJ/FwW/QLnLnc4ALvRXSqBgV9fryWK+XO23kMHD9tGHDhTLoAn3Ln3uRs2FTjaCOgzgfGtBAS9+a5pibCGDPoS0oc47G1N9pkKUf8Ahg/i1hEaOnEtjRE7EtM5RSk2CGUcit9qiQSaNcIVjkjUN2lnpEBFFKbgsqrflnxYUx0qOx/EFUEsBAhQAFAAAAAgADr9NXevwgayrAAAAGxEAAEUAAAAAAAAAAAAAAAAAAAAAADQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDI0MjQyNDIudHNzbFBLAQIUABQAAAAIAA6/TV2ZhChtpAAAADwBAAANAAAAAAAAAAAAAAAAAA4BAABtYW5pZmVzdC5qc29uUEsFBgAAAAACAAIArgAAAN0BAAAAAA=="));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto archive = directory.filePath(QStringLiteral("standard.zip"));
    QVERIFY(writeBytes(archive, bytes));
    const auto result = TsslBackupArchive::extract(archive, directory.filePath(QStringLiteral("restored")));
    QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error()));
    QCOMPARE(result->size(), 1);
    const auto package = TsslPackage::parse(readBytes(result->first()));
    QVERIFY(package.has_value());
    QCOMPARE(package->rootManifestDigest, QByteArray(32, 'B'));
    QCOMPARE(package->segmentKeys.value(QStringLiteral("segments/0001.ts")), QByteArray(32, 'C'));
}

void TsslBackupArchiveTest::discoversOldPackagesAndRecognizedArchives()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto archiveName = TsslBackupArchive::newFileName();
    QVERIFY(TsslBackupArchive::isArchiveName(archiveName));
    QVERIFY(archiveName != TsslBackupArchive::newFileName());
    QVERIFY(!TsslBackupArchive::isArchiveName(QStringLiteral("../tssl-backup-test.zip")));
    QVERIFY(!TsslBackupArchive::isArchiveName(QStringLiteral("other.zip")));
    const QStringList accepted {QStringLiteral("old.tssl"), archiveName};
    for (const auto& file : accepted) QVERIFY(writeBytes(directory.filePath(file), "fixture"));
    QVERIFY(writeBytes(directory.filePath(QStringLiteral("other.zip")), "fixture"));
    QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("folder.tssl"))));
    const auto discovered = TsslBackupArchive::discover(directory.path());
    QCOMPARE(discovered.size(), 2);
    for (const auto& file : accepted) QVERIFY(discovered.contains(directory.filePath(file)));
}

void TsslBackupArchiveTest::creationRejectsInvalidDuplicateAndExcessiveInputs()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto valid = directory.filePath(QStringLiteral("source.tssl"));
    QVERIFY(writeBytes(valid, fixture('B').toJson()));
    const auto output = directory.filePath(QStringLiteral("backup.zip"));
    QVERIFY(!TsslBackupArchive::create({}, output).has_value());
    QVERIFY(!TsslBackupArchive::create({valid, valid}, output).has_value());
    QVERIFY(!QFileInfo::exists(output));
    const auto invalid = directory.filePath(QStringLiteral("invalid.tssl"));
    QVERIFY(writeBytes(invalid, "invalid"));
    QVERIFY(!TsslBackupArchive::create({valid, invalid}, output).has_value());
    QVERIFY(!QFileInfo::exists(output));
    QStringList tooMany;
    for (int index = 0; index <= TsslBackupArchive::maximumEntries; ++index) tooMany.append(valid);
    QVERIFY(!TsslBackupArchive::create(tooMany, output).has_value());
    QVERIFY(!QFileInfo::exists(output));
}

void TsslBackupArchiveTest::rejectsTraversalLinksDuplicateAndUnsupportedEntries()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto package = fixture('B');
    const auto data = package.toJson();
    const auto metadata = manifest(package, data);
    const auto destination = directory.filePath(QStringLiteral("extract"));
    const auto archive = directory.filePath(QStringLiteral("bad.zip"));
    const QList<QList<QPair<QString, QByteArray>>> badEntries {
        {{QStringLiteral("../escape.tssl"), data}, {QStringLiteral("manifest.json"), metadata}},
        {{name(package), data}, {name(package), data}, {QStringLiteral("manifest.json"), metadata}},
        {{name(package), data}, {QStringLiteral("extra.txt"), "unexpected"}, {QStringLiteral("manifest.json"), metadata}},
        {{QStringLiteral("nested/") + name(package), data}, {QStringLiteral("manifest.json"), metadata}},
    };
    for (const auto& entries : badEntries) {
        QVERIFY(writeBytes(archive, archiveBytes(entries)));
        const auto extracted = TsslBackupArchive::extract(archive, destination);
        QVERIFY(!extracted.has_value());
        QVERIFY(QDir(destination).entryList(QDir::Files).isEmpty());
    }
    auto linked = archiveBytes({{name(package), data}, {QStringLiteral("manifest.json"), metadata}});
    const auto central = linked.indexOf(QByteArrayLiteral("PK\1\2"));
    QVERIFY(central >= 0);
    qToLittleEndian<quint32>(0120777U << 16, reinterpret_cast<uchar*>(linked.data() + central + 38));
    QVERIFY(writeBytes(archive, linked));
    QVERIFY(!TsslBackupArchive::extract(archive, destination).has_value());
    QVERIFY(QDir(destination).entryList(QDir::Files).isEmpty());
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("escape.tssl"))));
}

void TsslBackupArchiveTest::rejectsManifestMismatchAndCorruptArchiveWithoutPublishing()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto package = fixture('B');
    const auto data = package.toJson();
    const auto archive = directory.filePath(QStringLiteral("bad.zip"));
    const auto destination = directory.filePath(QStringLiteral("extract"));
    // Valid ZIP CRC but wrong manifest hash: every package must be checked
    // before the caller can see any restored path or import a partial archive.
    auto metadataObject = QJsonDocument::fromJson(manifest(package, data)).object();
    auto packages = metadataObject.value(QStringLiteral("packages")).toArray();
    auto entry = packages.first().toObject();
    entry.insert(QStringLiteral("sha256"), QString(64, 'a'));
    packages[0] = entry;
    metadataObject.insert(QStringLiteral("packages"), packages);
    QVERIFY(writeBytes(archive, archiveBytes({{name(package), data}, {QStringLiteral("manifest.json"), QJsonDocument(metadataObject).toJson()}})));
    const auto wrongHash = TsslBackupArchive::extract(archive, destination);
    QVERIFY(!wrongHash.has_value());
    QVERIFY(wrongHash.error().contains(QStringLiteral("SHA-256")));
    QVERIFY(QDir(destination).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    auto damaged = archiveBytes({{name(package), data}, {QStringLiteral("manifest.json"), manifest(package, data)}});
    QVERIFY(damaged.size() > 120);
    damaged[110] ^= 0x01;
    QVERIFY(writeBytes(archive, damaged));
    QVERIFY(!TsslBackupArchive::extract(archive, destination).has_value());
    QVERIFY(QDir(destination).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    QVERIFY(writeBytes(archive, QByteArrayLiteral("not a zip")));
    QVERIFY(!TsslBackupArchive::extract(archive, destination).has_value());
}

void TsslBackupArchiveTest::enforcesLimitsBeforeAllocatingOrExtracting()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto package = fixture('B');
    const auto data = package.toJson();
    // Deflate entries may legitimately have different compressed/expanded
    // sizes; this reaches our size guard rather than miniz's stored-size guard.
    auto oversized = archiveBytes({{name(package), data}, {QStringLiteral("manifest.json"), manifest(package, data)}}, 3);
    const auto central = oversized.indexOf(QByteArrayLiteral("PK\1\2"));
    QVERIFY(central >= 0);
    qToLittleEndian<quint32>(static_cast<quint32>(TsslBackupArchive::maxPackageBytes + 1), reinterpret_cast<uchar*>(oversized.data() + central + 24));
    const auto archive = directory.filePath(QStringLiteral("large.zip"));
    QVERIFY(writeBytes(archive, oversized));
    const auto rejected = TsslBackupArchive::extract(archive, directory.filePath(QStringLiteral("extract")));
    QVERIFY(!rejected.has_value());
    QVERIFY(rejected.error().contains(QStringLiteral("oversized")));
    auto giantDirectory = archiveBytes({{name(package), data}, {QStringLiteral("manifest.json"), manifest(package, data)}});
    const auto end = giantDirectory.lastIndexOf(QByteArrayLiteral("PK\5\6"));
    QVERIFY(end >= 0);
    qToLittleEndian<quint32>(16U * 1024 * 1024, reinterpret_cast<uchar*>(giantDirectory.data() + end + 12));
    QVERIFY(writeBytes(archive, giantDirectory));
    QVERIFY(!TsslBackupArchive::extract(archive, directory.filePath(QStringLiteral("extract"))).has_value());
    const auto source = directory.filePath(QStringLiteral("large.tssl"));
    QFile large(source);
    QVERIFY(large.open(QIODevice::WriteOnly));
    QVERIFY(large.resize(TsslBackupArchive::maxPackageBytes + 1));
    large.close();
    QVERIFY(!TsslBackupArchive::create({source}, directory.filePath(QStringLiteral("output.zip"))).has_value());
}

void TsslBackupArchiveTest::cancellationCleansArchiveAndExtractedFiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QStringList files;
    for (const auto digest : {'B', 'D'}) {
        const auto package = fixture(digest);
        const auto path = directory.filePath(name(package));
        QVERIFY(writeBytes(path, package.toJson()));
        files.append(path);
    }
    const auto archive = directory.filePath(QStringLiteral("cancel.zip"));
    std::atomic_bool canceled {false};
    auto cancelAfterFirst = [&canceled](int current, int) { if (current == 1) canceled.store(true); };
    const auto creation = TsslBackupArchive::create(files, archive, &canceled, cancelAfterFirst);
    QVERIFY(!creation.has_value());
    QVERIFY(creation.error().contains(QStringLiteral("canceled")));
    QVERIFY(!QFileInfo::exists(archive));
    canceled.store(false);
    QVERIFY(TsslBackupArchive::create(files, archive).has_value());
    const auto destination = directory.filePath(QStringLiteral("restored"));
    const auto extraction = TsslBackupArchive::extract(archive, destination, &canceled, cancelAfterFirst);
    QVERIFY(!extraction.has_value());
    QVERIFY(extraction.error().contains(QStringLiteral("canceled")));
    QVERIFY(QDir(destination).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    canceled.store(false);
    QVERIFY(TsslBackupArchive::extract(archive, destination).has_value());
}

void TsslBackupArchiveTest::extractionDoesNotOverwriteFiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto first = fixture('B');
    const auto second = fixture('D');
    const auto firstPath = directory.filePath(name(first));
    const auto secondPath = directory.filePath(name(second));
    QVERIFY(writeBytes(firstPath, first.toJson()));
    QVERIFY(writeBytes(secondPath, second.toJson()));
    const auto archive = directory.filePath(QStringLiteral("backup.zip"));
    QVERIFY(TsslBackupArchive::create({firstPath, secondPath}, archive).has_value());
    const auto destination = directory.filePath(QStringLiteral("existing"));
    QVERIFY(QDir().mkpath(destination));
    const auto existing = QDir(destination).filePath(name(second));
    QVERIFY(writeBytes(existing, "keep me"));
    QVERIFY(!TsslBackupArchive::extract(archive, destination).has_value());
    QCOMPARE(readBytes(existing), QByteArrayLiteral("keep me"));
    QVERIFY(!QFileInfo::exists(QDir(destination).filePath(name(first))));
}

QTEST_GUILESS_MAIN(TsslBackupArchiveTest)
#include "TsslBackupArchiveTest.moc"
