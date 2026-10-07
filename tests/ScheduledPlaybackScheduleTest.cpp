#include "services/scheduler/ScheduledPlaybackSchedule.h"

#include <QTest>
#include <QTimeZone>

namespace {
ScheduledPlaybackTask task(QString type,
                           QString startTime,
                           QString days = {},
                           QString lastRunDate = {})
{
    return ScheduledPlaybackTask {
        .id = QStringLiteral("task"),
        .scheduleType = std::move(type),
        .startTime = std::move(startTime),
        .scheduleDays = std::move(days),
        .enabled = true,
        .lastRunDate = std::move(lastRunDate),
    };
}

QDateTime utcDateTime(const QDate& date, const QTime& time)
{
    return QDateTime(date, time, QTimeZone::fromSecondsAheadOfUtc(0));
}
}

class ScheduledPlaybackScheduleTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesAndSerializesDays();
    void keepsEmptyScheduleDaysNonNull();
    void calculatesDailyRuns();
    void calculatesWeeklyRuns();
    void skipsMissingMonthlyDates();
    void calculatesCustomMonthlyRuns();
    void respectsLastRunDate();
    void detectsMissedOccurrencesWithinWindow();
    void ignoresManualAndDisabledTasks();
    void resolvesDaylightSavingGapAndRepeatedHour();
};

void ScheduledPlaybackScheduleTest::parsesAndSerializesDays()
{
    QCOMPARE(ScheduledPlaybackSchedule::parseDays(QStringLiteral("28,14,21,14,0,32"), 1, 31),
             QList<int>({ 14, 21, 28 }));
    QCOMPARE(ScheduledPlaybackSchedule::serializeDays({ 28, 14, 21, 14, 0, 32 }, 1, 31),
             QStringLiteral("14,21,28"));
}

void ScheduledPlaybackScheduleTest::keepsEmptyScheduleDaysNonNull()
{
    const ScheduledPlaybackTask emptyTask;
    QVERIFY(emptyTask.scheduleDays.isEmpty());
    QVERIFY(!emptyTask.scheduleDays.isNull());
}

void ScheduledPlaybackScheduleTest::calculatesDailyRuns()
{
    const auto daily = task(QStringLiteral("daily"), QStringLiteral("12:00"));
    const auto date = QDate(2026, 7, 15);

    const auto sameDay = ScheduledPlaybackSchedule::nextOccurrence(
        daily, utcDateTime(date, QTime(11, 59)));
    QVERIFY(sameDay.has_value());
    QCOMPARE(*sameDay, utcDateTime(date, QTime(12, 0)));

    const auto nextDay = ScheduledPlaybackSchedule::nextOccurrence(
        daily, utcDateTime(date, QTime(12, 0)));
    QVERIFY(nextDay.has_value());
    QCOMPARE(*nextDay, utcDateTime(date.addDays(1), QTime(12, 0)));
}

void ScheduledPlaybackScheduleTest::calculatesWeeklyRuns()
{
    const auto monday = QDate(2026, 7, 13);
    QCOMPARE(monday.dayOfWeek(), 1);
    const auto weekly = task(QStringLiteral("weekly"), QStringLiteral("08:30"), QStringLiteral("3"));

    const auto nextRun = ScheduledPlaybackSchedule::nextOccurrence(
        weekly, utcDateTime(monday, QTime(9, 0)));
    QVERIFY(nextRun.has_value());
    QCOMPARE(*nextRun, utcDateTime(monday.addDays(2), QTime(8, 30)));
}

void ScheduledPlaybackScheduleTest::skipsMissingMonthlyDates()
{
    const auto monthly = task(QStringLiteral("monthly"), QStringLiteral("09:00"), QStringLiteral("31"));
    const auto nextRun = ScheduledPlaybackSchedule::nextOccurrence(
        monthly, utcDateTime(QDate(2026, 4, 30), QTime(10, 0)));

    QVERIFY(nextRun.has_value());
    QCOMPARE(*nextRun, utcDateTime(QDate(2026, 5, 31), QTime(9, 0)));
}

void ScheduledPlaybackScheduleTest::calculatesCustomMonthlyRuns()
{
    const auto custom = task(QStringLiteral("custom_monthly"),
                             QStringLiteral("12:00"),
                             QStringLiteral("14,21,28"));
    const auto nextRun = ScheduledPlaybackSchedule::nextOccurrence(
        custom, utcDateTime(QDate(2026, 7, 15), QTime(10, 0)));

    QVERIFY(nextRun.has_value());
    QCOMPARE(*nextRun, utcDateTime(QDate(2026, 7, 21), QTime(12, 0)));
}

void ScheduledPlaybackScheduleTest::respectsLastRunDate()
{
    const auto daily = task(QStringLiteral("daily"),
                            QStringLiteral("12:00"),
                            {},
                            QStringLiteral("2026-07-15"));
    const auto nextRun = ScheduledPlaybackSchedule::nextOccurrence(
        daily, utcDateTime(QDate(2026, 7, 15), QTime(10, 0)));

    QVERIFY(nextRun.has_value());
    QCOMPARE(*nextRun, utcDateTime(QDate(2026, 7, 16), QTime(12, 0)));
}

void ScheduledPlaybackScheduleTest::detectsMissedOccurrencesWithinWindow()
{
    const auto daily = task(QStringLiteral("daily"), QStringLiteral("12:00"));
    const auto from = utcDateTime(QDate(2026, 7, 14), QTime(18, 0));

    const auto missed = ScheduledPlaybackSchedule::firstOccurrenceBetween(
        daily, from, utcDateTime(QDate(2026, 7, 15), QTime(14, 0)));
    QVERIFY(missed.has_value());
    QCOMPARE(*missed, utcDateTime(QDate(2026, 7, 15), QTime(12, 0)));

    QVERIFY(!ScheduledPlaybackSchedule::firstOccurrenceBetween(
        daily, from, utcDateTime(QDate(2026, 7, 15), QTime(10, 0))).has_value());
}

void ScheduledPlaybackScheduleTest::ignoresManualAndDisabledTasks()
{
    auto manual = task(QStringLiteral("manual"), QStringLiteral("manual"));
    QVERIFY(!ScheduledPlaybackSchedule::nextOccurrence(
        manual, utcDateTime(QDate(2026, 7, 15), QTime(10, 0))).has_value());

    auto disabled = task(QStringLiteral("daily"), QStringLiteral("12:00"));
    disabled.enabled = false;
    QVERIFY(!ScheduledPlaybackSchedule::nextOccurrence(
        disabled, utcDateTime(QDate(2026, 7, 15), QTime(10, 0))).has_value());
}

void ScheduledPlaybackScheduleTest::resolvesDaylightSavingGapAndRepeatedHour()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    const QTimeZone berlin("Europe/Berlin");
    QVERIFY(berlin.isValid());
    const auto daily = task("daily", "02:30");
    const auto spring = ScheduledPlaybackSchedule::nextOccurrence(daily, QDateTime(QDate(2026,3,29), QTime(0,0), berlin));
    QVERIFY(spring);
    QCOMPARE(spring->date(), QDate(2026,3,29));
    QCOMPARE(spring->time(), QTime(3,30));
    QCOMPARE(spring->offsetFromUtc(), 7200);
    const auto autumn = ScheduledPlaybackSchedule::nextOccurrence(daily, QDateTime(QDate(2026,10,25), QTime(0,0), berlin));
    QVERIFY(autumn);
    QCOMPARE(autumn->time(), QTime(2,30));
    QCOMPARE(autumn->offsetFromUtc(), 7200); // First occurrence, before the repeated hour.
    const auto following = ScheduledPlaybackSchedule::nextOccurrence(daily, autumn->addSecs(1));
    QVERIFY(following);
    QCOMPARE(following->date(), QDate(2026,10,26));
    QCOMPARE(following->time(), QTime(2,30));
    QVERIFY(!ScheduledPlaybackSchedule::firstOccurrenceBetween(daily, autumn->addSecs(1), autumn->addSecs(3600)));
#else
    QSKIP("Explicit DST resolution is documented and verified for Qt 6.7 and newer");
#endif
}

QTEST_GUILESS_MAIN(ScheduledPlaybackScheduleTest)

#include "ScheduledPlaybackScheduleTest.moc"
