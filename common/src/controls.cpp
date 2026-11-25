// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "controls.h"

#include "iconprovider.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>

namespace {

// VELA_ROUNDED_DUMP=<directory>: a development aid for the corners' check
// (scripts/check-corners.py, docs/visual-coherence.md): twice a second, every
// shown item with a radius in every window of this program, with where it is
// and its corners, as <directory>/<program>-<pid>.json. Off unless asked.
void roundedItems(QQuickItem* item, double opacity, QRectF shown, QJsonArray& out)
{
    if (!item->isVisible()) {
        return;
    }
    opacity *= item->opacity();
    // What a clipping ancestor cuts away is not there.
    if (item->clip()) {
        shown = shown.intersected(item->mapRectToScene(QRectF(0, 0, item->width(), item->height())));
        if (shown.isEmpty()) {
            return;
        }
    }
    QVariant radius = item->property("radius");
    // A picture is a shape with square corners: what it shows, in its box.
    const bool picture = item->inherits("QQuickImage") && item->property("status").toInt() == 1;
    QRectF local(0, 0, item->width(), item->height());
    if (picture) {
        radius = 0.0;
        const double w = std::min(item->width(), item->property("paintedWidth").toDouble());
        const double h = std::min(item->height(), item->property("paintedHeight").toDouble());
        local = QRectF((item->width() - w) / 2, (item->height() - h) / 2, w, h);
    }
    if (radius.isValid() && radius.canConvert<double>() && local.width() > 0 && local.height() > 0
        && shown.intersects(item->mapRectToScene(local))) {
        const QRectF box = item->mapRectToScene(local);
        const double r = radius.toDouble();
        const auto corner = [item, r](const char* name) {
            const QVariant value = item->property(name);
            return value.isValid() ? value.toDouble() : r;
        };
        const QColor color = item->property("color").value<QColor>();
        const QObject* border = item->property("border").value<QObject*>();
        const QQmlContext* context = qmlContext(item);
        QString id;
        QString file;
        for (const QQmlContext* c = context; c && id.isEmpty(); c = c->parentContext()) {
            id = c->nameForObject(item);
            file = c->baseUrl().fileName();
        }
        if (file.isEmpty() && context) {
            file = context->baseUrl().fileName();
        }
        // The file that uses it: the nearest ancestor from another file.
        QString owner;
        for (QQuickItem* up = item->parentItem(); up && owner.isEmpty(); up = up->parentItem()) {
            const QQmlContext* c = qmlContext(up);
            if (c && !c->baseUrl().fileName().isEmpty() && c->baseUrl().fileName() != file) {
                owner = c->baseUrl().fileName() + u'#' + c->nameForObject(up) + u'/' + up->objectName();
            }
        }
        out.append(QJsonObject {
            { QStringLiteral("type"), QString::fromLatin1(item->metaObject()->className()) },
            { QStringLiteral("id"), id },
            { QStringLiteral("name"), item->objectName() },
            { QStringLiteral("file"), file },
            { QStringLiteral("owner"), owner },
            { QStringLiteral("x"), box.x() },
            { QStringLiteral("y"), box.y() },
            { QStringLiteral("w"), box.width() },
            { QStringLiteral("h"), box.height() },
            { QStringLiteral("radii"), QJsonArray { corner("topLeftRadius"), corner("topRightRadius"),
                    corner("bottomLeftRadius"), corner("bottomRightRadius") } },
            { QStringLiteral("picture"), picture },
            { QStringLiteral("alpha"), picture ? 1.0 : color.isValid() ? color.alphaF() : 0.0 },
            // A Rectangle's unset border reads 1 wide and black, and is not drawn.
            { QStringLiteral("border"), border && !(border->property("width").toDouble() == 1.0
                    && border->property("color").value<QColor>() == QColor(Qt::black))
                    ? border->property("width").toDouble() : 0.0 },
            { QStringLiteral("borderAlpha"), border ? border->property("color").value<QColor>().alphaF() : 0.0 },
            { QStringLiteral("opacity"), opacity },
        });
    }
    const QList<QQuickItem*> children = item->childItems();
    for (QQuickItem* child : children) {
        roundedItems(child, opacity, shown, out);
    }
}

void dumpRounded(const QString& directory)
{
    QJsonArray windows;
    const QWindowList all = QGuiApplication::topLevelWindows();
    for (QWindow* window : all) {
        auto* quick = qobject_cast<QQuickWindow*>(window);
        if (!quick || !quick->isVisible()) {
            continue;
        }
        QJsonArray items;
        roundedItems(quick->contentItem(), 1.0, QRectF(0, 0, quick->width(), quick->height()), items);
        windows.append(QJsonObject {
            { QStringLiteral("title"), quick->title() },
            { QStringLiteral("class"), QString::fromLatin1(quick->metaObject()->className()) },
            { QStringLiteral("name"), quick->objectName() },
            { QStringLiteral("w"), quick->width() },
            { QStringLiteral("h"), quick->height() },
            { QStringLiteral("items"), items },
        });
    }
    QSaveFile file(QDir(directory).filePath(QCoreApplication::applicationName() + u'-'
        + QString::number(QCoreApplication::applicationPid()) + QStringLiteral(".json")));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(windows).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

} // namespace

namespace vela::controls {

Appearance* install(QQmlEngine& engine, Appearance::Mode mode)
{
    if (!StyleForeign::instance) {
        StyleForeign::instance = new Appearance(QCoreApplication::instance(), mode);
    }
    if (!EffectsForeign::instance) {
        EffectsForeign::instance = new BackgroundEffects(QCoreApplication::instance());
    }
    engine.addImageProvider(QStringLiteral("icon"), new IconProvider);
    const QString dump = qEnvironmentVariable("VELA_ROUNDED_DUMP");
    if (!dump.isEmpty() && qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        auto* timer = new QTimer(QCoreApplication::instance());
        QObject::connect(timer, &QTimer::timeout, timer, [dump] { dumpRounded(dump); });
        timer->start(500);
    }
    return StyleForeign::instance;
}

} // namespace vela::controls

Appearance* StyleForeign::create(QQmlEngine*, QJSEngine* engine)
{
    if (!instance) {
        instance = new Appearance(QCoreApplication::instance());
    }
    // The singleton belongs to the app, not to the QML engine.
    engine->setObjectOwnership(instance, QJSEngine::CppOwnership);
    return instance;
}

BackgroundEffects* EffectsForeign::create(QQmlEngine*, QJSEngine* engine)
{
    if (!instance) {
        instance = new BackgroundEffects(QCoreApplication::instance());
    }
    engine->setObjectOwnership(instance, QJSEngine::CppOwnership);
    return instance;
}
