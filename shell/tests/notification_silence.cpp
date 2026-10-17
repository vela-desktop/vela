// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Do not disturb: the user's own choice OR the screen being shared. Popups
// are held back (the notification center keeps them) and the user's choice
// is never overwritten.

#include "notifications.h"

#include <QSettings>
#include <QStandardPaths>
#include <QTest>

class NotificationSilenceTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void sharingSilencesAndRestores()
    {
        NotificationServer s;
        s.Notify("a", 0, "", "one", "", {}, {}, 5000);
        QCOMPARE(s.count(), 1); // a popup

        s.setCaptureSilenced(true);
        QVERIFY(s.silenced());
        QVERIFY(!s.doNotDisturb());
        s.Notify("a", 0, "", "two", "", {}, {}, 5000);
        QCOMPARE(s.count(), 1); // held back, in the center
        QCOMPARE(s.history()->property("count").toInt(), 1);

        s.setCaptureSilenced(false);
        QVERIFY(!s.silenced());
        s.Notify("a", 0, "", "three", "", {}, {}, 5000);
        QCOMPARE(s.count(), 2);
    }
    void manualChoiceSurvivesSharing()
    {
        NotificationServer s;
        s.setDoNotDisturb(true);
        s.setCaptureSilenced(true);
        s.setCaptureSilenced(false);
        QVERIFY(s.doNotDisturb());
        QVERIFY(s.silenced());
        s.setDoNotDisturb(false);
        QVERIFY(!s.silenced());
    }
};

QTEST_GUILESS_MAIN(NotificationSilenceTest)
#include "notification_silence.moc"
