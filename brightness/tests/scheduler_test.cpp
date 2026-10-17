// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The scheduler's rules (scheduler.h, docs/brightness.md §5.4, §5.5), one at
// a time, with the results of each transaction given by hand.

extern "C" {
#include "scheduler.h"
}

#include <gtest/gtest.h>

#include <vector>

namespace {

class Scheduler : public ::testing::Test {
protected:
    void SetUp() override
    {
        vela_scheduler_init(&s, now, now + boot_offset, [](void* data, const vela_event* event) {
            static_cast<Scheduler*>(data)->events.push_back(*event);
        }, this);
    }

    vela_action next() { return vela_scheduler_next(&s, now); }

    // The next action must be a read (of `kind`) now.
    void expect_read(vela_read_kind kind)
    {
        vela_action action = next();
        ASSERT_EQ(action.kind, VELA_ACTION_READ) << "at " << now;
        ASSERT_EQ(action.read, kind);
    }
    void read(vela_result result, uint16_t value = 0, uint16_t max = 100)
    {
        vela_scheduler_read_done(&s, result, value, max, now);
    }
    void expect_write(uint16_t value)
    {
        vela_action action = next();
        ASSERT_EQ(action.kind, VELA_ACTION_WRITE) << "at " << now;
        ASSERT_EQ(action.value, value);
    }
    void wrote(vela_result result = VELA_RESULT_OK) { vela_scheduler_write_done(&s, result, now); }
    void expect_wait(int64_t until)
    {
        vela_action action = next();
        ASSERT_EQ(action.kind, VELA_ACTION_WAIT) << "at " << now;
        ASSERT_EQ(action.at, until);
    }
    void set(uint64_t seq, uint32_t value) { vela_scheduler_set(&s, seq, value, now); }

    void ready(uint16_t value = 50)
    {
        expect_read(VELA_READ_PROBE);
        read(VELA_RESULT_OK, value);
        events.clear();
    }

    // The clocks move together.
    void advance(int64_t ms)
    {
        now += ms;
        vela_scheduler_clock(&s, now, now + boot_offset);
    }

    const vela_event& last() { return events.back(); }

    vela_scheduler s {};
    std::vector<vela_event> events;
    int64_t now = 1000000;
    int64_t boot_offset = 5000;
};

} // namespace

TEST_F(Scheduler, ProbesAtOnceAndReportsReady)
{
    expect_read(VELA_READ_PROBE);
    read(VELA_RESULT_OK, 80, 100);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, VELA_EVENT_STATE);
    EXPECT_EQ(events[0].state, VELA_STATE_READY);
    EXPECT_EQ(events[0].value, 80);
    EXPECT_EQ(events[0].max, 100);
    expect_wait(VELA_NEVER); // nothing else to do
}

TEST_F(Scheduler, WritesThenReadsBackAfterThePause)
{
    ready(50);
    set(1, 80);
    expect_write(80);
    wrote();
    expect_wait(now + VELA_READBACK_MS);
    advance(VELA_READBACK_MS);
    expect_read(VELA_READ_BACK);
    read(VELA_RESULT_OK, 80);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(last().kind, VELA_EVENT_VALUE);
    EXPECT_EQ(last().seq, 1u);
    EXPECT_EQ(last().value, 80);
    expect_wait(VELA_NEVER);
}

TEST_F(Scheduler, DragKeepsTheIntervalAndWritesOnlyTheNewest)
{
    ready(50);
    set(1, 60);
    expect_write(60);
    wrote();
    int64_t first = now;
    // Three targets in the next 100 ms: none written before the interval.
    for (uint32_t v : { 61, 62, 63 }) {
        advance(30);
        set(v - 59, v);
        expect_wait(first + VELA_WRITE_INTERVAL_MS);
    }
    now = first + VELA_WRITE_INTERVAL_MS;
    expect_write(63);
    wrote();
    EXPECT_TRUE(events.empty());
}

TEST_F(Scheduler, ANewTargetDuringTheReadBackPauseIsWrittenFirst)
{
    ready(50);
    set(1, 60);
    expect_write(60);
    wrote();
    advance(500);
    set(2, 70);
    expect_write(70);
    wrote();
    expect_wait(now + VELA_READBACK_MS); // the pause starts again
}

TEST_F(Scheduler, DroppedWriteIsWrittenAgain)
{
    ready(50);
    set(1, 80);
    expect_write(80);
    wrote();
    advance(VELA_READBACK_MS);
    expect_read(VELA_READ_BACK);
    read(VELA_RESULT_OK, 50); // the monitor dropped it
    EXPECT_TRUE(events.empty());
    expect_write(80);
    wrote();
    advance(VELA_READBACK_MS);
    expect_read(VELA_READ_BACK);
    read(VELA_RESULT_OK, 80);
    EXPECT_EQ(last().kind, VELA_EVENT_VALUE);
    EXPECT_EQ(last().value, 80);
}

TEST_F(Scheduler, RefusedAfterTheRewrites)
{
    ready(50);
    set(4, 80);
    int writes = 0;
    for (;;) {
        vela_action action = next();
        if (action.kind == VELA_ACTION_WAIT) {
            ASSERT_NE(action.at, VELA_NEVER);
            now = action.at;
            continue;
        }
        if (action.kind == VELA_ACTION_WRITE) {
            ++writes;
            wrote();
        } else {
            read(VELA_RESULT_OK, 50); // locked
            if (!events.empty()) {
                break;
            }
        }
    }
    EXPECT_EQ(writes, 1 + VELA_REWRITES);
    EXPECT_EQ(last().kind, VELA_EVENT_REFUSED);
    EXPECT_EQ(last().seq, 4u);
    EXPECT_EQ(last().value, 50);
    expect_wait(VELA_NEVER);
}

TEST_F(Scheduler, BudgetWithoutAnswersIsNotResponding)
{
    ready(50);
    set(1, 80);
    int64_t start = now;
    while (events.empty()) {
        vela_action action = next();
        if (action.kind == VELA_ACTION_WAIT) {
            ASSERT_NE(action.at, VELA_NEVER);
            now = action.at;
        } else if (action.kind == VELA_ACTION_WRITE) {
            wrote();
        } else {
            read(VELA_RESULT_NO_ANSWER);
        }
    }
    EXPECT_EQ(last().kind, VELA_EVENT_STATE);
    EXPECT_EQ(last().state, VELA_STATE_NOT_RESPONDING);
    EXPECT_LE(now - start, VELA_BUDGET_MS);
}

TEST_F(Scheduler, TargetAlreadyThereIsConfirmedAtOnce)
{
    ready(50);
    set(3, 50);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(last().kind, VELA_EVENT_VALUE);
    EXPECT_EQ(last().seq, 3u);
    expect_wait(VELA_NEVER);
}

TEST_F(Scheduler, BackToTheStartAfterADragIsStillWritten)
{
    ready(50);
    set(1, 60);
    expect_write(60);
    wrote();
    set(2, 50); // the monitor isn't at 50 any more
    EXPECT_TRUE(events.empty());
    now += VELA_WRITE_INTERVAL_MS;
    expect_write(50);
}

TEST_F(Scheduler, OldSequenceNumbersAreIgnored)
{
    ready(50);
    set(5, 80);
    set(4, 20);
    set(5, 30);
    expect_write(80);
}

TEST_F(Scheduler, TargetsAreClampedToTheMaximum)
{
    ready(50);
    set(1, 400);
    expect_write(100);
}

TEST_F(Scheduler, LiveSyncReadsOnceASecondOnlyWhenOn)
{
    ready(50);
    expect_wait(VELA_NEVER);
    vela_scheduler_live(&s, true);
    expect_wait(now + VELA_LIVE_MS); // the probe was a read
    advance(VELA_LIVE_MS);
    expect_read(VELA_READ_LIVE);
    read(VELA_RESULT_OK, 50);
    EXPECT_TRUE(events.empty()); // unchanged: nothing to say
    advance(VELA_LIVE_MS);
    expect_read(VELA_READ_LIVE);
    read(VELA_RESULT_OK, 35); // the monitor's buttons
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(last().kind, VELA_EVENT_VALUE);
    EXPECT_EQ(last().value, 35);
    vela_scheduler_live(&s, false);
    expect_wait(VELA_NEVER);
}

TEST_F(Scheduler, LiveSyncNeverReadsDuringAWriteNorRightAfter)
{
    ready(50);
    vela_scheduler_live(&s, true);
    advance(VELA_LIVE_MS);
    set(1, 80);
    expect_write(80);
    wrote();
    int64_t written = now;
    // Until the read back, only waiting.
    while (now < written + VELA_READBACK_MS) {
        vela_action action = next();
        ASSERT_EQ(action.kind, VELA_ACTION_WAIT);
        now = action.at;
    }
    expect_read(VELA_READ_BACK);
    read(VELA_RESULT_OK, 80);
    // The read back counts as the latest read.
    expect_wait(now + VELA_LIVE_MS);
}

TEST_F(Scheduler, FailedReadsMakeItNotRespondingAndAGoodOneRecovers)
{
    ready(50);
    vela_scheduler_live(&s, true);
    for (int i = 0; i < VELA_FAILED_READS; ++i) {
        advance(VELA_LIVE_MS);
        expect_read(VELA_READ_LIVE);
        read(VELA_RESULT_NO_ANSWER);
    }
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(last().state, VELA_STATE_NOT_RESPONDING);
    advance(VELA_LIVE_MS);
    expect_read(VELA_READ_LIVE);
    read(VELA_RESULT_OK, 42);
    EXPECT_EQ(last().state, VELA_STATE_READY);
    EXPECT_EQ(last().value, 42);
}

TEST_F(Scheduler, NoAnswerIsProbedAgainAfterTwoTenAndThirtySeconds)
{
    int64_t start = now;
    std::vector<int64_t> probes;
    for (int i = 0; i < 20; ++i) {
        vela_action action = next();
        if (action.kind == VELA_ACTION_WAIT) {
            if (action.at == VELA_NEVER) {
                break;
            }
            now = action.at;
            continue;
        }
        probes.push_back(now - start);
        read(VELA_RESULT_NO_ANSWER);
    }
    EXPECT_EQ(probes, (std::vector<int64_t> { 0, 2000, 10000, 30000 }));
    EXPECT_EQ(s.state, VELA_STATE_NO_ANSWER);
    ASSERT_EQ(events.size(), 1u); // one state line, not four
    // Settings opening the page: at once.
    vela_scheduler_probe(&s, now);
    expect_read(VELA_READ_PROBE);
    read(VELA_RESULT_OK, 70);
    EXPECT_EQ(last().state, VELA_STATE_READY);
}

TEST_F(Scheduler, BusyBusIsSkippedNotCounted)
{
    ready(50);
    vela_scheduler_live(&s, true);
    for (int i = 0; i < 2 * VELA_FAILED_READS; ++i) {
        advance(VELA_LIVE_MS);
        expect_read(VELA_READ_LIVE);
        read(VELA_RESULT_BUSY);
    }
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(s.state, VELA_STATE_READY);
}

TEST_F(Scheduler, BusyWriteWaitsAndRetries)
{
    ready(50);
    set(1, 80);
    expect_write(80);
    wrote(VELA_RESULT_BUSY);
    expect_wait(now + VELA_BUSY_RETRY_MS);
    advance(VELA_BUSY_RETRY_MS);
    expect_write(80);
}

TEST_F(Scheduler, StatesWithoutBrightnessIgnoreTargets)
{
    expect_read(VELA_READ_PROBE);
    read(VELA_RESULT_UNSUPPORTED);
    EXPECT_EQ(last().state, VELA_STATE_UNSUPPORTED);
    set(1, 50);
    expect_wait(VELA_NEVER);

    vela_scheduler_probe(&s, now);
    expect_read(VELA_READ_PROBE);
    read(VELA_RESULT_NO_ACCESS);
    EXPECT_EQ(last().state, VELA_STATE_NO_ACCESS);

    vela_scheduler_probe(&s, now);
    expect_read(VELA_READ_PROBE);
    read(VELA_RESULT_NO_BUS);
    EXPECT_EQ(last().state, VELA_STATE_NO_BUS);
    expect_wait(VELA_NEVER);
}

TEST_F(Scheduler, PowerOffMeansNoTrafficAndPowerOnWakesUp)
{
    ready(50);
    vela_scheduler_live(&s, true);
    vela_scheduler_power(&s, false, now);
    set(1, 80);
    expect_wait(VELA_NEVER);
    advance(60000);
    expect_wait(VELA_NEVER);
    vela_scheduler_power(&s, true, now);
    expect_wait(now + VELA_WAKE_MS);
    advance(VELA_WAKE_MS);
    expect_read(VELA_READ_WAKE);
    read(VELA_RESULT_OK, 50);
    expect_write(80); // the target waited
}

TEST_F(Scheduler, SessionInactiveMeansNoTraffic)
{
    ready(50);
    vela_scheduler_session(&s, false, now);
    vela_scheduler_live(&s, true);
    advance(10000);
    expect_wait(VELA_NEVER);
    vela_scheduler_session(&s, true, now);
    expect_wait(now + VELA_WAKE_MS);
}

TEST_F(Scheduler, WakeUpFailuresDontCount)
{
    ready(50);
    vela_scheduler_power(&s, false, now);
    vela_scheduler_power(&s, true, now);
    // Asleep for six seconds: three wake-up reads fail.
    for (int i = 0; i < 3; ++i) {
        advance(VELA_WAKE_MS);
        expect_read(VELA_READ_WAKE);
        read(VELA_RESULT_NO_ANSWER);
    }
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(s.state, VELA_STATE_READY);
    advance(VELA_WAKE_MS);
    expect_read(VELA_READ_WAKE);
    read(VELA_RESULT_OK, 50);
    EXPECT_TRUE(events.empty());
    expect_wait(VELA_NEVER);
}

TEST_F(Scheduler, WakeUpThatNeverAnswersCountsAfterTheGrace)
{
    ready(50);
    vela_scheduler_power(&s, false, now);
    vela_scheduler_power(&s, true, now);
    int64_t woke = now;
    while (now < woke + VELA_WAKE_GRACE_MS) {
        vela_action action = next();
        if (action.kind == VELA_ACTION_WAIT) {
            now = action.at;
        } else {
            read(VELA_RESULT_NO_ANSWER);
        }
    }
    EXPECT_TRUE(events.empty());
    // The first read after the grace counts.
    expect_read(VELA_READ_WAKE);
    read(VELA_RESULT_NO_ANSWER);
    EXPECT_EQ(s.failed_reads, 1);
}

TEST_F(Scheduler, SuspendIsAWakeUp)
{
    ready(50);
    vela_scheduler_live(&s, true);
    boot_offset += 3600 * 1000; // an hour asleep: only the boot clock moved
    advance(0);
    expect_wait(now + VELA_WAKE_MS);
}

TEST_F(Scheduler, SuspendDuringADragKeepsTheTarget)
{
    ready(50);
    set(1, 80);
    expect_write(80);
    wrote();
    boot_offset += 600 * 1000;
    advance(10);
    // A read that failed because the computer slept through it.
    advance(VELA_WAKE_MS);
    expect_read(VELA_READ_WAKE);
    read(VELA_RESULT_NO_ANSWER);
    advance(VELA_WAKE_MS);
    expect_read(VELA_READ_WAKE);
    read(VELA_RESULT_OK, 50); // the write was lost
    expect_write(80); // written again, not counted as a rewrite
    EXPECT_EQ(s.rewrites, 0);
    wrote();
    advance(VELA_READBACK_MS);
    expect_read(VELA_READ_BACK);
    read(VELA_RESULT_OK, 80);
    EXPECT_EQ(last().kind, VELA_EVENT_VALUE);
    for (const vela_event& event : events) {
        EXPECT_NE(event.state, VELA_STATE_NOT_RESPONDING);
    }
}

TEST_F(Scheduler, MaximumChangeIsANewReadyLine)
{
    ready(50);
    vela_scheduler_live(&s, true);
    advance(VELA_LIVE_MS);
    expect_read(VELA_READ_LIVE);
    read(VELA_RESULT_OK, 50, 255);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(last().kind, VELA_EVENT_STATE);
    EXPECT_EQ(last().max, 255);
}
