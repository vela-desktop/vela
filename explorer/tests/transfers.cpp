// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests of Explorer's copies and moves, without a UI: the cases where a
// file manager must never lose data. Each test works in a temporary directory
// of its own.
//
//   ctest --test-dir build -R files-copies   (or build/explorer/vela-files-test)

#include "appmodel.h"
#include "fileops.h"
#include "systemactions.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>

namespace {

std::atomic<int> exchangeError { 0 };
int pauseExchange = 0; // child process only: 1 before exchange, 2 after

void pauseAtExchange(int phase)
{
    if (pauseExchange == phase) {
        constexpr char marker[] = "exchange-boundary\n";
        ::write(STDOUT_FILENO, marker, sizeof(marker) - 1);
        ::raise(SIGSTOP);
    }
}

} // namespace

extern "C" int __real_renameat2(int oldfd, const char* from, int newfd, const char* to, unsigned int flags);

extern "C" int __wrap_renameat2(int oldfd, const char* from, int newfd, const char* to, unsigned int flags)
{
    if (flags == RENAME_EXCHANGE) {
        if (const int failure = exchangeError.load()) {
            errno = failure;
            return -1;
        }
        pauseAtExchange(1);
    }
    const int result = __real_renameat2(oldfd, from, newfd, to, flags);
    if (flags == RENAME_EXCHANGE && result == 0) {
        pauseAtExchange(2);
    }
    return result;
}

namespace {

void write(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(data), data.size());
}

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<illeggibile>");
}

// The copy temporaries (".name.vela-copy-XXXXXX") left under `dir`.
QStringList leftovers(const QString& dir)
{
    QStringList found;
    QDirIterator it(dir, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileName().contains(QLatin1String(".vela-copy-"))) {
            found.append(it.filePath());
        }
    }
    return found;
}

} // namespace

class Transfers : public QObject {
    Q_OBJECT

    AppModel m_apps;
    FileOps* m_ops = nullptr;
    QTemporaryDir* m_dir = nullptr;
    QString m_policy = QStringLiteral("replace"); // the answer to conflicts

    QString path(const QString& relative) const { return m_dir->filePath(relative); }

    // Drags `sources` into `directory` (2 = move, 1 = copy) and waits for the
    // job to finish. Returns the job's error (empty if it went fine).
    QString transfer(const QStringList& sources, const QString& directory, int action)
    {
        QStringList urls;
        for (const QString& source : sources) {
            urls.append(QUrl::fromLocalFile(source).toString());
        }
        const int before = int(m_ops->jobs().size());
        m_ops->drop(urls, directory, action);
        QString error = QStringLiteral("<no job>");
        const bool finished = QTest::qWaitFor(
            [&] {
                const QVariantList jobs = m_ops->jobs();
                if (jobs.size() <= before) {
                    return false;
                }
                const QVariantMap job = jobs.last().toMap();
                if (!job.value(QStringLiteral("finished")).toBool()) {
                    return false;
                }
                error = job.value(QStringLiteral("error")).toString();
                return true;
            },
            30000);
        return finished ? error : QStringLiteral("<tempo scaduto>");
    }

private slots:
    void init()
    {
        m_dir = new QTemporaryDir;
        QVERIFY(m_dir->isValid());
        m_ops = new FileOps(&m_apps);
        m_policy = QStringLiteral("replace");
        connect(m_ops, &FileOps::conflict, this, [this] { m_ops->resolveConflict(m_policy); });
    }

    void cleanup()
    {
        delete m_ops;
        delete m_dir;
    }

    void copyNewFile()
    {
        write(path("a/photo.jpg"), "content");
        QDir().mkpath(path("b"));
        QCOMPARE(transfer({ path("a/photo.jpg") }, path("b"), 1), QString());
        QCOMPARE(read(path("b/photo.jpg")), QByteArray("content"));
        QCOMPARE(read(path("a/photo.jpg")), QByteArray("content"));
        QVERIFY(leftovers(m_dir->path()).isEmpty());
    }

    void replaceKeepsMetadata()
    {
        write(path("a/doc.txt"), "new");
        write(path("b/doc.txt"), "old");
        const QDateTime when = QDateTime::fromSecsSinceEpoch(1700000000);
        QFile source(path("a/doc.txt"));
        QVERIFY(source.open(QIODevice::ReadWrite));
        QVERIFY(source.setFileTime(when, QFileDevice::FileModificationTime));
        source.close();
        QFile::setPermissions(path("a/doc.txt"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        QCOMPARE(transfer({ path("a/doc.txt") }, path("b"), 1), QString());
        QCOMPARE(read(path("b/doc.txt")), QByteArray("new"));
        QCOMPARE(QFileInfo(path("b/doc.txt")).lastModified(), when);
        QVERIFY(QFileInfo(path("b/doc.txt")).permissions() & QFile::ExeOwner);
        QVERIFY(leftovers(m_dir->path()).isEmpty());
    }

    // The case from the review: "Replace" with the disk filling up halfway
    // through the copy. The old file must stay intact and the temporary
    // disappear.
    void diskFullKeepsOldFile()
    {
        write(path("a/big.bin"), QByteArray(3 << 20, 'n'));
        write(path("b/big.bin"), "old");
        // Above 1 MB writes fail (EFBIG), as with a full disk.
        rlimit old {};
        getrlimit(RLIMIT_FSIZE, &old);
        rlimit small = old;
        small.rlim_cur = 1 << 20;
        signal(SIGXFSZ, SIG_IGN);
        QCOMPARE(setrlimit(RLIMIT_FSIZE, &small), 0);
        const QString error = transfer({ path("a/big.bin") }, path("b"), 1);
        setrlimit(RLIMIT_FSIZE, &old);

        QVERIFY2(!error.isEmpty(), "the copy should have failed");
        QCOMPARE(read(path("b/big.bin")), QByteArray("old"));
        QVERIFY(leftovers(m_dir->path()).isEmpty());
    }

    // A new folder that can't be copied entirely: no half tree in the
    // destination.
    void failedFolderLeavesNothing()
    {
        write(path("a/folder/small.txt"), "ok");
        write(path("a/folder/sub/big.bin"), QByteArray(3 << 20, 'g'));
        QDir().mkpath(path("b"));
        rlimit old {};
        getrlimit(RLIMIT_FSIZE, &old);
        rlimit small = old;
        small.rlim_cur = 1 << 20;
        signal(SIGXFSZ, SIG_IGN);
        QCOMPARE(setrlimit(RLIMIT_FSIZE, &small), 0);
        const QString error = transfer({ path("a/folder") }, path("b"), 1);
        setrlimit(RLIMIT_FSIZE, &old);

        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(path("b/folder")));
        QVERIFY(leftovers(m_dir->path()).isEmpty());
    }

    void recursiveTransferWorkCountsDirectoriesAndLinks()
    {
        write(path("tree/file.bin"), QByteArray(7, 'a'));
        QDir().mkpath(path("tree/empty/child/grandchild"));
        QVERIFY(QFile::link(QStringLiteral("file.bin"), path("tree/link")));
        // Four directories (including the root), one link, seven bytes.
        QCOMPARE(transferWorkUnits(path("tree")), qint64(12));
        QCOMPARE(transferWorkUnits(path("tree/link")), qint64(1));
        QDir().mkpath(path("out"));
        QCOMPARE(transfer({ path("tree") }, path("out"), 1), QString());
        QCOMPARE(read(path("out/tree/file.bin")), QByteArray(7, 'a'));
    }

    void copyFolderTree()
    {
        write(path("a/project/one.txt"), "1");
        write(path("a/project/sub/two.txt"), "2");
        QVERIFY(QFile::link(QStringLiteral("one.txt"), path("a/project/link")));
        QDir().mkpath(path("b"));
        QCOMPARE(transfer({ path("a/project") }, path("b"), 1), QString());
        QCOMPARE(read(path("b/project/one.txt")), QByteArray("1"));
        QCOMPARE(read(path("b/project/sub/two.txt")), QByteArray("2"));
        QCOMPARE(QFileInfo(path("b/project/link")).symLinkTarget(), path("b/project/one.txt"));
        QVERIFY(leftovers(m_dir->path()).isEmpty());
    }

    void mergeFolders()
    {
        write(path("a/photo/new.jpg"), "new");
        write(path("a/photo/common.jpg"), "updated");
        write(path("b/photo/old.jpg"), "old");
        write(path("b/photo/common.jpg"), "original");
        QCOMPARE(transfer({ path("a/photo") }, path("b"), 1), QString());
        QCOMPARE(read(path("b/photo/new.jpg")), QByteArray("new"));
        QCOMPARE(read(path("b/photo/old.jpg")), QByteArray("old"));
        QCOMPARE(read(path("b/photo/common.jpg")), QByteArray("updated"));
        QVERIFY(leftovers(m_dir->path()).isEmpty());
    }

    void fileReplacesFolder_data()
    {
        QTest::addColumn<int>("action");
        QTest::newRow("copy") << 1;
        QTest::newRow("move") << 2;
    }

    void fileReplacesFolder()
    {
        QFETCH(int, action);
        write(path("a/item"), "file");
        write(path("b/item/inside.txt"), "folder");
        QCOMPARE(transfer({ path("a/item") }, path("b"), action), QString());
        QVERIFY(QFileInfo(path("b/item")).isFile());
        QCOMPARE(read(path("b/item")), QByteArray("file"));
        QCOMPARE(QFileInfo::exists(path("a/item")), action == 1);
        QVERIFY(leftovers(m_dir->path()).isEmpty());
    }

    void folderReplacesFile_data() { fileReplacesFolder_data(); }

    void folderReplacesFile()
    {
        QFETCH(int, action);
        write(path("a/item/inside.txt"), "folder");
        write(path("b/item"), "file");
        QCOMPARE(transfer({ path("a/item") }, path("b"), action), QString());
        QVERIFY(QFileInfo(path("b/item")).isDir());
        QCOMPARE(read(path("b/item/inside.txt")), QByteArray("folder"));
        QCOMPARE(QFileInfo::exists(path("a/item")), action == 1);
        QVERIFY(leftovers(m_dir->path()).isEmpty());
    }

    void linkReplacesFolder()
    {
        write(path("a/target.txt"), "target");
        QVERIFY(QFile::link(QStringLiteral("target.txt"), path("a/item")));
        write(path("b/item/inside.txt"), "old");
        write(path("b/target.txt"), "keep");
        QCOMPARE(transfer({ path("a/item") }, path("b"), 1), QString());
        QVERIFY(QFileInfo(path("b/item")).isSymLink());
        QCOMPARE(QFileInfo(path("b/item")).symLinkTarget(), path("b/target.txt"));
        QCOMPARE(read(path("b/target.txt")), QByteArray("keep"));
        QVERIFY(leftovers(m_dir->path()).isEmpty());
    }

    void folderReplacesLinkToFolder()
    {
        write(path("a/item/inside.txt"), "new");
        write(path("outside/keep.txt"), "keep");
        QDir().mkpath(path("b"));
        QVERIFY(QFile::link(path("outside"), path("b/item")));
        QCOMPARE(transfer({ path("a/item") }, path("b"), 1), QString());
        QVERIFY(!QFileInfo(path("b/item")).isSymLink());
        QCOMPARE(read(path("b/item/inside.txt")), QByteArray("new"));
        QCOMPARE(read(path("outside/keep.txt")), QByteArray("keep"));
        QVERIFY(leftovers(m_dir->path()).isEmpty());
    }

    void failedExchangeKeepsBothItems_data()
    {
        QTest::addColumn<bool>("sourceIsDir");
        QTest::addColumn<int>("action");
        QTest::addColumn<int>("failure");
        for (bool sourceIsDir : { false, true }) {
            for (int action : { 1, 2 }) {
                for (int failure : { ENOSYS, EINVAL, EACCES }) {
                    const QByteArray name = QByteArray::number(sourceIsDir) + '-'
                        + QByteArray::number(action) + '-' + QByteArray::number(failure);
                    QTest::newRow(name.constData()) << sourceIsDir << action << failure;
                }
            }
        }
    }

    void failedExchangeKeepsBothItems()
    {
        QFETCH(bool, sourceIsDir);
        QFETCH(int, action);
        QFETCH(int, failure);
        const QString source = path("a/item");
        const QString destination = path("b/item");
        write(sourceIsDir ? source + "/inside.txt" : source, "new");
        write(sourceIsDir ? destination : destination + "/inside.txt", "old");

        exchangeError = failure;
        const QString error = transfer({ source }, path("b"), action);
        exchangeError = 0;

        QVERIFY2(!error.isEmpty(), "a failed exchange must be reported");
        QCOMPARE(read(sourceIsDir ? source + "/inside.txt" : source), QByteArray("new"));
        QCOMPARE(read(sourceIsDir ? destination : destination + "/inside.txt"), QByteArray("old"));
        QCOMPARE(QFileInfo(destination).isDir(), !sourceIsDir);
        QVERIFY(leftovers(m_dir->path()).isEmpty());
    }

    void interruptedReplacementKeepsBothItems_data()
    {
        QTest::addColumn<bool>("sourceIsDir");
        QTest::addColumn<int>("action");
        QTest::addColumn<int>("phase");
        for (bool sourceIsDir : { false, true }) {
            for (int action : { 1, 2 }) {
                for (int phase : { 1, 2 }) {
                    const QByteArray name = QByteArray::number(sourceIsDir) + '-'
                        + QByteArray::number(action) + '-' + QByteArray::number(phase);
                    QTest::newRow(name.constData()) << sourceIsDir << action << phase;
                }
            }
        }
    }

    void interruptedReplacementKeepsBothItems()
    {
        QFETCH(bool, sourceIsDir);
        QFETCH(int, action);
        QFETCH(int, phase);
        const QString source = path("a/item");
        const QString destination = path("b/item");
        write(sourceIsDir ? source + "/inside.txt" : source, "new");
        write(sourceIsDir ? destination : destination + "/inside.txt", "old");

        // Stop a separate Explorer worker at the actual syscall boundary,
        // then kill it: neither Qt cleanup nor error recovery can run.
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),
            { QStringLiteral("--replacement-child"), QString::number(phase), source, path("b"), QString::number(action) });
        QVERIFY(child.waitForStarted());
        QByteArray output;
        const bool stopped = QTest::qWaitFor([&] {
            output += child.readAllStandardOutput();
            return output.contains("exchange-boundary\n") || child.state() == QProcess::NotRunning;
        }, 30000);
        child.kill();
        QVERIFY(child.waitForFinished());
        QVERIFY2(stopped && output.contains("exchange-boundary\n"), child.readAllStandardError().constData());
        QCOMPARE(child.exitStatus(), QProcess::CrashExit);

        QCOMPARE(read(sourceIsDir ? source + "/inside.txt" : source), QByteArray("new"));
        const bool committed = phase == 2;
        QCOMPARE(QFileInfo(destination).isDir(), committed ? sourceIsDir : !sourceIsDir);
        QCOMPARE(read(QFileInfo(destination).isDir() ? destination + "/inside.txt" : destination),
            committed ? QByteArray("new") : QByteArray("old"));
        const QStringList staged = leftovers(m_dir->path());
        QCOMPARE(staged.size(), 1);
        const QString retained = staged.first();
        QCOMPARE(QFileInfo(retained).isDir(), committed ? !sourceIsDir : sourceIsDir);
        QCOMPARE(read(QFileInfo(retained).isDir() ? retained + "/inside.txt" : retained),
            committed ? QByteArray("old") : QByteArray("new"));
    }

    void failedOldItemRemovalIsReported()
    {
        if (::geteuid() == 0) {
            QSKIP("permissions do not prevent root from removing files");
        }
        write(path("a/item"), "new");
        write(path("b/item/inside.txt"), "old");
        QVERIFY(QFile::setPermissions(path("b/item"), QFile::ReadOwner | QFile::ExeOwner));

        const QString error = transfer({ path("a/item") }, path("b"), 2);
        const QStringList staged = leftovers(m_dir->path());
        // Restore permissions before assertions so even a failure cleans up.
        for (const QString& item : staged) {
            QFile::setPermissions(item, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        if (QFileInfo(path("b/item")).isDir()) {
            QFile::setPermissions(path("b/item"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }

        QVERIFY(!error.isEmpty());
        QCOMPARE(read(path("b/item")), QByteArray("new"));
        QCOMPARE(read(path("a/item")), QByteArray("new")); // do not delete the source on an error
        QCOMPARE(staged.size(), 1);
        QVERIFY(error.contains(staged.first()));
        QCOMPARE(read(staged.first() + "/inside.txt"), QByteArray("old"));
    }

    void failedSourceRemovalIsReported()
    {
        if (::geteuid() == 0) {
            QSKIP("permissions do not prevent root from removing files");
        }
        // Different types force the copy path even on the same filesystem.
        write(path("a/item"), "new");
        write(path("b/item/inside.txt"), "old");
        QVERIFY(QFile::setPermissions(path("a"), QFile::ReadOwner | QFile::ExeOwner));
        const QString error = transfer({ path("a/item") }, path("b"), 2);
        QFile::setPermissions(path("a"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        QVERIFY(!error.isEmpty());
        QVERIFY(error.contains(path("a/item")));
        QCOMPARE(read(path("b/item")), QByteArray("new"));
        QCOMPARE(read(path("a/item")), QByteArray("new"));
        QVERIFY(leftovers(m_dir->path()).isEmpty());
        QVERIFY(m_ops->undoText().isEmpty());
    }

    void keepBoth()
    {
        m_policy = QStringLiteral("keep");
        write(path("a/note.txt"), "new");
        write(path("b/note.txt"), "old");
        QCOMPARE(transfer({ path("a/note.txt") }, path("b"), 1), QString());
        QCOMPARE(read(path("b/note.txt")), QByteArray("old"));
        QCOMPARE(read(path("b/note (2).txt")), QByteArray("new"));
    }

    void skipExisting()
    {
        m_policy = QStringLiteral("skip");
        write(path("a/note.txt"), "new");
        write(path("b/note.txt"), "old");
        QCOMPARE(transfer({ path("a/note.txt") }, path("b"), 1), QString());
        QCOMPARE(read(path("b/note.txt")), QByteArray("old"));
    }

    void moveReplacesFile()
    {
        write(path("a/note.txt"), "new");
        write(path("b/note.txt"), "old");
        QCOMPARE(transfer({ path("a/note.txt") }, path("b"), 2), QString());
        QCOMPARE(read(path("b/note.txt")), QByteArray("new"));
        QVERIFY(!QFileInfo::exists(path("a/note.txt")));
    }

    void copyIntoSameFolder()
    {
        write(path("a/photo.jpg"), "x");
        QCOMPARE(transfer({ path("a/photo.jpg") }, path("a"), 1), QString());
        QCOMPARE(read(path("a/photo - Copy.jpg")), QByteArray("x"));
    }

    void emptyTrashRemovesFilesAndMetadata()
    {
        const QString trash = path("fake-trash");
        write(trash + "/files/file.txt", "old");
        write(trash + "/files/folder/nested.txt", "old");
        write(trash + "/info/file.txt.trashinfo", "[Trash Info]\\n");
        write(trash + "/info/folder.trashinfo", "[Trash Info]\\n");
        write(trash + "/info/orphan.trashinfo", "[Trash Info]\\n");
        write(path("outside.txt"), "keep");
        QVERIFY(QFile::link(path("outside.txt"), trash + "/files/link"));

        QVERIFY(vela::trash::empty(trash).isEmpty());
        QCOMPARE(read(path("outside.txt")), QByteArray("keep"));
        QVERIFY(QDir(trash + "/files").isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden));
        QVERIFY(QDir(trash + "/info").isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden));
        QVERIFY(vela::trash::empty(trash).isEmpty());
    }
};

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (argc == 6 && QByteArray(argv[1]) == "--replacement-child") {
        pauseExchange = QByteArray(argv[2]).toInt();
        AppModel apps;
        FileOps ops(&apps);
        QObject::connect(&ops, &FileOps::conflict, &ops, [&] { ops.resolveConflict(QStringLiteral("replace")); });
        ops.drop({ QUrl::fromLocalFile(QString::fromLocal8Bit(argv[3])).toString() },
            QString::fromLocal8Bit(argv[4]), QByteArray(argv[5]).toInt());
        // A successful test kills this process at the exchange. Exiting here
        // means that copying failed or never reached the requested boundary.
        const bool finished = QTest::qWaitFor([&] {
            const QVariantList jobs = ops.jobs();
            return !jobs.isEmpty() && jobs.last().toMap().value(QStringLiteral("finished")).toBool();
        }, 30000);
        return finished ? 1 : 2;
    }
    Transfers test;
    return QTest::qExec(&test, argc, argv);
}

#include "transfers.moc"
