// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The fake monitors (fake.h) and the transactions over them (transfer.h):
// the faults of docs/brightness.md §3, reproduced.

extern "C" {
#include "fake.h"
#include "transfer.h"
}

#include "fake_clock.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <string>
#include <vector>

namespace {

struct Monitor {
    FakeClock clock;
    vela_fake_monitor fake {};
    vela_transport transport {};

    explicit Monitor(const std::string& keys, const char* connector = "card1-DP-3")
    {
        std::string text = "[DP-3]\n" + keys;
        char error[128];
        EXPECT_EQ(vela_fake_parse(&fake, text.c_str(), connector, error, sizeof(error)), 0) << error;
        vela_fake_start(&fake, &clock.clock);
        transport = vela_fake_transport(&fake);
    }
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    vela_result read(uint16_t* value = nullptr, vela_transaction_log* log = nullptr)
    {
        uint16_t v = 0, max = 0;
        vela_result result = vela_ddc_read(&transport, &clock.clock, &v, &max, log);
        if (value) {
            *value = v;
        }
        return result;
    }
    vela_result write(uint16_t value) { return vela_ddc_write(&transport, &clock.clock, value, nullptr); }
};

int parse(const std::string& text, const char* connector, vela_fake_monitor& monitor, std::string* error = nullptr)
{
    char message[128] = "";
    int result = vela_fake_parse(&monitor, text.c_str(), connector, message, sizeof(message));
    if (error) {
        *error = message;
    }
    return result;
}

} // namespace

TEST(FakeFile, SectionsMatchWithOrWithoutTheCard)
{
    vela_fake_monitor monitor;
    const std::string text = "# two monitors\n[HDMI-A-1]\nvalue = 40\n\n[card1-DP-3]\nvalue = 60 # a comment\n";
    ASSERT_EQ(parse(text, "card1-HDMI-A-1", monitor), 0);
    EXPECT_EQ(monitor.value, 40);
    ASSERT_EQ(parse(text, "HDMI-A-1", monitor), 0);
    EXPECT_EQ(monitor.value, 40);
    ASSERT_EQ(parse(text, "card1-DP-3", monitor), 0);
    EXPECT_EQ(monitor.value, 60);
    EXPECT_EQ(parse(text, "DP-3", monitor), -ENOENT); // the section names the card
    EXPECT_EQ(parse(text, "card2-HDMI-A-1x", monitor), -ENOENT);
    EXPECT_EQ(parse(text, "HEADLESS-1", monitor), -ENOENT);
}

TEST(FakeFile, ReadsEveryKey)
{
    vela_fake_monitor m;
    ASSERT_EQ(parse("[X]\nmax = 255\nvalue = 128\nignore-write-gap = 5-50\ndrop-writes = 30\n"
                    "bad-replies = 50\nnull-replies = 10\nio-errors = 5\nsettle = 300\nasleep = 2000\n"
                    "buttons = 1000:40, 5000:90\nlocked = yes\nddc = no\nunsupported = yes\nno-bus = no\n"
                    "no-access = no\nbus-busy = 700\nhang-after = 3\nseed = 42\n",
                  "X", m),
        0);
    EXPECT_EQ(m.max, 255);
    EXPECT_EQ(m.value, 128);
    EXPECT_EQ(m.ignore_gap_lo, 5);
    EXPECT_EQ(m.ignore_gap_hi, 50);
    EXPECT_EQ(m.drop_writes, 30);
    EXPECT_EQ(m.bad_replies, 50);
    EXPECT_EQ(m.null_replies, 10);
    EXPECT_EQ(m.io_errors, 5);
    EXPECT_EQ(m.settle, 300);
    EXPECT_EQ(m.asleep, 2000);
    ASSERT_EQ(m.button_count, 2);
    EXPECT_EQ(m.buttons[1].at, 5000);
    EXPECT_EQ(m.buttons[1].value, 90);
    EXPECT_TRUE(m.locked);
    EXPECT_FALSE(m.ddc);
    EXPECT_TRUE(m.unsupported);
    EXPECT_EQ(m.bus_busy, 700);
    EXPECT_EQ(m.hang_after, 3);
    EXPECT_EQ(m.seed, 42u);
}

TEST(FakeFile, RejectsMistakesAnywhere)
{
    vela_fake_monitor m;
    std::string error;
    EXPECT_EQ(parse("[X]\nmax = 100\nvalue = 101\n", "X", m, &error), -EINVAL);
    EXPECT_EQ(parse("[X]\nvalue\n", "X", m, &error), -EINVAL);
    EXPECT_EQ(error, "line 2: expected key = value");
    EXPECT_EQ(parse("[X]\ndrop-writes = 101\n", "X", m), -EINVAL);
    EXPECT_EQ(parse("[X]\nlocked = maybe\n", "X", m), -EINVAL);
    EXPECT_EQ(parse("[X]\nignore-write-gap = 50-5\n", "X", m), -EINVAL);
    EXPECT_EQ(parse("[X]\nbuttons = 500:40, 100:50\n", "X", m), -EINVAL); // not in order
    EXPECT_EQ(parse("[X]\nvalue = -1\n", "X", m), -EINVAL);
    // A typo in another monitor's section is an error too.
    EXPECT_EQ(parse("[X]\nvalue = 5\n[Y]\nvlaue = 5\n", "X", m, &error), -EINVAL);
    EXPECT_EQ(error, "line 4: bad vlaue");
}

TEST(FakeFile, ListsSections)
{
    std::vector<std::string> names;
    vela_fake_sections("[A]\nvalue = 1\n# [not]\n[ B ]\n", [](void* data, const char* name) {
        static_cast<std::vector<std::string>*>(data)->push_back(name);
    }, &names);
    EXPECT_EQ(names, (std::vector<std::string> { "A", "B" }));
}

TEST(Fake, ReadsAndWrites)
{
    Monitor monitor("value = 80\n");
    uint16_t value = 0;
    ASSERT_EQ(monitor.read(&value), VELA_RESULT_OK);
    EXPECT_EQ(value, 80);
    ASSERT_EQ(monitor.write(30), VELA_RESULT_OK);
    monitor.clock.advance(500);
    ASSERT_EQ(monitor.read(&value), VELA_RESULT_OK);
    EXPECT_EQ(value, 30);
    // The device is open only during a transaction, and every request holds
    // the lock (COSMIC #165).
    EXPECT_FALSE(monitor.fake.is_open);
    EXPECT_EQ(monitor.fake.unlocked, 0);
    EXPECT_EQ(monitor.fake.opens, 3);
}

TEST(Fake, ValuesAboveTheMaximumAreClamped)
{
    Monitor monitor("max = 50\nvalue = 50\n");
    ASSERT_EQ(monitor.write(400), VELA_RESULT_OK);
    EXPECT_EQ(monitor.fake.value, 50);
}

// §3's tables for the LG: 90, a pause, 70, read 1.5 s later.
TEST(Fake, LgIgnoresAWriteFiveTo190MsAfterAnother)
{
    const std::vector<std::pair<int, uint16_t>> table = {
        { 0, 70 }, { 5, 90 }, { 20, 90 }, { 100, 90 }, { 190, 90 }, { 200, 70 }, { 300, 70 }, { 800, 70 },
    };
    for (auto [pause, expected] : table) {
        Monitor lg("ignore-write-gap = 5-190\n");
        lg.write(90);
        lg.clock.advance(pause);
        lg.write(70);
        lg.clock.advance(1500);
        uint16_t value = 0;
        ASSERT_EQ(lg.read(&value), VELA_RESULT_OK);
        EXPECT_EQ(value, expected) << "pause " << pause;
    }
}

TEST(Fake, MiFixedBlockCostsAttemptsNotReads)
{
    Monitor mi("bad-replies = 50\n");
    int ok = 0;
    int attempts = 0;
    for (int i = 0; i < 100; ++i) {
        vela_transaction_log log;
        if (mi.read(nullptr, &log) == VELA_RESULT_OK) {
            ++ok;
        }
        attempts += log.attempts;
        for (int a = 0; a + 1 < log.attempts; ++a) {
            EXPECT_EQ(log.attempt[a].reply, VELA_DDC_REPLY_BAD); // only the last one is good
        }
    }
    // Six attempts at 50% fail together about 1.6% of the time.
    EXPECT_GE(ok, 95);
    EXPECT_GT(attempts, 150);
}

TEST(Fake, AllBadRepliesIsNoAnswerAfterSixAttempts)
{
    Monitor mi("bad-replies = 100\n");
    vela_transaction_log log;
    int64_t start = mi.clock.monotonic;
    EXPECT_EQ(mi.read(nullptr, &log), VELA_RESULT_NO_ANSWER);
    EXPECT_EQ(log.attempts, VELA_DDC_READ_ATTEMPTS);
    EXPECT_EQ(mi.clock.monotonic - start,
        VELA_DDC_READ_ATTEMPTS * VELA_DDC_REPLY_WAIT_MS + (VELA_DDC_READ_ATTEMPTS - 1) * VELA_DDC_RETRY_MS);
}

TEST(Fake, NullMessagesAndIoErrorsAreAttemptsToo)
{
    Monitor null("null-replies = 100\n");
    vela_transaction_log log;
    EXPECT_EQ(null.read(nullptr, &log), VELA_RESULT_NO_ANSWER);
    EXPECT_EQ(log.attempt[0].reply, VELA_DDC_REPLY_NULL);
    Monitor eio("io-errors = 100\n");
    EXPECT_EQ(eio.read(nullptr, &log), VELA_RESULT_NO_ANSWER);
    EXPECT_EQ(log.attempt[0].error, EIO);
}

TEST(Fake, DdcOffNeverAnswers)
{
    Monitor off("ddc = no\n");
    EXPECT_EQ(off.read(), VELA_RESULT_NO_ANSWER);
    EXPECT_EQ(off.write(50), VELA_RESULT_NO_ANSWER);
    EXPECT_EQ(off.fake.value, 100);
}

TEST(Fake, UnsupportedAnswersSo)
{
    Monitor monitor("unsupported = yes\n");
    vela_transaction_log log;
    EXPECT_EQ(monitor.read(nullptr, &log), VELA_RESULT_UNSUPPORTED);
    EXPECT_EQ(log.attempts, 1); // a real answer: no more attempts
}

TEST(Fake, MaximumOfZeroIsUnsupported)
{
    Monitor monitor("max = 0\nvalue = 0\n");
    EXPECT_EQ(monitor.read(), VELA_RESULT_UNSUPPORTED);
}

TEST(Fake, NoBusAndNoAccess)
{
    Monitor no_bus("no-bus = yes\n");
    EXPECT_EQ(no_bus.read(), VELA_RESULT_NO_BUS);
    EXPECT_EQ(no_bus.write(5), VELA_RESULT_NO_BUS);
    Monitor no_access("no-access = yes\n");
    EXPECT_EQ(no_access.read(), VELA_RESULT_NO_ACCESS);
}

TEST(Fake, BusyBusWaitsForTheLockWithinItsBudget)
{
    Monitor shortly("bus-busy = 300\n");
    vela_transaction_log log;
    EXPECT_EQ(shortly.read(nullptr, &log), VELA_RESULT_OK);
    EXPECT_GE(log.lock_ms, 300);
    EXPECT_LT(log.lock_ms, 300 + VELA_DDC_LOCK_RETRY_MS);

    Monitor long_busy("bus-busy = 5000\n");
    EXPECT_EQ(long_busy.read(nullptr, &log), VELA_RESULT_BUSY);
    EXPECT_GE(log.lock_ms, VELA_DDC_LOCK_BUDGET_MS);
    EXPECT_EQ(log.attempts, 0);
    EXPECT_FALSE(long_busy.fake.is_open);
    EXPECT_EQ(long_busy.fake.gets, 0); // nothing sent without the lock
}

TEST(Fake, ButtonsChangeTheValue)
{
    Monitor monitor("value = 100\nbuttons = 1000:40, 2000:300\n");
    uint16_t value = 0;
    monitor.read(&value);
    EXPECT_EQ(value, 100);
    monitor.clock.advance(1000);
    monitor.read(&value);
    EXPECT_EQ(value, 40);
    monitor.clock.advance(1000);
    monitor.read(&value);
    EXPECT_EQ(value, 100); // clamped to the maximum
}

TEST(Fake, LockedAcceptsWritesAndKeepsItsValue)
{
    Monitor monitor("value = 70\nlocked = yes\n");
    EXPECT_EQ(monitor.write(20), VELA_RESULT_OK);
    monitor.clock.advance(500);
    uint16_t value = 0;
    monitor.read(&value);
    EXPECT_EQ(value, 70);
}

TEST(Fake, AsleepAtStartAndAfterPowerOn)
{
    Monitor monitor("asleep = 3000\n");
    EXPECT_EQ(monitor.read(), VELA_RESULT_NO_ANSWER);
    monitor.clock.advance(3000);
    EXPECT_EQ(monitor.read(), VELA_RESULT_OK);
    vela_fake_power_on(&monitor.fake);
    EXPECT_EQ(monitor.read(), VELA_RESULT_NO_ANSWER);
    monitor.clock.advance(3000);
    EXPECT_EQ(monitor.read(), VELA_RESULT_OK);
}

TEST(Fake, SettlingAfterAWrite)
{
    Monitor monitor("settle = 300\n");
    monitor.write(50);
    vela_transaction_log log;
    uint16_t value = 0;
    // Attempts every 90 ms: the first four fall within the 300 ms.
    EXPECT_EQ(monitor.read(&value, &log), VELA_RESULT_OK);
    EXPECT_EQ(value, 50);
    EXPECT_EQ(log.attempt[0].error, EIO);
    EXPECT_EQ(log.attempt[log.attempts - 1].error, 0);
}

TEST(Fake, SameSeedSameFaults)
{
    auto run = [](const char* keys) {
        Monitor mi(keys);
        std::vector<int> attempts;
        for (int i = 0; i < 30; ++i) {
            vela_transaction_log log;
            mi.read(nullptr, &log);
            attempts.push_back(log.attempts);
            mi.write((uint16_t)(i * 3));
            mi.clock.advance(200);
        }
        attempts.push_back(mi.fake.applied);
        return attempts;
    };
    const char* mi = "bad-replies = 50\ndrop-writes = 30\nseed = 7\n";
    EXPECT_EQ(run(mi), run(mi));
    EXPECT_NE(run(mi), run("bad-replies = 50\ndrop-writes = 30\nseed = 8\n"));
}
