#include "services/webdav/AesGcmDecryptor.h"
#include "services/webdav/HlsManifestValidator.h"
#include "services/webdav/TsslStore.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace {
QByteArray manifestBytes()
{
    return QByteArrayLiteral("#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:4.0,\nsegments/0001.ts\n#EXT-X-ENDLIST\n");
}

TsslPackage packageFor(const QByteArray& manifest)
{
    return TsslPackage {
        .rootManifestDigest = QCryptographicHash::hash(manifest, QCryptographicHash::Sha256),
        .manifestDigests = {
            { QStringLiteral("variants/720p.m3u8"), QByteArray(32, '\x22') },
        },
        .segmentKeys = {
            { QStringLiteral("segments/0001.ts"), QByteArray(32, '\x11') },
        },
        .resourceDigests = {
            { QStringLiteral("subtitles/zh.vtt"), QByteArray(32, '\x33') },
        },
    };
}

QString writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    file.close();
    return path;
}
}

class EncryptedHlsFormatTest final : public QObject {
    Q_OBJECT

private slots:
    void tsslRoundTripsDeterministically();
    void tsslRestoreLookupAndExport();
    void tsslRejectsUnsafePathsAndInvalidKeys();
    void manifestValidatorAcceptsPackageRelativeUris();
    void manifestValidatorRejectsExternalUrisAndKeyTags();
    void aesGcmRequiresAValidAuthenticationTag();
};

void EncryptedHlsFormatTest::tsslRoundTripsDeterministically()
{
    const auto original = packageFor(manifestBytes());
    const auto encoded = original.toJson();
    const auto parsed = TsslPackage::parse(encoded);

    if (!parsed) {
        QFAIL(qPrintable(parsed.error()));
    }
    QCOMPARE(parsed->rootManifestDigest, original.rootManifestDigest);
    QCOMPARE(parsed->manifestDigests, original.manifestDigests);
    QCOMPARE(parsed->segmentKeys, original.segmentKeys);
    QCOMPARE(parsed->resourceDigests, original.resourceDigests);
    QCOMPARE(parsed->toJson(), encoded);
}

void EncryptedHlsFormatTest::tsslRestoreLookupAndExport()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto package = packageFor(manifestBytes());
    const auto sourcePath = writeFile(temporary.filePath(QStringLiteral("source.tssl")), package.toJson());
    QVERIFY(!sourcePath.isEmpty());

    TsslStore store(temporary.filePath(QStringLiteral("store")));
    const auto restored = store.restoreFromFile(sourcePath);
    if (!restored) {
        QFAIL(qPrintable(restored.error()));
    }
    QCOMPARE(*restored, package.rootManifestDigest);

    const auto loaded = store.packageForRootDigest(package.rootManifestDigest);
    if (!loaded) {
        QFAIL(qPrintable(loaded.error()));
    }
    QVERIFY(loaded->has_value());
    QCOMPARE((**loaded).segmentKeys, package.segmentKeys);

    const auto exportPath = temporary.filePath(QStringLiteral("backup.tssl"));
    const auto exported = store.exportByRootDigest(package.rootManifestDigest, exportPath);
    if (!exported) {
        QFAIL(qPrintable(exported.error()));
    }
    QFile exportedFile(exportPath);
    QVERIFY(exportedFile.open(QIODevice::ReadOnly));
    const auto exportedPackage = TsslPackage::parse(exportedFile.readAll());
    if (!exportedPackage) {
        QFAIL(qPrintable(exportedPackage.error()));
    }
    QCOMPARE(exportedPackage->rootManifestDigest, package.rootManifestDigest);
}

void EncryptedHlsFormatTest::tsslRejectsUnsafePathsAndInvalidKeys()
{
    auto object = QJsonDocument::fromJson(packageFor(manifestBytes()).toJson()).object();
    auto segments = object.value(QStringLiteral("segments")).toArray();
    auto segment = segments.at(0).toObject();
    segment.insert(QStringLiteral("path"), QStringLiteral("../outside.ts"));
    segments.replace(0, segment);
    object.insert(QStringLiteral("segments"), segments);
    QVERIFY(!TsslPackage::parse(QJsonDocument(object).toJson()).has_value());

    object = QJsonDocument::fromJson(packageFor(manifestBytes()).toJson()).object();
    segments = object.value(QStringLiteral("segments")).toArray();
    segment = segments.at(0).toObject();
    segment.insert(QStringLiteral("key"), QStringLiteral("not-a-key"));
    segments.replace(0, segment);
    object.insert(QStringLiteral("segments"), segments);
    QVERIFY(!TsslPackage::parse(QJsonDocument(object).toJson()).has_value());
}

void EncryptedHlsFormatTest::manifestValidatorAcceptsPackageRelativeUris()
{
    const QByteArray root = "#EXTM3U\n#EXT-X-MEDIA:TYPE=SUBTITLES,URI=\"subtitles/zh.m3u8\"\nvariants/720p.m3u8\n";
    const QByteArray child = "#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n../segments/0001.ts?token=one\n";
    QVERIFY(HlsManifestValidator::validate(root).has_value());
    QVERIFY(HlsManifestValidator::validate(child, QStringLiteral("variants/720p.m3u8")).has_value());
}

void EncryptedHlsFormatTest::manifestValidatorRejectsExternalUrisAndKeyTags()
{
    const QByteArray external = "#EXTM3U\nhttps://example.com/segment.ts\n";
    const QByteArray escaping = "#EXTM3U\n../segment.ts\n";
    const QByteArray keyTag = "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\nsegment.ts\n";
    QVERIFY(!HlsManifestValidator::validate(external).has_value());
    QVERIFY(!HlsManifestValidator::validate(escaping).has_value());
    QVERIFY(!HlsManifestValidator::validate(keyTag).has_value());
}

void EncryptedHlsFormatTest::aesGcmRequiresAValidAuthenticationTag()
{
    const auto key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const auto iv = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const auto ciphertext = QByteArray::fromHex("2202c30440943e4df9df8f7a75d44dca38dc0ac547ebbc31646a1e86c2");
    const auto tag = QByteArray::fromHex("4607c2b2d88008a628da3a3f92378a13");
    const auto encrypted = iv + ciphertext + tag;

    const auto decrypted = AesGcmDecryptor::decryptTsSegment(encrypted, key);
    if (!decrypted) {
        QFAIL(qPrintable(decrypted.error()));
    }
    QCOMPARE(*decrypted, QByteArrayLiteral("Encrypted TS payload for TSSL"));

    auto tampered = encrypted;
    tampered.back() ^= 0x01;
    QVERIFY(!AesGcmDecryptor::decryptTsSegment(tampered, key).has_value());
    QVERIFY(!AesGcmDecryptor::decryptTsSegment(encrypted.chopped(1), key).has_value());
}

QTEST_GUILESS_MAIN(EncryptedHlsFormatTest)

#include "EncryptedHlsFormatTest.moc"
