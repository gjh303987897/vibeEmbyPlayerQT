#include "services/encryptedhls/EncryptedHlsRestorer.h"
#include "services/encryptedhls/EncryptedHlsPackager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

#include <optional>
#include <algorithm>

namespace {
std::expected<QByteArray, QString> ffmpeg(const QStringList& arguments)
{
    QProcess process;
    process.start(EncryptedHlsPackager::locateFfmpegExecutable(), arguments);
    if (!process.waitForStarted(3000) || !process.waitForFinished(15000))
        return std::unexpected(QStringLiteral("Fixture FFmpeg timed out"));
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return std::unexpected(QString::fromUtf8(process.readAllStandardError()));
    return process.readAllStandardOutput();
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray {};
}

struct Row { QString state; QString output; QString error; bool legacyName { false }; };

class Run final {
public:
    explicit Run(EncryptedHlsRestorer& restorer)
        : finished(&restorer, &EncryptedHlsRestorer::finished)
    {
        connection = QObject::connect(&restorer, &EncryptedHlsRestorer::itemChanged, &restorer,
            [this](int index, const QString& state, const QString& output, const QString& error, bool legacyName) {
                rows[index] = {state, output, error, legacyName};
            });
    }
    ~Run() { QObject::disconnect(connection); }
    bool wait() { return !finished.isEmpty() || finished.wait(30000); }
    QSignalSpy finished;
    QHash<int, Row> rows;
private:
    QMetaObject::Connection connection;
};
}

class EncryptedHlsRestorerTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void restoresBothFormatsWithoutEncodingOrOverwriting();
    void supportsExplicitContainers();
    void continuesAfterAnInvalidInput();
    void rejectsMissingKeysAndTamperedManifest();
    void rejectsMissingAndTamperedSegments_data();
    void rejectsMissingAndTamperedSegments();
    void cancellationCleansPartialFilesAndCanRestart();
    void cancellationDuringPreparationDiscardsStaleSession();
    void cancellationBetweenItemsPreservesCompletedOutput();
    void restoresLegacyNameAndSanitizesPortableNames();
    void restoresOriginalMovAndRejectsUnsupportedContainer();
private:
    QString output(const QString& name);
    QString directoryPackage(const QString& name, const QString& sourceName);
    QTemporaryDir m_directory;
    std::unique_ptr<TsslStore> m_store;
    QString m_source;
    QStringList m_packages;
    QByteArray m_videoHash;
};

QString EncryptedHlsRestorerTest::output(const QString& name)
{
    const auto path = m_directory.filePath(name);
    QDir().mkpath(path);
    return path;
}

void EncryptedHlsRestorerTest::initTestCase()
{
    QVERIFY(m_directory.isValid());
    if (EncryptedHlsPackager::locateFfmpegExecutable().isEmpty())
        QSKIP("FFmpeg is not installed; restoration integration tests require the optional external executable");
    m_store = std::make_unique<TsslStore>(m_directory.filePath(QStringLiteral("keys")));
    m_source = m_directory.filePath(QStringLiteral("原视频.mp4"));
    const auto generated = ffmpeg({"-hide_banner", "-nostdin", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=12",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "4", "-c:v", "libx264",
        "-preset", "ultrafast", "-g", "12", "-c:a", "aac", m_source});
    QVERIFY2(generated.has_value(), qPrintable(generated ? QString() : generated.error()));
    const auto hash = ffmpeg({"-v", "error", "-i", m_source, "-map", "0:v:0", "-f", "hash", "-hash", "sha256", "-"});
    QVERIFY(hash.has_value());
    m_videoHash = *hash;
    for (const auto format : {EncryptedHlsContainerFormat::DirectoryM3u8s, EncryptedHlsContainerFormat::TarM3u8sp}) {
        EncryptedHlsPackager packager(*m_store);
        std::optional<EncryptedHlsPackageResult> result;
        QString error;
        connect(&packager, &EncryptedHlsPackager::completed, this, [&result](const auto& value) { result = value; });
        connect(&packager, &EncryptedHlsPackager::failed, this, [&error](const auto& value) { error = value; });
        EncryptedHlsPackageRequest request;
        request.sourcePath = m_source;
        request.outputDirectory = output(format == EncryptedHlsContainerFormat::TarM3u8sp ? "archive" : "directory");
        request.videoEncoding = EncryptedHlsVideoEncoding::Copy;
        request.audioEncoding = EncryptedHlsAudioEncoding::Copy;
        request.segmentDurationSeconds = 2;
        request.containerFormat = format;
        QVERIFY(packager.start(request).has_value());
        QTRY_VERIFY_WITH_TIMEOUT(!packager.isRunning(), 30000);
        QVERIFY2(result.has_value(), qPrintable(error));
        m_packages.append(result->manifestPath);
    }
}

void EncryptedHlsRestorerTest::restoresBothFormatsWithoutEncodingOrOverwriting()
{
    const auto target = output("roundtrip");
    const auto existing = QDir(target).filePath(QFileInfo(m_source).fileName());
    QVERIFY(writeFile(existing, "existing-video"));
    EncryptedHlsRestorer restorer(*m_store);
    Run run(restorer);
    QVERIFY(restorer.start({m_packages, target}).has_value());
    QVERIFY(run.wait());
    QCOMPARE(restorer.progress(), 1.0);
    QCOMPARE(run.rows.size(), 2);
    QCOMPARE(readFile(existing), QByteArray("existing-video"));
    for (int i = 0; i < 2; ++i) {
        const auto row = run.rows.value(i);
        QCOMPARE(row.state, QStringLiteral("completed"));
        QCOMPARE(QFileInfo(row.output).fileName(), QStringLiteral("原视频 (%1).mp4").arg(i + 1));
        const auto hash = ffmpeg({"-v", "error", "-i", row.output, "-map", "0:v:0", "-f", "hash", "-hash", "sha256", "-"});
        QVERIFY2(hash.has_value(), qPrintable(hash ? QString() : hash.error()));
        QCOMPARE(*hash, m_videoHash); // every decoded frame, not just the file extension
        const auto audio = ffmpeg({"-v", "error", "-i", row.output, "-map", "0:a:0", "-f", "hash", "-hash", "sha256", "-"});
        QVERIFY(audio.has_value());
        QVERIFY(!audio->isEmpty());
        QVERIFY(QFileInfo::exists(m_packages.at(i)));
    }
    QVERIFY(QDir(target).entryList({".vibe-restore-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void EncryptedHlsRestorerTest::supportsExplicitContainers()
{
    for (const auto& format : {QStringLiteral("mkv"), QStringLiteral("mp4")}) {
        EncryptedHlsRestorer restorer(*m_store);
        Run run(restorer);
        QVERIFY(restorer.start({{m_packages.last()}, output("explicit-" + format), format}).has_value());
        QVERIFY(run.wait());
        QCOMPARE(run.rows[0].state, QStringLiteral("completed"));
        QCOMPARE(QFileInfo(run.rows[0].output).suffix(), format);
        const auto hash = ffmpeg({"-v", "error", "-i", run.rows[0].output, "-map", "0:v:0", "-f", "hash", "-hash", "sha256", "-"});
        QVERIFY(hash.has_value());
        QCOMPARE(*hash, m_videoHash);
    }
}

void EncryptedHlsRestorerTest::continuesAfterAnInvalidInput()
{
    EncryptedHlsRestorer restorer(*m_store);
    Run run(restorer);
    QVERIFY(!restorer.start({{}, output("invalid")}).has_value());
    QVERIFY(!restorer.start({m_packages, m_source}).has_value());
    QVERIFY(!restorer.start({m_packages, output("invalid"), "exe"}).has_value());
    QVERIFY(restorer.start({{"missing.m3u8sp", m_packages.last()}, output("mixed")}).has_value());
    QVERIFY(!restorer.start({m_packages, output("invalid")}).has_value());
    QVERIFY(run.wait());
    QCOMPARE(run.rows[0].state, QStringLiteral("failed"));
    QCOMPARE(run.rows[1].state, QStringLiteral("completed"));
}

void EncryptedHlsRestorerTest::rejectsMissingKeysAndTamperedManifest()
{
    TsslStore missingKeys(m_directory.filePath("missing-keys"));
    EncryptedHlsRestorer restorer(missingKeys);
    Run run(restorer);
    QVERIFY(restorer.start({m_packages, output("missing-keys-output")}).has_value());
    QVERIFY(run.wait());
    QCOMPARE(run.rows[0].state, QStringLiteral("failed"));
    QCOMPARE(run.rows[1].state, QStringLiteral("failed"));
    QVERIFY(run.rows[0].output.isEmpty());
    const auto changed = m_directory.filePath("changed.m3u8s");
    QVERIFY(writeFile(changed, readFile(m_packages.first()) + "\n#tampered\n"));
    EncryptedHlsRestorer tampered(*m_store);
    Run changedRun(tampered);
    QVERIFY(tampered.start({{changed}, output("tampered-manifest")}).has_value());
    QVERIFY(changedRun.wait());
    QCOMPARE(changedRun.rows[0].state, QStringLiteral("failed"));
}

void EncryptedHlsRestorerTest::rejectsMissingAndTamperedSegments_data()
{
    QTest::addColumn<bool>("archive");
    QTest::addColumn<bool>("missing");
    QTest::newRow("directory-gcm-tamper") << false << false;
    QTest::newRow("directory-missing-segment") << false << true;
    QTest::newRow("archive-segment-tamper") << true << false;
}

void EncryptedHlsRestorerTest::rejectsMissingAndTamperedSegments()
{
    QFETCH(bool, archive);
    QFETCH(bool, missing);
    const auto corrupted = output(QStringLiteral("corrupt-%1-%2").arg(archive).arg(missing));
    QString input;
    if (archive) {
        input = QDir(corrupted).filePath("corrupt.m3u8sp");
        QVERIFY(QFile::copy(m_packages.last(), input));
        auto index = EncryptedHlsTarContainer::readIndex(input);
        QVERIFY(index.has_value());
        const auto entry = std::ranges::find_if(index->entries, [](const auto& entry) { return entry.path.endsWith(".ts"); });
        QVERIFY(entry != index->entries.end());
        QFile file(input);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.seek(entry->dataOffset + entry->size - 1));
        auto byte = file.read(1); byte[0] ^= 1;
        QVERIFY(file.seek(entry->dataOffset + entry->size - 1));
        QCOMPARE(file.write(byte), 1);
        file.close();
    } else {
        const QDir original(QFileInfo(m_packages.first()).absolutePath());
        for (const auto& name : original.entryList(QDir::Files))
            QVERIFY(QFile::copy(original.filePath(name), QDir(corrupted).filePath(name)));
        input = QDir(corrupted).filePath(QFileInfo(m_packages.first()).fileName());
        // Corrupt the last segment so output has already started when this fails.
        const auto segments = QDir(corrupted).entryList({"*.ts"}, QDir::Files, QDir::Name);
        QVERIFY(segments.size() >= 2);
        const auto path = QDir(corrupted).filePath(segments.last());
        if (missing) QVERIFY(QFile::remove(path));
        else { auto bytes = readFile(path); bytes.back() ^= 1; QVERIFY(writeFile(path, bytes)); }
    }
    const auto target = output(QStringLiteral("rejected-%1-%2").arg(archive).arg(missing));
    EncryptedHlsRestorer restorer(*m_store);
    Run run(restorer);
    QVERIFY(restorer.start({{input}, target}).has_value());
    QVERIFY(run.wait());
    QCOMPARE(run.rows[0].state, QStringLiteral("failed"));
    QVERIFY(!run.rows[0].error.isEmpty());
    QVERIFY(QDir(target).entryList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void EncryptedHlsRestorerTest::cancellationCleansPartialFilesAndCanRestart()
{
    EncryptedHlsRestorer restorer(*m_store);
    Run run(restorer);
    const auto connection = connect(&restorer, &EncryptedHlsRestorer::itemChanged, &restorer,
        [&restorer](int, const QString& state, const QString&, const QString&, bool) {
            if (state == "restoring") restorer.cancel();
        });
    const auto target = output("canceled-output");
    QVERIFY(restorer.start({m_packages, target}).has_value());
    QVERIFY(run.wait());
    QCOMPARE(run.rows[0].state, QStringLiteral("canceled"));
    QCOMPARE(run.rows[1].state, QStringLiteral("canceled"));
    QVERIFY(QDir(target).entryList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    disconnect(connection);
    Run restarted(restorer);
    QVERIFY(restorer.start({{m_packages.last()}, target}).has_value());
    QVERIFY(restarted.wait());
    QCOMPARE(restarted.rows[0].state, QStringLiteral("completed"));
}

void EncryptedHlsRestorerTest::cancellationDuringPreparationDiscardsStaleSession()
{
    EncryptedHlsRestorer restorer(*m_store);
    Run run(restorer);
    QVERIFY(restorer.start({m_packages, output("cancel-preparation")}).has_value());
    restorer.cancel(); // before the queued first item; no decoder process should start
    QCOMPARE(restorer.phase(), QStringLiteral("canceled"));
    QCOMPARE(run.rows[0].state, QStringLiteral("canceled"));
    Run preparing(restorer);
    const auto connection = connect(&restorer, &EncryptedHlsRestorer::itemChanged, &restorer,
        [&restorer](int, const QString& state, const QString&, const QString&, bool) {
            if (state == "preparing") QTimer::singleShot(0, &restorer, &EncryptedHlsRestorer::cancel);
        });
    QVERIFY(restorer.start({m_packages, output("cancel-inflight-preparation")}).has_value());
    QVERIFY(preparing.wait());
    QCOMPARE(preparing.rows[0].state, QStringLiteral("canceled"));
    disconnect(connection);
    Run restarted(restorer);
    QVERIFY(restorer.start({{m_packages.last()}, output("restart-preparation")}).has_value());
    QVERIFY(restarted.wait());
    QCOMPARE(restarted.rows[0].state, QStringLiteral("completed"));
}

void EncryptedHlsRestorerTest::cancellationBetweenItemsPreservesCompletedOutput()
{
    EncryptedHlsRestorer restorer(*m_store);
    Run run(restorer);
    connect(&restorer, &EncryptedHlsRestorer::itemChanged, &restorer,
        [&restorer](int, const QString& state, const QString&, const QString&, bool) {
            if (state == "completed") restorer.cancel();
        });
    QVERIFY(restorer.start({m_packages, output("cancel-between")}).has_value());
    QVERIFY(run.wait());
    QCOMPARE(run.rows[0].state, QStringLiteral("completed"));
    QVERIFY(QFileInfo::exists(run.rows[0].output));
    QCOMPARE(run.rows[1].state, QStringLiteral("canceled"));
}

QString EncryptedHlsRestorerTest::directoryPackage(const QString& name, const QString& sourceName)
{
    const auto directory = output(name);
    const auto manifest = QDir(directory).filePath("index.m3u8");
    const auto generated = ffmpeg({"-v", "error", "-i", m_source, "-c", "copy", "-f", "hls", "-hls_time", "2",
        "-hls_list_size", "0", "-hls_segment_filename", QDir(directory).filePath("segment_%06d.ts"), manifest});
    if (!generated) return {};
    std::atomic_bool canceled {false};
    auto encrypted = EncryptedHlsPackaging::encryptHlsDirectory(directory, sourceName, canceled);
    if (!encrypted || !m_store->savePackage(encrypted->tsslPackage)) return {};
    return QDir(directory).filePath(encrypted->manifestFileName);
}

void EncryptedHlsRestorerTest::restoresLegacyNameAndSanitizesPortableNames()
{
    const auto named = directoryPackage("portable-name", "CON.mp4");
    QVERIFY(!named.isEmpty());
    const auto legacy = directoryPackage("legacy-name", "old.mp4");
    QVERIFY(!legacy.isEmpty());
    auto manifest = readFile(legacy);
    auto package = m_store->packageForRootDigest(QCryptographicHash::hash(manifest, QCryptographicHash::Sha256));
    QVERIFY(package && package->has_value());
    auto key = **package;
    manifest = QString::fromUtf8(manifest).replace(QRegularExpression("#M3U8S-SOURCE-NAME:[^\\n]*\\n"), "").toUtf8();
    QVERIFY(writeFile(legacy, manifest));
    key.version = 2;
    key.sourceFileNameKey.clear();
    key.encryptedSourceFileName.clear();
    key.rootManifestDigest = QCryptographicHash::hash(manifest, QCryptographicHash::Sha256);
    QVERIFY(m_store->savePackage(key).has_value());
    EncryptedHlsRestorer restorer(*m_store);
    Run run(restorer);
    QVERIFY(restorer.start({{named, legacy}, output("legacy-restored")}).has_value());
    QVERIFY(run.wait());
    QCOMPARE(run.rows[0].state, QStringLiteral("completed"));
    QCOMPARE(QFileInfo(run.rows[0].output).fileName(), QStringLiteral("_CON.mp4"));
    QCOMPARE(run.rows[1].state, QStringLiteral("completed"));
    QCOMPARE(QFileInfo(run.rows[1].output).suffix(), QStringLiteral("mkv"));
    QVERIFY(run.rows[1].legacyName);
}

void EncryptedHlsRestorerTest::restoresOriginalMovAndRejectsUnsupportedContainer()
{
    const auto mov = directoryPackage("original-mov", "original.MOV");
    const auto unsupported = directoryPackage("unsupported-original", "original.y4m");
    QVERIFY(!mov.isEmpty());
    QVERIFY(!unsupported.isEmpty());
    EncryptedHlsRestorer restorer(*m_store);
    Run run(restorer);
    QVERIFY(restorer.start({{mov, unsupported}, output("original-containers")}).has_value());
    QVERIFY(run.wait());
    QCOMPARE(run.rows[0].state, QStringLiteral("completed"));
    QCOMPARE(QFileInfo(run.rows[0].output).fileName(), QStringLiteral("original.MOV"));
    const auto hash = ffmpeg({"-v", "error", "-i", run.rows[0].output, "-map", "0:v:0", "-f", "hash", "-hash", "sha256", "-"});
    QVERIFY(hash.has_value());
    QCOMPARE(*hash, m_videoHash);
    QCOMPARE(run.rows[1].state, QStringLiteral("failed"));
    QVERIFY(run.rows[1].error.contains("MKV"));
    Run retry(restorer);
    QVERIFY(restorer.start({{unsupported}, output("original-containers"), "mkv"}).has_value());
    QVERIFY(retry.wait());
    QCOMPARE(retry.rows[0].state, QStringLiteral("completed"));
}

QTEST_GUILESS_MAIN(EncryptedHlsRestorerTest)
#include "EncryptedHlsRestorerTest.moc"
