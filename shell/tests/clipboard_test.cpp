// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Clipboard history (src/clipboard.cpp) without a compositor: what goes into
// the list and when an entry moves back to the top instead, the images it
// keeps (a small preview and the PNG as it arrived) and what it hands the
// image provider, and the pinned entries that survive a restart in
// ~/.local/share/vela/clipboard, private and without stale files.

#include "clipboard.h"
#include "privatehome.h"
#include "shellcontroller.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <thread>

using namespace vela::test;

namespace {

// What the history asked the compositor (paste() sends "paste").
QList<QByteArray>& sentCommands()
{
    static QList<QByteArray> commands;
    return commands;
}

QString storageDir()
{
    return home().filePath(QStringLiteral("data/vela/clipboard"));
}

// Stripes four pixels wide, with a gradient: downscaling through the
// 544 × 192 preview blurs them, the full picture keeps them sharp.
QImage picture(const QSize& size, int seed = 0)
{
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const int stripe = (x / 4 + seed) % 2 ? 230 : 20;
            image.setPixel(x, y, qRgb(stripe, (y * 255) / size.height(), (x + seed * 37) % 256));
        }
    }
    return image;
}

QByteArray encode(const QImage& image, int quality = -1)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG", quality);
    return png;
}

QVariantMap entry(const Clipboard& clipboard, int index)
{
    return clipboard.items().value(index).toMap();
}

QImage rgb(const QImage& image)
{
    return image.convertToFormat(QImage::Format_RGB32);
}

} // namespace

// The shell's socket to the compositor: none here, the command is noted.
bool ShellController::sendToCompositor(const QByteArray& command)
{
    sentCommands().append(command);
    return true;
}

class ClipboardTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        QDir(storageDir()).removeRecursively();
        QSettings().setValue(QStringLiteral("clipboard/history"), true);
        sentCommands().clear();
    }

    void historyOffKeepsNothing()
    {
        QSettings().setValue(QStringLiteral("clipboard/history"), false);
        Clipboard clipboard;
        QVERIFY(!clipboard.enabled());
        clipboard.copyText(QStringLiteral("secret"));
        clipboard.copyImage(picture(QSize(8, 8)));
        QVERIFY(clipboard.items().isEmpty());
    }

    void turningHistoryOffForgetsAllButPinned()
    {
        Clipboard clipboard;
        QVERIFY(clipboard.enabled());
        clipboard.copyText(QStringLiteral("kept"));
        clipboard.togglePin(entry(clipboard, 0)[QStringLiteral("id")].toInt());
        clipboard.copyText(QStringLiteral("forgotten"));
        QSignalSpy enabled(&clipboard, &Clipboard::enabledChanged);

        clipboard.setEnabled(false);

        QCOMPARE(enabled.count(), 1);
        QCOMPARE(QSettings().value(QStringLiteral("clipboard/history")).toBool(), false);
        QCOMPARE(clipboard.items().size(), 1);
        QCOMPARE(entry(clipboard, 0)[QStringLiteral("text")].toString(), QStringLiteral("kept"));
        clipboard.setEnabled(false); // already off: nothing to tell
        QCOMPARE(enabled.count(), 1);
    }

    void copiedTextGoesBackToTheTop()
    {
        Clipboard clipboard;
        QSignalSpy changed(&clipboard, &Clipboard::itemsChanged);
        clipboard.copyText(QStringLiteral("first"));
        clipboard.copyText(QStringLiteral("second"));
        const int firstId = entry(clipboard, 1)[QStringLiteral("id")].toInt();

        clipboard.copyText(QStringLiteral("first"));

        QCOMPARE(changed.count(), 3);
        QCOMPARE(clipboard.items().size(), 2);
        QCOMPARE(entry(clipboard, 0)[QStringLiteral("text")].toString(), QStringLiteral("first"));
        QCOMPARE(entry(clipboard, 0)[QStringLiteral("kind")].toString(), QStringLiteral("text"));
        QCOMPARE(entry(clipboard, 0)[QStringLiteral("id")].toInt(), firstId);
    }

    void pinnedEntriesComeFirstAndStayPinned()
    {
        Clipboard clipboard;
        clipboard.copyText(QStringLiteral("old"));
        clipboard.togglePin(entry(clipboard, 0)[QStringLiteral("id")].toInt());
        clipboard.copyText(QStringLiteral("new"));
        QCOMPARE(entry(clipboard, 0)[QStringLiteral("text")].toString(), QStringLiteral("old"));
        QVERIFY(entry(clipboard, 0)[QStringLiteral("pinned")].toBool());

        // Copied again: still pinned, not a second entry.
        clipboard.copyText(QStringLiteral("old"));
        QCOMPARE(clipboard.items().size(), 2);
        QVERIFY(entry(clipboard, 0)[QStringLiteral("pinned")].toBool());
        QVERIFY(!entry(clipboard, 1)[QStringLiteral("pinned")].toBool());
    }

    void pasteMovesTheEntryUpAndAsksTheCompositor()
    {
        Clipboard clipboard;
        clipboard.copyText(QStringLiteral("older"));
        clipboard.copyText(QStringLiteral("newer"));
        const int olderId = entry(clipboard, 1)[QStringLiteral("id")].toInt();

        clipboard.paste(olderId);

        QCOMPARE(sentCommands(), QList<QByteArray> { "paste" });
        QCOMPARE(clipboard.items().size(), 2);
        QCOMPARE(entry(clipboard, 0)[QStringLiteral("id")].toInt(), olderId);

        clipboard.paste(9999); // gone: nothing to paste
        QCOMPARE(sentCommands().size(), 1);
    }

    void removeAndClear()
    {
        Clipboard clipboard;
        clipboard.copyText(QStringLiteral("a"));
        clipboard.copyText(QStringLiteral("b"));
        clipboard.copyText(QStringLiteral("c"));
        clipboard.togglePin(entry(clipboard, 2)[QStringLiteral("id")].toInt()); // "a"
        QSignalSpy changed(&clipboard, &Clipboard::itemsChanged);

        clipboard.remove(entry(clipboard, 1)[QStringLiteral("id")].toInt()); // "c"
        QCOMPARE(changed.count(), 1);
        QCOMPARE(clipboard.items().size(), 2);
        clipboard.remove(9999);
        clipboard.togglePin(9999);
        QCOMPARE(changed.count(), 1);

        clipboard.clear();
        QCOMPARE(clipboard.items().size(), 1);
        QCOMPARE(entry(clipboard, 0)[QStringLiteral("text")].toString(), QStringLiteral("a"));
    }

    void nullImageIsRefused()
    {
        Clipboard clipboard;
        clipboard.copyImage(QImage());
        QVERIFY(clipboard.items().isEmpty());
        QVERIFY(clipboard.image(1, QSize()).isNull());
    }

    void imagesAreServedAtTheSizeAsked()
    {
        Clipboard clipboard;
        const QImage original = picture(QSize(2000, 1000));
        clipboard.copyImage(original);
        QCOMPARE(entry(clipboard, 0)[QStringLiteral("kind")].toString(), QStringLiteral("image"));
        const int id = entry(clipboard, 0)[QStringLiteral("id")].toInt();

        // No size asked: the whole picture.
        QCOMPARE(rgb(clipboard.image(id, QSize())), original);
        // The card: from the preview, with the picture's proportions.
        QCOMPARE(clipboard.image(id, QSize(272, 96)).size(), QSize(192, 96));
        // Larger than the preview: from the PNG, sharp. Scaling the preview
        // up would blur the stripes.
        const QImage large = clipboard.image(id, QSize(1000, 1000));
        QCOMPARE(large.size(), QSize(1000, 500));
        QCOMPARE(rgb(large), original.scaled(QSize(1000, 500), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        QVERIFY(clipboard.image(id + 1, QSize(10, 10)).isNull());

        // A small picture is its own preview, and a request never ends at 0.
        const QImage small = picture(QSize(100, 3), 1);
        clipboard.copyImage(small);
        const int smallId = entry(clipboard, 0)[QStringLiteral("id")].toInt();
        QCOMPARE(rgb(clipboard.image(smallId, QSize())), small);
        QCOMPARE(clipboard.image(smallId, QSize(10, 10)).size(), QSize(10, 1));
    }

    void imageProviderAnswersWithTheSize()
    {
        Clipboard clipboard;
        clipboard.copyImage(picture(QSize(600, 300)));
        const int id = entry(clipboard, 0)[QStringLiteral("id")].toInt();
        ClipboardImageProvider provider(&clipboard);
        QSize size;
        // QML adds a suffix to reload an entry: "<id>/<anything>".
        const QImage image = provider.requestImage(QString::number(id) + QStringLiteral("/2"), &size, QSize(100, 100));
        QCOMPARE(image.size(), QSize(100, 50));
        QCOMPARE(size, QSize(100, 50));
        QVERIFY(provider.requestImage(QStringLiteral("9999"), nullptr, QSize()).isNull()); // gone, no size to write
    }

    void samePictureIsOneEntry()
    {
        Clipboard clipboard;
        const QImage image = picture(QSize(1200, 600));
        clipboard.copyImage(image);
        const int id = entry(clipboard, 0)[QStringLiteral("id")].toInt();
        clipboard.copyText(QStringLiteral("between"));

        clipboard.copyImage(image);

        QCOMPARE(clipboard.items().size(), 2);
        QCOMPARE(entry(clipboard, 0)[QStringLiteral("id")].toInt(), id);
    }

    void picturesWithTheSamePreviewStayApart()
    {
        // One pixel apart: the 544 × 192 previews match, the pictures don't.
        const QImage first = picture(QSize(2200, 1100));
        QImage second = first;
        const QRgb pixel = second.pixel(1001, 501);
        second.setPixel(1001, 501, qRgb(qRed(pixel), qGreen(pixel), qBlue(pixel) ^ 1));
        const QSize bounds(544, 192);
        QCOMPARE(first.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation),
            second.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation));

        Clipboard clipboard;
        clipboard.copyImage(first);
        clipboard.copyImage(second);

        QCOMPARE(clipboard.items().size(), 2);
    }

    void samePixelsInAnotherEncodingAreOneEntry()
    {
        // A pinned PNG saved without compression; copying the same pixels
        // encodes them again, with Qt's default compression.
        const QImage image = picture(QSize(300, 200));
        const QByteArray stored = encode(image, 100);
        QVERIFY(stored != encode(image));
        QVERIFY(QDir().mkpath(storageDir()));
        QFile png(storageDir() + QStringLiteral("/stored.png"));
        QVERIFY(png.open(QIODevice::WriteOnly));
        png.write(stored);
        png.close();
        writePinnedJson(QJsonArray { QJsonObject { { QStringLiteral("image"), QStringLiteral("stored.png") } } });

        Clipboard clipboard;
        QCOMPARE(clipboard.items().size(), 1);
        const int id = entry(clipboard, 0)[QStringLiteral("id")].toInt();
        clipboard.copyText(QStringLiteral("text"));

        clipboard.copyImage(image);

        QCOMPARE(clipboard.items().size(), 2);
        QCOMPARE(entry(clipboard, 0)[QStringLiteral("id")].toInt(), id);
        QVERIFY(entry(clipboard, 0)[QStringLiteral("pinned")].toBool());
    }

    void pinnedEntriesSurviveARestart()
    {
        const QImage image = picture(QSize(900, 450));
        QString pngName;
        {
            Clipboard clipboard;
            clipboard.copyImage(image);
            clipboard.copyText(QStringLiteral("pinned text"));
            clipboard.copyText(QStringLiteral("session only"));
            // A file left by the old numbered names: it goes on the next save.
            QFile stale(storageDir() + QStringLiteral("/1.png"));
            QVERIFY(QDir().mkpath(storageDir()));
            QVERIFY(stale.open(QIODevice::WriteOnly));
            stale.close();

            clipboard.togglePin(entry(clipboard, 2)[QStringLiteral("id")].toInt()); // the image
            clipboard.togglePin(entry(clipboard, 2)[QStringLiteral("id")].toInt()); // "pinned text"

            const QStringList files = QDir(storageDir()).entryList({ QStringLiteral("*.png") }, QDir::Files);
            QCOMPARE(files.size(), 1);
            pngName = files.first();
            QFile png(storageDir() + u'/' + pngName);
            QVERIFY(png.open(QIODevice::ReadOnly));
            // Named after its bytes, so an unchanged image is never rewritten.
            QCOMPARE(pngName,
                QString::fromLatin1(QCryptographicHash::hash(png.readAll(), QCryptographicHash::Sha256).toHex())
                    + QStringLiteral(".png"));
            for (const QString& name : { pngName, QStringLiteral("pinned.json") }) {
                QCOMPARE(QFile::permissions(storageDir() + u'/' + name),
                    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser);
            }
        }

        // Back in the order they had: the history's, most recent first.
        Clipboard clipboard;
        QCOMPARE(clipboard.items().size(), 2);
        QCOMPARE(entry(clipboard, 0)[QStringLiteral("text")].toString(), QStringLiteral("pinned text"));
        QCOMPARE(entry(clipboard, 1)[QStringLiteral("kind")].toString(), QStringLiteral("image"));
        QVERIFY(entry(clipboard, 0)[QStringLiteral("pinned")].toBool());
        QVERIFY(entry(clipboard, 1)[QStringLiteral("pinned")].toBool());
        const int imageId = entry(clipboard, 1)[QStringLiteral("id")].toInt();
        QCOMPARE(rgb(clipboard.image(imageId, QSize())), image);

        // Unpinned: its file goes, the text stays saved.
        clipboard.togglePin(imageId);
        QVERIFY(QDir(storageDir()).entryList({ QStringLiteral("*.png") }, QDir::Files).isEmpty());
        QCOMPARE(readPinnedJson(),
            (QJsonArray { QJsonObject { { QStringLiteral("text"), QStringLiteral("pinned text") } } }));
    }

    void unreadablePinnedImagesAreSkipped()
    {
        QVERIFY(QDir().mkpath(storageDir()));
        QFile broken(storageDir() + QStringLiteral("/broken.png"));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("not a png");
        broken.close();
        writePinnedJson(QJsonArray { QJsonObject { { QStringLiteral("image"), QStringLiteral("missing.png") } },
            QJsonObject { { QStringLiteral("image"), QStringLiteral("broken.png") } },
            QJsonObject { { QStringLiteral("text"), QStringLiteral("survivor") } } });

        Clipboard clipboard;
        QCOMPARE(clipboard.items().size(), 1);
        QCOMPARE(entry(clipboard, 0)[QStringLiteral("text")].toString(), QStringLiteral("survivor"));
    }

    void imageReadsWhileTheHistoryChanges()
    {
        // The image provider reads from QML's loader threads while the
        // history changes in the main thread (ASan and TSan check the rest).
        Clipboard clipboard;
        clipboard.copyImage(picture(QSize(700, 350)));
        const int id = entry(clipboard, 0)[QStringLiteral("id")].toInt();
        clipboard.togglePin(id); // past the history's limit too
        std::atomic<bool> stop { false };
        std::atomic<int> reads { 0 };
        std::thread reader([&] {
            while (!stop) {
                (void)clipboard.image(id, QSize(64, 64));
                ++reads;
            }
        });
        for (int i = 0; i < 200; ++i) {
            clipboard.copyText(QString::number(i));
            clipboard.copyImage(picture(QSize(32, 32), i));
            clipboard.remove(entry(clipboard, 1)[QStringLiteral("id")].toInt()); // after the pinned one
        }
        stop = true;
        reader.join();
        QVERIFY(reads > 0);
        QVERIFY(!clipboard.image(id, QSize(64, 64)).isNull());
    }

private:
    static void writePinnedJson(const QJsonArray& list)
    {
        QVERIFY(QDir().mkpath(storageDir()));
        QFile file(storageDir() + QStringLiteral("/pinned.json"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(list).toJson());
    }

    static QJsonArray readPinnedJson()
    {
        QFile file(storageDir() + QStringLiteral("/pinned.json"));
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).array();
    }
};

int main(int argc, char** argv)
{
    if (!isolate()) {
        return 1;
    }
    QGuiApplication app(argc, argv); // offscreen: no Wayland, the history alone
    QGuiApplication::setApplicationName(QStringLiteral("vela-shell"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    ClipboardTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "clipboard_test.moc"
