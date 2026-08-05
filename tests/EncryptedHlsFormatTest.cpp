#include "services/webdav/AesGcmDecryptor.h"
#include "services/webdav/HlsManifestValidator.h"
#include "services/webdav/TsslStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace {
QByteArray identifierBytes(char value = 'A')
{
    return QByteArray(TsslPackage::identifierLength, value);
}

QByteArray manifestBytes()
{
    const QByteArray manifest = "#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:4.0,\nsegments/0001.ts\n#EXT-X-ENDLIST\n";
    return *HlsManifestValidator::insertM3u8sIdentifier(manifest, identifierBytes());
}

TsslPackage packageFor(const QByteArray& manifest)
{
    return TsslPackage {
        .identifier = identifierBytes(),
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
    void sourceFilenameMetadataIsAuthenticatedAndRoundTrips();
    void m3u8sIdentifierIsStrictAndRoundTrips();
    void manifestValidatorAcceptsPackageRelativeUris();
    void manifestValidatorRejectsExternalUrisAndKeyTags();
    void aesGcmRequiresAValidAuthenticationTag();
    void aesGcmEncryptionMatchesTheAuthenticatedLayout();
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
    QCOMPARE(parsed->identifier, original.identifier);
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

    const auto listed = store.listPackages();
    if (!listed) {
        QFAIL(qPrintable(listed.error()));
    }
    QCOMPARE(listed->size(), size_t(1));
    QCOMPARE(listed->front().identifier, package.identifier);
    QCOMPARE(listed->front().segmentCount, 1);

    QVERIFY(store.deleteByRootDigest(package.rootManifestDigest).has_value());
    const auto afterDelete = store.listPackages();
    QVERIFY(afterDelete.has_value());
    QVERIFY(afterDelete->empty());

    const QByteArray invalidDigest(32, '\x44');
    const auto invalidPath = QDir(store.storageDirectory())
                                 .filePath(QString::fromLatin1(invalidDigest.toHex()) + QStringLiteral(".tssl"));
    QVERIFY(!writeFile(invalidPath, QByteArrayLiteral("{}")).isEmpty());
    const auto withInvalid = store.listPackages();
    QVERIFY(withInvalid.has_value());
    QCOMPARE(withInvalid->size(), size_t(1));
    QVERIFY(!withInvalid->front().valid);
    QVERIFY(!withInvalid->front().validationError.isEmpty());
    QVERIFY(store.deleteByRootDigest(invalidDigest).has_value());
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

    object = QJsonDocument::fromJson(packageFor(manifestBytes()).toJson()).object();
    object.insert(QStringLiteral("identifier"), QStringLiteral("too-short"));
    QVERIFY(!TsslPackage::parse(QJsonDocument(object).toJson()).has_value());
}

void EncryptedHlsFormatTest::sourceFilenameMetadataIsAuthenticatedAndRoundTrips()
{
    const auto identifier = identifierBytes('N');
    const auto sourceFileName = QStringLiteral("Private Video.final.mkv");
    const auto key = QByteArray::fromHex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const auto iv = QByteArray::fromHex("101112131415161718191a1b1c1d1e1f");
    const auto aad = TsslPackage::sourceFileNameAuthenticatedData(identifier);
    const auto encrypted = AesGcmDecryptor::encryptAuthenticatedData(
        sourceFileName.toUtf8(), key, iv, aad);
    if (!encrypted) {
        QFAIL(qPrintable(encrypted.error()));
    }

    const QByteArray plainManifest =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:4.0,\nsegments/0001.ts\n#EXT-X-ENDLIST\n";
    auto manifest = HlsManifestValidator::insertM3u8sIdentifier(plainManifest, identifier);
    QVERIFY(manifest.has_value());
    manifest = HlsManifestValidator::insertEncryptedSourceFileName(*manifest, *encrypted);
    QVERIFY(manifest.has_value());
    QVERIFY(!manifest->contains(sourceFileName.toUtf8()));
    QCOMPARE(HlsManifestValidator::extractEncryptedSourceFileName(*manifest), encrypted);

    TsslPackage package {
        .version = 3,
        .identifier = identifier,
        .rootManifestDigest = QCryptographicHash::hash(*manifest, QCryptographicHash::Sha256),
        .encryptedSourceFileName = *encrypted,
        .sourceFileNameKey = key,
        .segmentKeys = {
            { QStringLiteral("segments/0001.ts"), QByteArray(32, '\x11') },
        },
    };
    const auto parsed = TsslPackage::parse(package.toJson());
    if (!parsed) {
        QFAIL(qPrintable(parsed.error()));
    }
    QCOMPARE(parsed->version, 3);
    QCOMPARE(parsed->encryptedSourceFileName, *encrypted);
    const auto recovered = parsed->decryptedSourceFileName();
    QVERIFY(recovered.has_value());
    QVERIFY(recovered->has_value());
    QCOMPARE(**recovered, sourceFileName);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto recoveryPath = writeFile(temporary.filePath(QStringLiteral("v3-recovery.tssl")), package.toJson());
    QVERIFY(!recoveryPath.isEmpty());
    TsslStore store(temporary.filePath(QStringLiteral("store")));
    QVERIFY(store.restoreFromFile(recoveryPath).has_value());
    const auto exportPath = temporary.filePath(QStringLiteral("v3-export.tssl"));
    QVERIFY(store.exportByRootDigest(package.rootManifestDigest, exportPath).has_value());
    QFile exportedFile(exportPath);
    QVERIFY(exportedFile.open(QIODevice::ReadOnly));
    const auto exported = TsslPackage::parse(exportedFile.readAll());
    QVERIFY(exported.has_value());
    QCOMPARE(exported->version, 3);
    QCOMPARE(exported->encryptedSourceFileName, package.encryptedSourceFileName);
    QCOMPARE(exported->sourceFileNameKey, package.sourceFileNameKey);
    const auto exportedSourceName = exported->decryptedSourceFileName();
    QVERIFY(exportedSourceName.has_value());
    QCOMPARE(exportedSourceName->value_or(QString()), sourceFileName);

    package.encryptedSourceFileName.back() ^= 0x01;
    QVERIFY(!TsslPackage::parse(package.toJson()).has_value());

    auto duplicate = *manifest;
    const auto sourceLineStart = duplicate.indexOf("#M3U8S-SOURCE-NAME:");
    const auto sourceLineEnd = duplicate.indexOf('\n', sourceLineStart);
    duplicate.append(duplicate.sliced(sourceLineStart, sourceLineEnd - sourceLineStart + 1));
    QVERIFY(!HlsManifestValidator::validate(duplicate).has_value());
}

void EncryptedHlsFormatTest::m3u8sIdentifierIsStrictAndRoundTrips()
{
    const QByteArray plain = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-ENDLIST\n";
    const auto inserted = HlsManifestValidator::insertM3u8sIdentifier(plain, identifierBytes('Z'));
    QVERIFY(inserted.has_value());
    const auto extracted = HlsManifestValidator::extractM3u8sIdentifier(*inserted);
    QVERIFY(extracted.has_value());
    QCOMPARE(*extracted, identifierBytes('Z'));
    QVERIFY(HlsManifestValidator::validate(*inserted).has_value());
    QVERIFY(!HlsManifestValidator::extractM3u8sIdentifier(plain).has_value());
    QVERIFY(!HlsManifestValidator::insertM3u8sIdentifier(plain, QByteArray(4095, 'A')).has_value());

    const auto duplicate = *inserted + QByteArrayLiteral("#M3U8S-IDENTIFIER:") + identifierBytes('Y') + '\n';
    QVERIFY(!HlsManifestValidator::validate(duplicate).has_value());
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

void EncryptedHlsFormatTest::aesGcmEncryptionMatchesTheAuthenticatedLayout()
{
    const auto key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const auto iv = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const auto plaintext = QByteArrayLiteral("Encrypted TS payload for TSSL");
    const auto expected = iv +
        QByteArray::fromHex("2202c30440943e4df9df8f7a75d44dca38dc0ac547ebbc31646a1e86c2") +
        QByteArray::fromHex("4607c2b2d88008a628da3a3f92378a13");

    const auto encrypted = AesGcmDecryptor::encryptTsSegment(plaintext, key, iv);
    if (!encrypted) {
        QFAIL(qPrintable(encrypted.error()));
    }
    QCOMPARE(*encrypted, expected);
    const auto decrypted = AesGcmDecryptor::decryptTsSegment(*encrypted, key);
    QVERIFY(decrypted.has_value());
    QCOMPARE(*decrypted, plaintext);

    const auto randomized = AesGcmDecryptor::encryptTsSegment(plaintext);
    QVERIFY(randomized.has_value());
    QCOMPARE(randomized->key.size(), 32);
    QCOMPARE(randomized->bytes.size(), plaintext.size() + 32);
    const auto randomizedDecrypted = AesGcmDecryptor::decryptTsSegment(randomized->bytes, randomized->key);
    QVERIFY(randomizedDecrypted.has_value());
    QCOMPARE(*randomizedDecrypted, plaintext);
}

QTEST_GUILESS_MAIN(EncryptedHlsFormatTest)

#include "EncryptedHlsFormatTest.moc"
