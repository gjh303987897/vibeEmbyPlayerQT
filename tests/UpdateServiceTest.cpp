#include "services/update/UpdateService.h"

#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>

namespace {
QString nativePackageName()
{
#if defined(Q_OS_WIN)
    const QString platform = "windows";
#elif defined(Q_OS_MACOS)
    const QString platform = "macos";
#else
    const QString platform = "linux";
#endif
    const auto cpu = QSysInfo::currentCpuArchitecture();
    const auto arch = cpu == "arm64" || cpu == "aarch64" ? "arm64" : "x86_64";
    return QStringLiteral("vibePlayerQT-%1-%2.zip").arg(platform, arch);
}
QJsonObject release(const QString& version, const QString& package = nativePackageName())
{
    return {{"tag_name", version}, {"draft", false}, {"body", "notes"},
            {"assets", QJsonArray {QJsonObject {{"name", package},
                {"browser_download_url", "https://example.test/" + package}}}}};
}
}

class UpdateServiceTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesStrictVersions();
    void comparesSemanticVersions();
    void filtersChannels();
    void parsesChecksumSidecar();
    void selectsHighestRelease();
    void skipsReleasesWithoutPlatformAssets();
};

void UpdateServiceTest::parsesStrictVersions()
{
    QVERIFY(UpdateService::parseVersion(QStringLiteral("v1.2.3")));
    QVERIFY(UpdateService::parseVersion(QStringLiteral("1.2.3-beta.1")));
    QVERIFY(!UpdateService::parseVersion(QStringLiteral("1.2")));
    QVERIFY(UpdateService::parseVersion(QStringLiteral("1.2.3-rc.1")));
    QVERIFY(!UpdateService::parseVersion(QStringLiteral("1.02.3")));
}

void UpdateServiceTest::comparesSemanticVersions()
{
    const auto alpha = *UpdateService::parseVersion(QStringLiteral("1.0.0-alpha"));
    const auto beta = *UpdateService::parseVersion(QStringLiteral("1.0.0-beta.1"));
    const auto stable = *UpdateService::parseVersion(QStringLiteral("1.0.0-stable"));
    QVERIFY(UpdateService::compareVersions(alpha, beta) < 0);
    QVERIFY(UpdateService::compareVersions(beta, stable) < 0);
    QCOMPARE(UpdateService::compareVersions(stable, stable), 0);
}

void UpdateServiceTest::filtersChannels()
{
    const auto stable = *UpdateService::parseVersion(QStringLiteral("1.0.1-stable"));
    const auto beta = *UpdateService::parseVersion(QStringLiteral("1.0.1-beta.2"));
    const auto alpha = *UpdateService::parseVersion(QStringLiteral("1.0.1-alpha"));
    const auto rc = *UpdateService::parseVersion(QStringLiteral("1.0.1-rc.1"));
    const auto unmarked = *UpdateService::parseVersion(QStringLiteral("1.0.1"));
    QVERIFY(UpdateService::channelAccepts(stable, UpdateChannel::Stable));
    QVERIFY(!UpdateService::classifyVersion(unmarked));
    QVERIFY(!UpdateService::channelAccepts(beta, UpdateChannel::Stable));
    QVERIFY(UpdateService::channelAccepts(beta, UpdateChannel::Beta));
    QVERIFY(!UpdateService::channelAccepts(alpha, UpdateChannel::Beta));
    QVERIFY(UpdateService::channelAccepts(alpha, UpdateChannel::Alpha));
    QVERIFY(!UpdateService::channelAccepts(rc, UpdateChannel::Alpha));
    QCOMPARE(UpdateService::classifyVersion(stable).value(), UpdateChannel::Stable);
    QCOMPARE(UpdateService::classifyVersion(beta).value(), UpdateChannel::Beta);
    QCOMPARE(UpdateService::classifyVersion(alpha).value(), UpdateChannel::Alpha);
    QVERIFY(!UpdateService::classifyVersion(rc));
}

void UpdateServiceTest::parsesChecksumSidecar()
{
    const auto hash = QString(64, QLatin1Char('a')).toLatin1();
    const auto fileName = QStringLiteral("vibePlayerQT-1.0.1-windows-x86_64-installer.exe");
    QCOMPARE(UpdateService::parseChecksum(hash + "  " + fileName.toUtf8(), fileName).value(), QString(64, QLatin1Char('a')));
    QVERIFY(!UpdateService::parseChecksum(hash + "  other.exe", fileName));
    QCOMPARE(UpdateService::parseChecksum(hash + " " + fileName.toUtf8(), fileName).value(), QString(64, 'a'));
    QCOMPARE(UpdateService::parseChecksum(hash + " *" + fileName.toUtf8(), fileName).value(), QString(64, 'a'));
    QCOMPARE(UpdateService::parseChecksum(hash + "  other.exe\r\n" + hash + "  " + fileName.toUtf8() + "\r\n", fileName).value(), QString(64, 'a'));
    QVERIFY(!UpdateService::parseChecksum(hash + "  " + fileName.toUtf8() + "\n" + QByteArray(64, 'b') + "  " + fileName.toUtf8(), fileName));
    QVERIFY(!UpdateService::parseChecksum(hash + "  path/" + fileName.toUtf8(), fileName));
}

void UpdateServiceTest::selectsHighestRelease()
{
    const auto current = *UpdateService::parseVersion(QStringLiteral("1.0.0-stable"));
    auto draft = release("v1.0.3-stable"); draft.insert("draft", true);
    auto latest = release("v1.0.2-stable"); latest.insert("body", "new");
    const auto json = QJsonDocument(QJsonArray {release("v1.0.1-stable"), draft, latest}).toJson();
    const auto result = UpdateService::parseReleases(json, UpdateChannel::Stable, current);
    QVERIFY(result);
    QCOMPARE(result->version.toString(), QStringLiteral("1.0.2-stable"));
    QCOMPARE(result->notes, QStringLiteral("new"));
    QCOMPARE(result->assets.size(), 1);
}

void UpdateServiceTest::skipsReleasesWithoutPlatformAssets()
{
    const auto current = *UpdateService::parseVersion("1.0.0-stable");
    auto empty = release("v1.0.4-stable"); empty.insert("assets", QJsonArray {});
    const auto wrong = release("v1.0.3-stable", "vibePlayerQT-unsupported-platform.zip");
    const auto supported = release("v1.0.2-stable");
    const auto result = UpdateService::parseReleases(QJsonDocument(QJsonArray {empty, wrong, supported}).toJson(), UpdateChannel::Stable, current);
    QVERIFY(result);
    QCOMPARE(result->version.toString(), QString("1.0.2-stable"));
    QVERIFY(!UpdateService::parseReleases(QJsonDocument(QJsonArray {empty, wrong}).toJson(), UpdateChannel::Stable, current));
}

QTEST_MAIN(UpdateServiceTest)
#include "UpdateServiceTest.moc"
