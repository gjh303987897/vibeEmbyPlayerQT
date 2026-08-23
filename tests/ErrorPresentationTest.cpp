#include "utils/ErrorPresentation.h"

#include <QtTest>

class ErrorPresentationTest final : public QObject
{
    Q_OBJECT

private slots:
    void classifiesPackagingError()
    {
        const auto result = ErrorPresentation::fromMessage(QStringLiteral("The generated HLS segments contain no decodable video track; choose H.264 or H.265 video encoding."));
        QCOMPARE(result.code, AppErrorCode::Packaging);
        QCOMPARE(result.summaryKey, QStringLiteral("error.packagingSummary"));
        QVERIFY(!result.details.isEmpty());
    }

    void classifiesNetworkError()
    {
        const auto result = ErrorPresentation::fromMessage(QStringLiteral("Network request timed out"));
        QCOMPARE(result.code, AppErrorCode::Network);
        QCOMPARE(result.summaryKey, QStringLiteral("error.networkSummary"));
    }

    void sanitizesCredentialsAndLimitsDetails()
    {
        const auto input = QStringLiteral("Authorization: Bearer secret-token https://user:password@example.test/a?token=abc&x=1 ").repeated(1000);
        const auto result = ErrorPresentation::fromMessage(input);
        QVERIFY(result.details.size() <= 12000);
        QVERIFY(!result.details.contains(QStringLiteral("secret-token")));
        QVERIFY(!result.details.contains(QStringLiteral("password@example")));
        QVERIFY(!result.details.contains(QStringLiteral("token=abc")));
        QVERIFY(result.details.contains(QStringLiteral("[REDACTED]")));
    }

    void unknownErrorKeepsReadableOriginal()
    {
        const auto result = ErrorPresentation::fromMessage(QStringLiteral("A custom operation failed"));
        QCOMPARE(result.summaryKey, QStringLiteral("error.genericSummary"));
        QCOMPARE(result.details, QStringLiteral("A custom operation failed"));
    }
};

QTEST_MAIN(ErrorPresentationTest)
#include "ErrorPresentationTest.moc"
