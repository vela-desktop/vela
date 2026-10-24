// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The whole helper (helper.h) on fake monitors and a fake clock: the lines
// the compositor sends, the lines it gets back, and what the monitor saw.
// Includes the regressions COSMIC fixed (docs/brightness.md §4.3, §12).

extern "C" {
#include "fake.h"
#include "helper.h"
}

#include "fake_clock.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

struct Rig {
    FakeClock clock;
    vela_fake_monitor monitor {};
    vela_helper helper {};
    std::vector<std::string> lines; // without busy/idle
    int busy = 0;
    int idle = 0;
    int open_while_waiting = 0; // COSMIC #165

    explicit Rig(const std::string& keys)
    {
        std::string text = "[X]\n" + keys;
        char error[128];
        EXPECT_EQ(vela_fake_parse(&monitor, text.c_str(), "X", error, sizeof(error)), 0) << error;
        vela_fake_start(&monitor, &clock.clock);
        vela_helper_init(&helper, vela_fake_transport(&monitor), &clock.clock, [](void* data, const char* line) {
            Rig* rig = static_cast<Rig*>(data);
            std::string text(line);
            EXPECT_EQ(text.back(), '\n');
            text.pop_back();
            if (text == "busy") {
                ++rig->busy;
            } else if (text == "idle") {
                ++rig->idle;
            } else {
                rig->lines.push_back(text);
            }
        }, this);
        helper.power_on = [](void* data) { vela_fake_power_on(static_cast<vela_fake_monitor*>(data)); };
        helper.power_data = &monitor;
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // Runs the helper for `ms` of monitor time, as main.c's loop does.
    void run(int64_t ms)
    {
        int64_t end = clock.monotonic + ms;
        for (;;) {
            int64_t at = vela_helper_run(&helper);
            EXPECT_EQ(busy, idle);
            if (at <= clock.monotonic) {
                continue;
            }
            if (monitor.is_open) {
                ++open_while_waiting;
            }
            if (at >= end) {
                if (end > clock.monotonic) {
                    clock.advance(end - clock.monotonic);
                }
                return;
            }
            clock.advance(at - clock.monotonic);
        }
    }

    void send(const std::string& line) { vela_helper_line(&helper, line.c_str()); }

    // A drag from `from` to `to`, a target every 16 ms (a 60 Hz slider).
    uint64_t drag(int from, int to, uint64_t seq)
    {
        int step = from < to ? 1 : -1;
        for (int v = from; v != to + step; v += step) {
            send("set " + std::to_string(++seq) + " " + std::to_string(v));
            run(16);
        }
        return seq;
    }

    bool saw(const std::string& line) const { return std::find(lines.begin(), lines.end(), line) != lines.end(); }

    int count_prefix(const std::string& prefix) const
    {
        return (int)std::count_if(lines.begin(), lines.end(),
            [&](const std::string& line) { return line.rfind(prefix, 0) == 0; });
    }
};

// The monitors of §3.
const char* lg = "ignore-write-gap = 5-190\nbad-replies = 2\n";
const char* mi = "drop-writes = 30\nbad-replies = 50\n";

} // namespace

TEST(Helper, StartsByProbing)
{
    Rig rig("value = 70\n");
    rig.run(1000);
    EXPECT_EQ(rig.lines, (std::vector<std::string> { "state ready 70 100" }));
    EXPECT_EQ(rig.busy, 1);
    EXPECT_EQ(rig.open_while_waiting, 0);
}

TEST(Helper, LgDragEndsOnTheLastValue)
{
    Rig rig(lg);
    rig.run(500);
    uint64_t last = rig.drag(100, 40, 0);
    rig.run(3000);
    EXPECT_EQ(rig.monitor.value, 40);
    EXPECT_EQ(rig.lines.back(), "value " + std::to_string(last) + " 40");
    // No write ever came 5-190 ms after another, so the LG kept them all.
    EXPECT_EQ(rig.monitor.sets, rig.monitor.applied);
    EXPECT_EQ(rig.count_prefix("refused"), 0);
}

TEST(Helper, MiDragEndsOnTheLastValueDespiteDrops)
{
    for (int seed = 1; seed <= 20; ++seed) {
        Rig rig(std::string(mi) + "seed = " + std::to_string(seed) + "\n");
        rig.run(500);
        uint64_t last = rig.drag(100, 40, 0);
        rig.run(VELA_BUDGET_MS + 2000);
        EXPECT_EQ(rig.monitor.value, 40) << "seed " << seed;
        EXPECT_EQ(rig.lines.back(), "value " + std::to_string(last) + " 40") << "seed " << seed;
    }
}

// COSMIC #1483: a drag costs a bounded, known number of transactions.
TEST(Helper, DragTransactionsAreBounded)
{
    Rig rig("");
    rig.run(500);
    int before = rig.busy;
    rig.drag(100, 40, 0); // 61 targets in ~1 s
    rig.run(3000);
    int writes = rig.monitor.sets;
    // One write per interval during the drag, plus the last; one read back.
    int64_t drag_ms = 61 * 16;
    EXPECT_LE(writes, drag_ms / VELA_WRITE_INTERVAL_MS + 2);
    EXPECT_GE(writes, 2);
    EXPECT_EQ(rig.busy - before, writes + 1);
}

// COSMIC #165: no device open between transactions, during a drag's pauses,
// or while idle.
TEST(Helper, DeviceIsNeverHeldWhileWaiting)
{
    Rig rig(mi);
    rig.send("live on");
    rig.run(500);
    rig.drag(100, 30, 0);
    rig.run(20000);
    EXPECT_EQ(rig.open_while_waiting, 0);
    EXPECT_FALSE(rig.monitor.is_open);
    EXPECT_EQ(rig.monitor.unlocked, 0);
}

// COSMIC #183: a read never overlaps a write, and never comes right after one.
TEST(Helper, ReadsNeverOverlapWrites)
{
    Rig rig(mi);
    rig.send("live on");
    rig.run(500);
    rig.drag(100, 60, 0);
    rig.run(2000);
    rig.drag(60, 80, 1000);
    rig.run(10000);
    ASSERT_LE(rig.monitor.log_count, VELA_FAKE_LOG);
    bool open = false;
    bool got_set = false, got_get = false;
    int64_t last_set = INT64_MIN;
    for (int i = 0; i < rig.monitor.log_count; ++i) {
        const vela_fake_entry& entry = rig.monitor.log[i];
        switch (entry.op) {
        case VELA_FAKE_OPEN:
            EXPECT_FALSE(open);
            open = true;
            got_set = got_get = false;
            break;
        case VELA_FAKE_CLOSE:
            open = false;
            break;
        case VELA_FAKE_SET:
            EXPECT_TRUE(open);
            EXPECT_FALSE(got_set || got_get) << "a transaction is one write";
            got_set = true;
            last_set = entry.at;
            break;
        case VELA_FAKE_GET:
            EXPECT_TRUE(open);
            EXPECT_FALSE(got_set) << "a read inside a write's transaction";
            got_get = true;
            if (last_set != INT64_MIN) {
                EXPECT_GE(entry.at - last_set, VELA_READBACK_MS) << "a read right after a write";
            }
            break;
        case VELA_FAKE_REPLY:
            break;
        }
    }
}

// COSMIC #150: two outputs are two helpers; one's drag never touches the other.
TEST(Helper, OneOutputsDragNeverTouchesAnother)
{
    Rig left(lg);
    Rig right(mi);
    left.run(500);
    right.run(500);
    int right_opens = right.monitor.opens;
    left.drag(100, 50, 0);
    left.run(3000);
    right.run(3000);
    EXPECT_EQ(left.monitor.value, 50);
    EXPECT_EQ(right.monitor.value, 100);
    EXPECT_EQ(right.monitor.opens, right_opens);
}

TEST(Helper, ButtonsMoveAnOpenSlider)
{
    Rig rig("value = 80\nbuttons = 5000:30\n");
    rig.run(500);
    rig.send("live on");
    rig.run(10000);
    EXPECT_TRUE(rig.saw("value 0 30"));
}

TEST(Helper, ButtonsAreNotReadWithoutASlider)
{
    Rig rig("value = 80\nbuttons = 5000:30\n");
    rig.run(500);
    int opens = rig.monitor.opens;
    rig.run(60000);
    EXPECT_EQ(rig.monitor.opens, opens);
    EXPECT_EQ(rig.lines.size(), 1u);
}

TEST(Helper, LiveValueCarriesTheLatestSeq)
{
    Rig rig("value = 80\nbuttons = 8000:30\n");
    rig.run(500);
    rig.send("set 6 60");
    rig.run(3000);
    EXPECT_EQ(rig.lines.back(), "value 6 60");
    rig.send("live on");
    rig.run(10000);
    EXPECT_EQ(rig.lines.back(), "value 6 30");
}

TEST(Helper, LockedMonitorIsRefused)
{
    Rig rig("value = 70\nlocked = yes\n");
    rig.run(500);
    rig.send("set 1 20");
    rig.run(VELA_BUDGET_MS + 1000);
    EXPECT_EQ(rig.lines.back(), "refused 1 70");
    EXPECT_EQ(rig.monitor.sets, 1 + VELA_REWRITES);
}

TEST(Helper, DdcOffIsNoAnswerAndGivesUpAfterThreeRetries)
{
    Rig rig("ddc = no\n");
    rig.run(10 * 60 * 1000);
    EXPECT_EQ(rig.lines, (std::vector<std::string> { "state no-answer" }));
    EXPECT_EQ(rig.monitor.opens, 4);
    rig.send("probe");
    rig.run(1000);
    EXPECT_EQ(rig.monitor.opens, 5);
}

TEST(Helper, MonitorWakingUpAtStartBecomesReady)
{
    Rig rig("asleep = 5000\nvalue = 60\n");
    rig.run(20000);
    EXPECT_EQ(rig.lines, (std::vector<std::string> { "state no-answer", "state ready 60 100" }));
}

TEST(Helper, UnsupportedAndNoBus)
{
    Rig unsupported("unsupported = yes\n");
    unsupported.run(60000);
    EXPECT_EQ(unsupported.lines, (std::vector<std::string> { "state unsupported" }));
    EXPECT_EQ(unsupported.monitor.opens, 1);
    Rig no_bus("no-bus = yes\n");
    no_bus.run(1000);
    EXPECT_EQ(no_bus.lines, (std::vector<std::string> { "state no-bus" }));
}

TEST(Helper, PowerOffIsSilentAndPowerOnWaitsForTheMonitor)
{
    Rig rig("asleep = 4000\n");
    rig.run(12000); // asleep at start too: ready at the probe of 10 s
    ASSERT_EQ(rig.lines.back(), "state ready 100 100");
    rig.lines.clear();
    rig.send("live on");
    rig.run(3000);
    rig.send("power off");
    int opens = rig.monitor.opens;
    rig.send("set 1 50");
    rig.run(60000);
    EXPECT_EQ(rig.monitor.opens, opens);
    rig.send("power on"); // the monitor sleeps for 4 s again
    rig.run(15000);
    EXPECT_EQ(rig.monitor.value, 50);
    EXPECT_EQ(rig.lines, (std::vector<std::string> { "value 1 50" }));
}

TEST(Helper, SessionSwitchIsSilent)
{
    Rig rig("");
    rig.run(500);
    rig.send("live on");
    rig.send("session inactive");
    int opens = rig.monitor.opens;
    rig.run(30000);
    EXPECT_EQ(rig.monitor.opens, opens);
    rig.send("session active");
    rig.run(VELA_WAKE_MS - 10);
    EXPECT_EQ(rig.monitor.opens, opens);
    rig.run(100);
    EXPECT_EQ(rig.monitor.opens, opens + 1);
}

TEST(Helper, SuspendInTheMiddleOfADragAndOfLiveSync)
{
    Rig rig(std::string(mi) + "asleep = 9000\n");
    rig.run(12000);
    ASSERT_EQ(rig.lines.back(), "state ready 100 100");
    rig.lines.clear();
    rig.send("live on");
    uint64_t seq = rig.drag(100, 70, 0);
    // The computer sleeps for an hour; on resume the monitor needs 9 s,
    // longer than a write's budget: only the wake-up's grace covers it.
    rig.clock.suspend(3600 * 1000);
    vela_fake_power_on(&rig.monitor);
    seq = rig.drag(70, 50, seq);
    rig.run(20000);
    EXPECT_EQ(rig.monitor.value, 50);
    EXPECT_EQ(rig.lines.back(), "value " + std::to_string(seq) + " 50");
    EXPECT_FALSE(rig.saw("state not-responding"));
    EXPECT_EQ(rig.count_prefix("refused"), 0);
}

TEST(Helper, BusyBusSkipsLiveReadsWithoutFailing)
{
    Rig rig("bus-busy = 20000\n");
    rig.run(25000);
    EXPECT_EQ(rig.lines, (std::vector<std::string> { "state ready 100 100" }));
    rig.send("live on");
    rig.run(5000);
    EXPECT_EQ(rig.lines.size(), 1u);
}

TEST(Helper, NotRespondingRecoversByItself)
{
    // Answers, then stops (io-errors from the start would never be ready).
    Rig rig("value = 50\n");
    rig.run(500);
    rig.send("live on");
    rig.monitor.io_errors = 100;
    rig.run(10000);
    EXPECT_TRUE(rig.saw("state not-responding"));
    rig.monitor.io_errors = 0;
    rig.run(3000);
    EXPECT_EQ(rig.lines.back(), "state ready 50 100");
}

TEST(Helper, ManySetLinesAtOnceOnlyTheLastCounts)
{
    Rig rig("");
    rig.run(500);
    std::string burst;
    for (int i = 1; i <= 30; ++i) {
        burst += "set " + std::to_string(i) + " " + std::to_string(100 - i) + "\n";
    }
    vela_helper_feed(&rig.helper, burst.data(), burst.size());
    rig.run(3000);
    EXPECT_EQ(rig.monitor.sets, 1);
    EXPECT_EQ(rig.lines.back(), "value 30 70");
}
