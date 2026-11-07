// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// BlueZ's errors in plain words, and an Add's steps (outcome.h).

extern "C" {
#include "outcome.h"
}

#include <gtest/gtest.h>

#include <string>

namespace {

std::string code(vela_bt_step step, const char* name, const char* message = nullptr)
{
    const vela_bt_failure* failure = vela_bt_failure_of(step, name, message);
    return failure ? failure->code : "success";
}

} // namespace

TEST(Outcome, ErrorsThatMeanSuccess)
{
    EXPECT_EQ(code(VELA_BT_STEP_PAIR, "org.bluez.Error.AlreadyExists"), "success");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.bluez.Error.AlreadyConnected"), "success");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.bluez.Error.Failed", "br-connection-already-connected"), "success");
    EXPECT_EQ(code(VELA_BT_STEP_DISCONNECT, "org.bluez.Error.NotConnected"), "success");
    EXPECT_EQ(code(VELA_BT_STEP_REMOVE, "org.bluez.Error.DoesNotExist"), "success");
    // Only for their own step.
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.bluez.Error.AlreadyExists"), "failed");
    EXPECT_EQ(code(VELA_BT_STEP_PAIR, "org.bluez.Error.DoesNotExist"), "device-gone");
}

TEST(Outcome, PairingErrors)
{
    EXPECT_EQ(code(VELA_BT_STEP_PAIR, "org.bluez.Error.AuthenticationRejected"), "rejected");
    EXPECT_EQ(code(VELA_BT_STEP_PAIR, "org.bluez.Error.AuthenticationCanceled"), "pairing-cancelled");
    EXPECT_EQ(code(VELA_BT_STEP_PAIR, "org.bluez.Error.AuthenticationFailed"), "auth-failed");
    EXPECT_EQ(code(VELA_BT_STEP_PAIR, "org.bluez.Error.AuthenticationTimeout"), "no-response");
    EXPECT_EQ(code(VELA_BT_STEP_PAIR, "org.bluez.Error.ConnectionAttemptFailed", "Page Timeout"), "no-response");
    EXPECT_EQ(code(VELA_BT_STEP_PAIR, "org.bluez.Error.InProgress"), "busy");
    EXPECT_EQ(code(VELA_BT_STEP_PAIR, "org.bluez.Error.Failed", "Bonding already in progress"), "busy");
    EXPECT_STREQ(vela_bt_failure_of(VELA_BT_STEP_PAIR, "org.bluez.Error.AuthenticationRejected", nullptr)->message,
        "Pairing was refused on the device.");
}

TEST(Outcome, ConnectionReasons)
{
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.bluez.Error.Failed", "br-connection-page-timeout"), "no-response");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.bluez.Error.Failed", "le-connection-abort-by-remote"), "no-response");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.bluez.Error.Failed", "br-connection-profile-unavailable"), "not-supported");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.bluez.Error.Failed", "br-connection-adapter-not-powered"), "adapter-off");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.bluez.Error.Failed", "le-connection-busy"), "busy");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.bluez.Error.Failed", "br-connection-key-missing"), "forgotten");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.bluez.Error.Failed", "br-connection-unknown"), "failed");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.bluez.Error.NotAvailable"), "not-supported");
}

TEST(Outcome, ConnectionRefusedIsNotNoResponse)
{
    EXPECT_STREQ(vela_bt_failure_of(VELA_BT_STEP_CONNECT, "org.bluez.Error.Failed",
        "br-connection-refused")->code, "failed");
}

TEST(Outcome, BusErrors)
{
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.freedesktop.DBus.Error.ServiceUnknown"), "not-running");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.freedesktop.DBus.Error.NoReply"), "no-response");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "org.freedesktop.DBus.Error.Timeout"), "no-response");
    EXPECT_EQ(code(VELA_BT_STEP_RENAME, "org.freedesktop.DBus.Error.UnknownObject"), "device-gone");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, "com.example.Whatever"), "failed");
    EXPECT_EQ(code(VELA_BT_STEP_CONNECT, nullptr), "failed");
}

TEST(Outcome, PowerErrors)
{
    EXPECT_EQ(code(VELA_BT_STEP_POWER, "org.bluez.Error.Blocked"), "hard-blocked");
    EXPECT_EQ(code(VELA_BT_STEP_POWER, "org.bluez.Error.Failed", "Blocked through rfkill"), "hard-blocked");
    EXPECT_EQ(code(VELA_BT_STEP_POWER, "org.bluez.Error.Failed"), "power-failed");
}

TEST(Outcome, AddGoesPairTrustConnect)
{
    EXPECT_EQ(vela_bt_add_next(VELA_BT_STEP_PAIR, true), VELA_BT_ADD_TRUST);
    EXPECT_EQ(vela_bt_add_next(VELA_BT_STEP_PAIR, false), VELA_BT_ADD_FAILED);
    // A failed Trust is shown, and the Add goes on.
    EXPECT_EQ(vela_bt_add_next(VELA_BT_STEP_TRUST, true), VELA_BT_ADD_CONNECT);
    EXPECT_EQ(vela_bt_add_next(VELA_BT_STEP_TRUST, false), VELA_BT_ADD_CONNECT);
    EXPECT_EQ(vela_bt_add_next(VELA_BT_STEP_CONNECT, true), VELA_BT_ADD_DONE);
    EXPECT_EQ(vela_bt_add_next(VELA_BT_STEP_CONNECT, false), VELA_BT_ADD_FAILED);
}

TEST(Outcome, StepStates)
{
    EXPECT_STREQ(vela_bt_step_state(VELA_BT_STEP_PAIR), "pairing");
    EXPECT_STREQ(vela_bt_step_state(VELA_BT_STEP_CONNECT), "connecting");
    EXPECT_STREQ(vela_bt_step_state(VELA_BT_STEP_REMOVE), "removing");
}
