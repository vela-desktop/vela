// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QFile>
#include <QTimer>
#include <fontconfig/fontconfig.h>
#include <xcb/xcb.h>
#include <cstdio>
#include <ctime>
#include <unistd.h>
#ifdef CORPUS_WEB
#include <QWebEngineView>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestInfo>
#include <QtWebEngineCore/qtwebenginecoreglobal.h>
#include <sched.h>
#include <sys/wait.h>
#endif
#include "vela-bench-corpus-content.h"

static QString caseId;
static int family;
static QJsonObject description;
static QByteArray previous;
static unsigned sequence;
static xcb_connection_t *connection;
static quint64 now()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return quint64(t.tv_sec) * 1000000000ull + t.tv_nsec;
}
static QString digest(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()); }
static void output(QJsonObject value)
{
    const QByteArray bytes=QJsonDocument(value).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(),1,bytes.size(),stdout);std::putchar('\n');std::fflush(stdout);
}
static QJsonObject describe()
{
    QJsonObject value{{"schema",1},{"method",CORPUS_METHOD},{"content_sha256",digest(CORPUS_CONTENT)},
        {"toolkit","Qt"},{"toolkit_version",qVersion()},{"font_requested",CORPUS_FONT},{"font_size_px",16}};
    FcPattern *pattern=FcPatternCreate();
    FcPatternAddString(pattern,FC_FAMILY,reinterpret_cast<const FcChar8 *>(CORPUS_FONT));
    FcPatternAddInteger(pattern,FC_WEIGHT,FC_WEIGHT_REGULAR);
    FcConfigSubstitute(nullptr,pattern,FcMatchPattern);FcDefaultSubstitute(pattern);
    FcResult result;FcPattern *match=FcFontMatch(nullptr,pattern,&result);
    FcChar8 *file=nullptr,*name=nullptr;
    if (match && FcPatternGetString(match,FC_FILE,0,&file)==FcResultMatch
        && FcPatternGetString(match,FC_FAMILY,0,&name)==FcResultMatch) {
        QFile font(QString::fromUtf8(reinterpret_cast<const char *>(file)));
        if (font.open(QIODevice::ReadOnly) && font.size()<=32*1024*1024) {
            value["font_file"]=font.fileName();value["font_sha256"]=digest(font.readAll());
            value["font_resolved"]=QString::fromUtf8(reinterpret_cast<const char *>(name));
        }
    }
    if (match)FcPatternDestroy(match);
    FcPatternDestroy(pattern);
#ifdef CORPUS_WEB
    value["cases"]=QJsonArray{"browser-wayland"};value["engine_version"]=qWebEngineVersion();
    value["chromium_version"]=qWebEngineChromiumVersion();
#else
    value["cases"]=QJsonArray{"qt-wayland","qt-xwayland"};
#endif
    return value;
}
static QJsonObject metadata(QWidget *window,int w,int h)
{
    QJsonObject value=description;
    value["event"]="corpus-state";value["case"]=caseId;value["uid"]=qint64(getuid());value["pid"]=qint64(getpid());
    value["backend"]=QGuiApplication::platformName();
    value["protocol"]=QGuiApplication::platformName()=="xcb" ? "x11" : "wayland";
    value["canvas_width"]=w;value["canvas_height"]=h;
    value["window_width"]=window->width();value["window_height"]=window->height();
    value["maximized"]=window->isMaximized();value["reported_scale"]=window->devicePixelRatioF();
    value["font_actual_family"]=QFontInfo(window->font()).family();
    if (connection) {
        const xcb_setup_t *setup=xcb_get_setup(connection);
        xcb_screen_t *screen=xcb_setup_roots_iterator(setup).data;
        auto *root=xcb_get_geometry_reply(connection,xcb_get_geometry(connection,screen->root),nullptr);
        auto *client=xcb_get_geometry_reply(connection,xcb_get_geometry(connection,xcb_window_t(window->winId())),nullptr);
        if (root && client) {
            value["x11_root_width"]=root->width;value["x11_root_height"]=root->height;
            value["x11_width"]=client->width;value["x11_height"]=client->height;
            value["x11_window_id"]=QString::number(window->winId());
        }
        free(root);free(client);
    }
    return value;
}
static void publish(QJsonObject value)
{
    QByteArray identity=QJsonDocument(value).toJson(QJsonDocument::Compact);
    if (identity==previous)return;
    previous=identity;value["sequence"]=qint64(++sequence);value["monotonic_ns"]=QString::number(now());
    output(value);
}
#ifndef CORPUS_WEB
class Corpus : public QWidget {
public:
    Corpus() {
        setAutoFillBackground(true);resize(CORPUS_WIDTH,CORPUS_HEIGHT);
        auto *layout=new QVBoxLayout(this);layout->setContentsMargins(32,32,32,32);layout->setSpacing(6);
        layout->addWidget(new QLabel("Local scaling corpus"));
        auto *row=new QHBoxLayout;
        auto *entry=new QLineEdit("0123456789 AaBb CcDd");entry->setReadOnly(true);row->addWidget(entry);
        auto *check=new QCheckBox("Enabled");check->setChecked(true);row->addWidget(check);layout->addLayout(row);
        auto *table=new QTableWidget(4,3);table->setHorizontalHeaderLabels({"Name","Scale","State"});
        const char *cells[4][3]={{"Alpha","100%","Ready"},{"Beta","125%","Local"},{"Gamma","150%","Fixed"},{"Delta","200%","Offline"}};
        for (int y=0;y<4;++y)for(int x=0;x<3;++x)table->setItem(y,x,new QTableWidgetItem(cells[y][x]));
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->verticalHeader()->hide();layout->addWidget(table);
        auto *buttons=new QHBoxLayout;buttons->addWidget(new QPushButton("Open"));buttons->addWidget(new QPushButton("Save"));layout->addLayout(buttons);
    }
    void paintEvent(QPaintEvent *event) override {
        QWidget::paintEvent(event);QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing,false);
        for(int corner=0;corner<4;++corner) {
            unsigned char rgb[3];corpus_color(family,corner,rgb);
            painter.fillRect((corner%2)?width()-24:8,(corner/2)?height()-24:8,16,16,QColor(rgb[0],rgb[1],rgb[2]));
        }
        painter.end();publish(metadata(this,width(),height()));
    }
};
#else
class LocalRequests : public QWebEngineUrlRequestInterceptor {
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;
    void interceptRequest(QWebEngineUrlRequestInfo &info) override {
        const auto scheme=info.requestUrl().scheme();
        if (scheme!="qrc" && scheme!="data" && scheme!="about")info.block(true);
    }
};
static bool sandboxAvailable()
{
    const pid_t child=fork();
    if(child==0)_exit(unshare(CLONE_NEWUSER)==0?0:1);
    int status=0;
    return child>0 && waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0;
}
static QString html()
{
    QString page=QStringLiteral("<!doctype html><meta charset=utf-8><meta http-equiv=Content-Security-Policy content=\"default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'\"><style>html,body{margin:0;width:100%;height:100%;overflow:hidden;font:16px 'DejaVu Sans';background:#f4f6f8;color:#18212a}main{position:absolute;inset:32px;display:flex;flex-direction:column;gap:6px;overflow:hidden}input,button{font:inherit;min-width:0}table{border-collapse:collapse;width:100%}td,th{border:1px solid #8a98a6;padding:3px;text-align:left}.marker{position:fixed;width:16px;height:16px}</style><main><div>Local scaling corpus</div><div><input readonly value='0123456789 AaBb CcDd'><input type=checkbox checked disabled>Enabled</div><table><tr><th>Name</th><th>Scale</th><th>State</th></tr><tr><td>Alpha</td><td>100%</td><td>Ready</td></tr><tr><td>Beta</td><td>125%</td><td>Local</td></tr><tr><td>Gamma</td><td>150%</td><td>Fixed</td></tr><tr><td>Delta</td><td>200%</td><td>Offline</td></tr></table><div><button>Open</button><button>Save</button></div></main>");
    for(int i=0;i<4;++i) {
        unsigned char rgb[3];corpus_color(family,i,rgb);
        page+=QString("<div class=marker style='%1:8px;%2:8px;background:rgb(%3,%4,%5)'></div>")
            .arg(i%2?"right":"left").arg(i/2?"bottom":"top").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]);
    }
    page+=QStringLiteral("<script>window.corpusReady=false;let generation=0;function ready(){const g=++generation;window.corpusReady=false;document.fonts.ready.then(()=>requestAnimationFrame(()=>requestAnimationFrame(()=>{if(g===generation)window.corpusReady=true})));}addEventListener('resize',ready);ready();</script>");
    return page;
}
#endif
int main(int argc,char **argv)
{
    description=describe();
    if(argc==2 && QByteArray(argv[1])=="--describe") { output(description);return 0; }
    if(getuid()==0 || geteuid()==0 || argc!=3 || QByteArray(argv[1])!="--case")return 1;
    caseId=QString::fromUtf8(argv[2]);
    if(!description["cases"].toArray().contains(caseId))return 1;
    family=caseId=="qt-wayland"?0:caseId=="gtk-wayland"?1:caseId=="browser-wayland"?2:3;
    auto unsupported=[](const char *reason){output(QJsonObject{{"event","corpus-unsupported"},{"schema",1},{"case",caseId},{"reason",reason}});return 77;};
    if(description["font_resolved"].toString()!=CORPUS_FONT || description["font_sha256"].toString().isEmpty())return unsupported("fixed-font-unavailable");
    if(caseId=="qt-xwayland") {
        if(qEnvironmentVariableIsEmpty("DISPLAY"))return unsupported("x11-display-unavailable");
        connection=xcb_connect(nullptr,nullptr);
        if(xcb_connection_has_error(connection))return unsupported("x11-display-unreachable");
    }
#ifdef CORPUS_WEB
    if(!qEnvironmentVariableIsEmpty("QTWEBENGINE_DISABLE_SANDBOX") || !qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS"))
        return unsupported("browser-rendering-overrides");
    if(!sandboxAvailable())return unsupported("browser-sandbox-user-namespace-unavailable");
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#endif
    qputenv("QT_QPA_PLATFORM",caseId=="qt-xwayland"?"xcb":"wayland");
    int count=1;char *arguments[]={argv[0],nullptr};QApplication app(count,arguments);
    if(QGuiApplication::platformName()!=(caseId=="qt-xwayland"?"xcb":"wayland"))return 1;
    QGuiApplication::setDesktopFileName("org.vela.BenchmarkCorpus."+caseId);
    QFont font(CORPUS_FONT);font.setPixelSize(16);app.setFont(font);
#ifdef CORPUS_WEB
    QWebEngineProfile profile;profile.setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    profile.setHttpCacheType(QWebEngineProfile::MemoryHttpCache);profile.setUrlRequestInterceptor(new LocalRequests(&profile));
    QWebEngineView window;window.setPage(new QWebEnginePage(&profile,&window));window.resize(CORPUS_WIDTH,CORPUS_HEIGHT);
    QTimer timer;QObject::connect(&timer,&QTimer::timeout,&window,[&]{
        window.page()->runJavaScript("JSON.stringify({ready:window.corpusReady===true,width:innerWidth,height:innerHeight,dpr:devicePixelRatio,font:getComputedStyle(document.body).fontFamily})",[&](const QVariant &result){
            auto dom=QJsonDocument::fromJson(result.toString().toUtf8()).object();
            if(!dom["ready"].toBool())return;
            auto value=metadata(&window,dom["width"].toInt(),dom["height"].toInt());
            value["dom"]=dom;value["sandbox"]="enabled";value["page_requests"]="inline-local-only";
            publish(value);
        });
    });timer.start(100);window.setHtml(html(),QUrl("qrc:/vela-corpus/index.html"));
#else
    Corpus window;
    QTimer timer;QObject::connect(&timer,&QTimer::timeout,&window,[&]{publish(metadata(&window,window.width(),window.height()));});timer.start(100);
#endif
    window.setWindowTitle("Vela toolkit corpus — "+caseId);window.show();
    QTimer::singleShot(55000,&app,&QCoreApplication::quit);
    const int result=app.exec();if(connection)xcb_disconnect(connection);return result;
}
