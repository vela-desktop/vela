// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../common/primaryoutput.h"
#include <QTest>
class PrimaryOutputTest : public QObject {
    Q_OBJECT
private slots:
    void preferredWinsOverQt()
    {
        QCOMPARE(vela::primaryOutputName("DP-2", {"HDMI-A-1", "DP-2"}, "HDMI-A-1"), QString("DP-2"));
    }
    void defaultUsesQtPrimary()
    {
        QCOMPARE(vela::primaryOutputName("", {"DP-2", "HDMI-A-1"}, "HDMI-A-1"), QString("HDMI-A-1"));
    }
    void unplugAndReplug()
    {
        const QString saved = "DP-2";
        QCOMPARE(vela::primaryOutputName(saved, {"HDMI-A-1", "DP-2"}, "HDMI-A-1"), saved);
        QCOMPARE(vela::primaryOutputName(saved, {"HDMI-A-1"}, "HDMI-A-1"), QString("HDMI-A-1"));
        QCOMPARE(vela::primaryOutputName(saved, {"HDMI-A-1", "DP-2"}, "HDMI-A-1"), saved);
    }
    void noOutputAndMissingQtPrimary()
    {
        QCOMPARE(vela::primaryOutputName("DP-2", {"HDMI-A-1"}, "DP-2"), QString("HDMI-A-1"));
        QCOMPARE(vela::primaryOutputName("", {"DP-3", "HDMI-A-1"}, ""), QString("DP-3"));
        QVERIFY(vela::primaryOutputName("DP-2", {}, "").isEmpty());
    }
};
QTEST_GUILESS_MAIN(PrimaryOutputTest)
#include "primaryoutput.moc"
