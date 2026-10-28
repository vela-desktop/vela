// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// rfkill switches on a fake /sys (rfkill.h): the adapter's own, the
// platform's, and the event written to a fake /dev/rfkill.

extern "C" {
#include "rfkill.h"
}

#include <gtest/gtest.h>

#include <linux/rfkill.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

class Rfkill : public testing::Test {
protected:
    fs::path root;

    void SetUp() override
    {
        std::string pattern = ::testing::TempDir() + "vela-bluetooth-rfkill-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
        // hci0 with switch 3, a dongle hci1 with switch 7, a Wi-Fi switch 1
        // and a platform Bluetooth switch 5 (asus-wmi).
        addSwitch("class/bluetooth/hci0/rfkill3", "bluetooth", 3, true, false);
        addSwitch("class/bluetooth/hci1/rfkill7", "bluetooth", 7, true, false);
        addSwitch("class/rfkill/rfkill3", "bluetooth", 3, true, false);
        addSwitch("class/rfkill/rfkill7", "bluetooth", 7, true, false);
        addSwitch("class/rfkill/rfkill1", "wlan", 1, true, false);
        addSwitch("class/rfkill/rfkill5", "bluetooth", 5, true, false);
        std::ofstream(root / "rfkill").flush();
    }

    void TearDown() override { fs::remove_all(root); }

    void addSwitch(const std::string& dir, const char* type, int index, bool soft, bool hard)
    {
        fs::create_directories(root / dir);
        std::ofstream(root / dir / "type") << type << '\n';
        std::ofstream(root / dir / "index") << index << '\n';
        std::ofstream(root / dir / "soft") << soft << '\n';
        std::ofstream(root / dir / "hard") << hard << '\n';
    }

    std::string written()
    {
        std::ifstream file(root / "rfkill", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    }
};

} // namespace

TEST(RfkillNames, AdapterPaths)
{
    EXPECT_STREQ(vela_bt_hci_name("/org/bluez/hci0"), "hci0");
    EXPECT_STREQ(vela_bt_hci_name("/org/bluez/hci12"), "hci12");
    EXPECT_EQ(vela_bt_hci_name("/org/bluez/hci0/dev_00_11"), nullptr);
    EXPECT_EQ(vela_bt_hci_name("/org/bluez/hci"), nullptr);
    EXPECT_EQ(vela_bt_hci_name("/org/bluez/hci0/../hci1"), nullptr);
    EXPECT_EQ(vela_bt_hci_name("/org/bluez"), nullptr);
    EXPECT_EQ(vela_bt_hci_name(nullptr), nullptr);
}

TEST_F(Rfkill, FindsTheAdaptersOwnSwitch)
{
    vela_bt_switch found {};
    ASSERT_EQ(vela_bt_adapter_switch(root.c_str(), "hci0", &found), 0);
    EXPECT_EQ(found.index, 3);
    EXPECT_TRUE(found.soft);
    EXPECT_FALSE(found.hard);
    ASSERT_EQ(vela_bt_adapter_switch(root.c_str(), "hci1", &found), 0);
    EXPECT_EQ(found.index, 7);
    EXPECT_LT(vela_bt_adapter_switch(root.c_str(), "hci2", &found), 0);
    EXPECT_LT(vela_bt_adapter_switch(root.c_str(), "../hci0", &found), 0);
}

TEST_F(Rfkill, PlatformSwitchesAreTheBluetoothOnesWithoutAnAdapter)
{
    vela_bt_switch found[8];
    ASSERT_EQ(vela_bt_platform_switches(root.c_str(), found, 8), 1);
    EXPECT_EQ(found[0].index, 5);
    EXPECT_TRUE(found[0].soft);
}

TEST_F(Rfkill, UnblocksOneSwitchByIndex)
{
    ASSERT_EQ(vela_bt_unblock((root / "rfkill").c_str(), 3), 0);
    const std::string bytes = written();
    ASSERT_EQ(bytes.size(), size_t(RFKILL_EVENT_SIZE_V1));
    rfkill_event event {};
    memcpy(&event, bytes.data(), RFKILL_EVENT_SIZE_V1);
    EXPECT_EQ(event.op, RFKILL_OP_CHANGE);
    EXPECT_EQ(event.idx, 3u);
    EXPECT_EQ(event.type, RFKILL_TYPE_BLUETOOTH);
    EXPECT_EQ(event.soft, 0);
    EXPECT_LT(vela_bt_unblock((root / "missing").c_str(), 3), 0);
}
