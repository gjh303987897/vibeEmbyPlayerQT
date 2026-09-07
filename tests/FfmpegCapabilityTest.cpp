#include "services/ffmpeg/FfmpegCapability.h"

#include <QTest>
#include <QVersionNumber>

class FfmpegCapabilityTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesNumericBanners();
    void parsesTaggedAndPrefixedBanners();
    void rejectsDateStampedAndGarbageBanners();
    void featureScanReportsMissingEncodersAndMuxers();
};

void FfmpegCapabilityTest::parsesNumericBanners()
{
    const auto plain = FfmpegCapabilityProbe::parseVersionBanner(
        QStringLiteral("ffmpeg version 7.1.1 Copyright (c) 2000-2024 the FFmpeg developers\n"));
    QVERIFY(plain.has_value());
    QCOMPARE(*plain, (QVersionNumber(7, 1, 1)));

    const auto patchless = FfmpegCapabilityProbe::parseVersionBanner(
        QStringLiteral("ffmpeg version 6 Copyright (c)\n"));
    QVERIFY(patchless.has_value());
    QCOMPARE(*patchless, (QVersionNumber(6, 0, 0)));
}

void FfmpegCapabilityTest::parsesTaggedAndPrefixedBanners()
{
    // BtbN and gyan.dev suffix the version with build metadata.
    const auto essentials = FfmpegCapabilityProbe::parseVersionBanner(
        QStringLiteral("ffmpeg version 7.1-essentials_build-www.gyan.dev Copyright (c)\n"));
    QVERIFY(essentials.has_value());
    QCOMPARE(*essentials, (QVersionNumber(7, 1, 0)));

    // Some distributions prefix the release tag with 'n'.
    const auto tagged = FfmpegCapabilityProbe::parseVersionBanner(
        QStringLiteral("ffmpeg version n6.1.1-3-gabcdef Copyright (c)\n"));
    QVERIFY(tagged.has_value());
    QCOMPARE(*tagged, (QVersionNumber(6, 1, 1)));
}

void FfmpegCapabilityTest::rejectsDateStampedAndGarbageBanners()
{
    // A date-stamped git build carries no comparable numeric version: the
    // probe must return an error (callers then trust the feature scan).
    QVERIFY(!FfmpegCapabilityProbe::parseVersionBanner(
        QStringLiteral("ffmpeg version git-2024-05-01-abcdef Copyright (c)\n")).has_value());
    QVERIFY(!FfmpegCapabilityProbe::parseVersionBanner(QStringLiteral("not ffmpeg")).has_value());
    QVERIFY(!FfmpegCapabilityProbe::parseVersionBanner(QString {}).has_value());
}

void FfmpegCapabilityTest::featureScanReportsMissingEncodersAndMuxers()
{
    const QStringList fullEncoders { QStringLiteral("libx264"), QStringLiteral("libx265"),
                                     QStringLiteral("aac"), QStringLiteral("hevc") };
    const QStringList hlsMuxers { QStringLiteral("hls"), QStringLiteral("mpegts") };
    QVERIFY(FfmpegCapabilityProbe::missingRequiredFeatures(fullEncoders, hlsMuxers).isEmpty());

    // A --disable-gpl build has no libx264/libx265: the pipeline needs both.
    const QStringList lgplEncoders { QStringLiteral("aac") };
    const auto missing = FfmpegCapabilityProbe::missingRequiredFeatures(lgplEncoders, hlsMuxers);
    QCOMPARE(missing.size(), 2);
    QVERIFY(missing.contains(QStringLiteral("encoder:libx264")));
    QVERIFY(missing.contains(QStringLiteral("encoder:libx265")));

    // Missing HLS muxer is fatal on its own even with every encoder present.
    const QStringList noHls { QStringLiteral("mp4"), QStringLiteral("mpegts") };
    const auto muxerMissing = FfmpegCapabilityProbe::missingRequiredFeatures(fullEncoders, noHls);
    QCOMPARE(muxerMissing, QStringList { QStringLiteral("muxer:hls") });
}

QTEST_GUILESS_MAIN(FfmpegCapabilityTest)
#include "FfmpegCapabilityTest.moc"
