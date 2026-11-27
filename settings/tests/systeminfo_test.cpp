// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// What Settings > About makes of the system files and of lspci
// (src/systeminfo.h), from what real machines print.

#include "systeminfo.h"

#include <QTest>

class SystemInfoTest : public QObject {
    Q_OBJECT

private slots:
    void cpuName_data()
    {
        QTest::addColumn<QString>("cpuinfo");
        QTest::addColumn<QString>("name");
        QTest::newRow("x86") << QStringLiteral("processor\t: 0\nvendor_id\t: AuthenticAMD\n"
                                               "model name\t: AMD Ryzen 9 9950X 16-Core Processor  \n\n"
                                               "processor\t: 1\nmodel name\t: AMD Ryzen 9 9950X 16-Core Processor\n")
                             << QStringLiteral("AMD Ryzen 9 9950X 16-Core Processor   (2 thread)");
        // ARM boards list processors with no model name.
        QTest::newRow("no model name") << QStringLiteral("processor\t: 0\nBogoMIPS\t: 108.00\n")
                                       << QStringLiteral("aarch64   (1 thread)");
        QTest::newRow("unreadable") << QString() << QStringLiteral("aarch64");
    }

    void cpuName()
    {
        QFETCH(QString, cpuinfo);
        QFETCH(QString, name);
        QCOMPARE(systeminfo::cpuName(cpuinfo, QStringLiteral("aarch64")), name);
    }

    void memory()
    {
        const std::optional<double> gib =
            systeminfo::memoryGiB(QStringLiteral("MemTotal:       65536000 kB\nMemFree:  1024 kB\n"));
        QVERIFY(gib);
        QVERIFY(qAbs(*gib - 62.5) < 1e-9);
        QVERIFY(!systeminfo::memoryGiB(QStringLiteral("MemFree:  1024 kB\n")));
        QVERIFY(!systeminfo::memoryGiB(QString()));
    }

    void graphicsCards()
    {
        const QString lspci = QStringLiteral(
            "00:00.0 Host bridge: Advanced Micro Devices, Inc. [AMD] Device 14d8\n"
            "03:00.0 VGA compatible controller: Advanced Micro Devices, Inc. [AMD/ATI] Navi 48 [Radeon RX 9070 XT] "
            "(rev c0)\n"
            "04:00.0 Display controller: Advanced Micro Devices, Inc. [AMD/ATI] Granite Ridge (rev c5)\n"
            "01:00.0 VGA compatible controller: NVIDIA Corporation AD107 [GeForce RTX 4060] (rev a1)\n"
            "02:00.0 3D controller: NVIDIA Corporation GA100\n"
            "00:02.0 VGA compatible controller: Intel Corporation Meteor Lake-P [Intel Arc Graphics] (rev 08)\n"
            "00:02.0 VGA compatible controller: Intel Corporation HD Graphics 620 (rev 02)\n"
            "00:01.0 VGA compatible controller: Red Hat, Inc. Virtio 1.0 GPU (rev 01)\n");
        QCOMPARE(systeminfo::graphicsCards(lspci),
            (QStringList { QStringLiteral("AMD Radeon RX 9070 XT"), QStringLiteral("AMD Granite Ridge"),
                QStringLiteral("NVIDIA GeForce RTX 4060"), QStringLiteral("NVIDIA GA100"),
                QStringLiteral("Intel Intel Arc Graphics"), QStringLiteral("Intel HD Graphics 620"),
                QStringLiteral("Red Hat, Inc. Virtio 1.0 GPU") }));
        QVERIFY(systeminfo::graphicsCards(QString()).isEmpty());
    }

    void osReleaseValue()
    {
        const QString text = QStringLiteral("NAME=\"Arch Linux\"\nPRETTY_NAME=\"Arch Linux\"\nID=arch\n"
                                            "BUILD_ID=rolling\nANSI_COLOR=\"38;2;23;147;209\"\n");
        QCOMPARE(systeminfo::osReleaseValue(text, QStringLiteral("PRETTY_NAME")), QStringLiteral("Arch Linux"));
        QCOMPARE(systeminfo::osReleaseValue(text, QStringLiteral("ID")), QStringLiteral("arch"));
        // A key is matched whole, never as the end of another one.
        QCOMPARE(systeminfo::osReleaseValue(text, QStringLiteral("_ID")), QString());
        QCOMPARE(systeminfo::osReleaseValue(text, QStringLiteral("VERSION_ID")), QString());
        // Regex characters in the key are only characters.
        QCOMPARE(systeminfo::osReleaseValue(text, QStringLiteral(".*")), QString());
    }
};

QTEST_APPLESS_MAIN(SystemInfoTest)
#include "systeminfo_test.moc"
