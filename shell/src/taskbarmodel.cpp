// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "taskbarmodel.h"

#include "appmodel.h"
#include "config.h"
#include "launcherentry.h"
#include "workspaces.h"
#include "foreigntoplevels.h"
#include "shellcontroller.h"

#include <QSettings>

#include <algorithm>

TaskbarModel::TaskbarModel(AppModel* apps, ForeignToplevelManager* windows, QObject* parent)
    : QAbstractListModel(parent)
    , m_apps(apps)
    , m_windows(windows)
{
    connect(m_windows, &ForeignToplevelManager::windowsChanged, this, &TaskbarModel::rebuild);
    connect(m_apps, &AppModel::catalogRevisionChanged, this, &TaskbarModel::rebuild);
    QSettings settings;
    if (settings.contains(QStringLiteral("taskbar/pinned"))) {
        m_pinsSaved = true;
        m_pinnedIds = settings.value(QStringLiteral("taskbar/pinned")).toStringList();
        rebuild();
    }
}

TaskbarModel::TaskbarModel(TaskbarModel* source, Config* config, Workspaces* workspaces, LauncherEntry* launcherEntry,
    const QString& output, QObject* parent)
    : QAbstractListModel(parent)
    , m_source(source)
    , m_config(config)
    , m_workspaces(workspaces)
    , m_launcherEntry(launcherEntry)
    , m_output(output)
    , m_apps(source->m_apps)
    , m_windows(source->m_windows)
{
    connect(source, &TaskbarModel::itemsChanged, this, &TaskbarModel::rebuild);
    connect(config, &Config::taskbarChanged, this, &TaskbarModel::rebuild);
    connect(workspaces, &Workspaces::changed, this, &TaskbarModel::rebuild);
    connect(launcherEntry, &LauncherEntry::changed, this, &TaskbarModel::rebuild);
    rebuild();
}

void TaskbarModel::setPinnedIds(const QStringList& ids)
{
    if (ids == m_pinnedIds) {
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("taskbar/pinned"), ids);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        return;
    }
    m_pinsSaved = true;
    m_pinnedIds = ids;
    emit pinnedIdsChanged();
    rebuild();
}

void TaskbarModel::pin(const QString& desktopId)
{
    if (!desktopId.isEmpty() && !m_pinnedIds.contains(desktopId)) {
        setPinnedIds(m_pinnedIds + QStringList { desktopId });
    }
}

void TaskbarModel::unpin(const QString& desktopId)
{
    QStringList ids = m_pinnedIds;
    if (ids.removeAll(desktopId) > 0) {
        setPinnedIds(ids);
    }
}

void TaskbarModel::move(int from, int to)
{
    if (from < 0 || from >= m_items.size() || to < 0 || to >= m_items.size() || from == to) {
        return;
    }
    if (m_source) {
        m_source->move(m_source->rowForKey(m_items[from].key), m_source->rowForKey(m_items[to].key));
        return;
    }
    const bool pinned = m_items[from].pinned;
    // Within its own group (pinned first, then the others).
    int first = 0;
    int last = int(m_items.size()) - 1;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].pinned != pinned) {
            if (pinned) {
                last = std::min(last, i - 1);
            } else {
                first = std::max(first, i + 1);
            }
        }
    }
    to = std::clamp(to, first, last);
    if (to == from) {
        return;
    }
    QStringList keys;
    for (const Item& item : std::as_const(m_items)) {
        if (item.pinned == pinned) {
            keys << (pinned ? item.desktopId : item.key);
        }
    }
    keys.move(from - first, to - first);
    if (pinned) {
        // Pinned apps not shown (apps not installed) stay too, at the end.
        QStringList ids = keys;
        for (const QString& id : std::as_const(m_pinnedIds)) {
            if (!ids.contains(id)) {
                ids << id;
            }
        }
        setPinnedIds(ids);
    } else {
        QStringList order = keys;
        for (const QString& key : std::as_const(m_unpinnedOrder)) {
            if (!order.contains(key)) {
                order << key;
            }
        }
        m_unpinnedOrder = order;
        rebuild();
    }
}

QList<TaskbarModel::Item> TaskbarModel::buildItems()
{
    QList<Item> items;
    const auto findItem = [&](const QString& key) -> Item* {
        for (Item& item : items) {
            if (item.key == key) {
                return &item;
            }
        }
        return nullptr;
    };

    // Pinned apps. Those not installed are skipped, and the same app can exist
    // both as a package and as a Flatpak: we keep one.
    for (const QString& id : std::as_const(m_pinnedIds)) {
        const QVariantMap entry = m_apps->entry(id);
        const QString name = entry.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) {
            continue;
        }
        const bool duplicate = std::any_of(items.cbegin(), items.cend(),
            [&](const Item& item) { return item.name == name; });
        if (!duplicate) {
            items.append({ id, id, name, entry.value(QStringLiteral("iconName")).toString(), true, {} });
        }
    }
    const qsizetype pinnedCount = items.size();

    // Open windows, grouped by app. Dialogs stay under the main window, they
    // don't have a button of their own.
    QList<Item> running;
    for (ForeignToplevel* window : m_windows->windows()) {
        if (window->parentWindow) {
            continue;
        }
        const QString desktopId = m_apps->findDesktopId(window->appId);
        const QString key = desktopId.isEmpty() ? QStringLiteral("app:") + window->appId : desktopId;

        Item* item = findItem(key);
        if (!item && !desktopId.isEmpty()) {
            // The pinned version of the same app (such as Flatpak vs package).
            const QString name = m_apps->entry(desktopId).value(QStringLiteral("name")).toString();
            for (qsizetype i = 0; i < pinnedCount && !item; ++i) {
                if (items[i].name == name) {
                    item = &items[i];
                }
            }
        }
        if (!item) {
            for (Item& other : running) {
                if (other.key == key) {
                    item = &other;
                }
            }
        }
        if (!item) {
            Item fresh { key, desktopId, window->title, window->appId, false, {} };
            if (!desktopId.isEmpty()) {
                const QVariantMap entry = m_apps->entry(desktopId);
                fresh.name = entry.value(QStringLiteral("name")).toString();
                fresh.icon = entry.value(QStringLiteral("iconName")).toString();
            }
            running.append(fresh);
            item = &running.last();
        }
        item->windows.append(window);
    }

    // Unpinned apps keep the place they had when they appeared.
    for (const Item& item : std::as_const(running)) {
        if (!m_unpinnedOrder.contains(item.key)) {
            m_unpinnedOrder.append(item.key);
        }
    }
    m_unpinnedOrder.removeIf([&](const QString& key) {
        return std::none_of(running.cbegin(), running.cend(), [&](const Item& item) { return item.key == key; });
    });
    std::sort(running.begin(), running.end(), [&](const Item& a, const Item& b) {
        return m_unpinnedOrder.indexOf(a.key) < m_unpinnedOrder.indexOf(b.key);
    });

    items.append(running);
    return items;
}

QList<TaskbarModel::Item> TaskbarModel::filteredItems() const
{
    QList<Item> items = m_source->m_items;
    const bool displayFilter = m_config->taskbarAllScreens() && m_config->taskbarAppsOn() == QLatin1String("window");
    const bool desktopFilter = m_config->taskbarWindowsFrom() != QLatin1String("all");
    QHash<const ForeignToplevel*, ForeignToplevel*> attentionTargets;
    if (m_config->taskbarAttention()) {
        for (ForeignToplevel* window : m_windows->windows()) {
            if (!m_workspaces->needsAttention(window->identifier)) {
                continue;
            }
            const ForeignToplevel* owner = window;
            for (qsizetype hops = 0; owner->parentWindow && hops < m_windows->windows().size(); ++hops) {
                owner = owner->parentWindow;
            }
            attentionTargets.insert(owner, window);
        }
    }
    for (Item& item : items) {
        const auto badge = m_launcherEntry->badge(item.desktopId);
        item.badgeCount = m_config->taskbarBadges() ? badge.count : 0;
        item.progress = m_config->taskbarProgress() ? badge.progress : -1;
        const bool urgent = m_config->taskbarAttention() && badge.urgent;
        item.attention = urgent && item.pinned && item.windows.isEmpty();
        item.windows.removeIf([&](const ForeignToplevel* window) {
            const QString output = m_workspaces->outputOf(window->identifier);
            if (displayFilter && !output.isEmpty() && output != m_output) {
                return true;
            }
            if (auto* target = attentionTargets.value(window)) {
                item.attention = true;
                item.attentionTarget = target;
                return false;
            }
            if (urgent) {
                item.attention = true;
                return false;
            }
            const int desktop = m_workspaces->workspaceOf(window->identifier);
            return desktopFilter && desktop >= 0 && desktop != m_workspaces->current();
        });
    }
    items.removeIf([](const Item& item) { return !item.pinned && item.windows.isEmpty(); });
    return items;
}

int TaskbarModel::rowForKey(const QString& key) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].key == key) {
            return i;
        }
    }
    return -1;
}

ForeignToplevel* TaskbarModel::attentionWindow(const Item& item) const
{
    if (item.attention && m_workspaces) {
        if (item.attentionTarget) {
            return item.attentionTarget;
        }
        if (m_launcherEntry->badge(item.desktopId).urgent && !item.windows.isEmpty()) {
            return *std::max_element(item.windows.cbegin(), item.windows.cend(),
                [](const ForeignToplevel* a, const ForeignToplevel* b) { return a->lastActivated < b->lastActivated; });
        }
    }
    return nullptr;
}

void TaskbarModel::rebuild()
{
    QList<Item> items = m_source ? filteredItems() : buildItems();

    // We update the model with targeted inserts and removals: the taskbar can
    // animate the button that appears or disappears, without recreating the
    // others.
    const auto keyAt = [](const QList<Item>& list, const QString& key) {
        for (qsizetype i = 0; i < list.size(); ++i) {
            if (list[i].key == key) {
                return i;
            }
        }
        return qsizetype(-1);
    };
    for (qsizetype i = m_items.size() - 1; i >= 0; --i) {
        if (keyAt(items, m_items[i].key) < 0) {
            beginRemoveRows({}, int(i), int(i));
            m_items.removeAt(i);
            endRemoveRows();
        }
    }
    for (qsizetype i = 0; i < items.size(); ++i) {
        if (i < m_items.size() && m_items[i].key == items[i].key) {
            m_items[i] = items[i];
            continue;
        }
        if (keyAt(m_items, items[i].key) > i) {
            // Order changed (such as newly pinned apps): start over.
            beginResetModel();
            m_items = std::move(items);
            endResetModel();
            sendButtonRects();
            emit itemsChanged();
            return;
        }
        beginInsertRows({}, int(i), int(i));
        m_items.insert(i, items[i]);
        endInsertRows();
    }
    if (!m_items.isEmpty()) {
        emit dataChanged(index(0), index(int(m_items.size()) - 1));
    }
    sendButtonRects(); // the windows just opened must know too
    emit itemsChanged();
}

QStringList TaskbarModel::appIds(int row) const
{
    QStringList out;
    if (row < 0 || row >= m_items.size()) {
        return out;
    }
    for (ForeignToplevel* window : m_items.at(row).windows) {
        if (window && !out.contains(window->appId)) {
            out << window->appId;
        }
    }
    return out;
}

void TaskbarModel::setButtonGeometry(int row, QWindow* panel, const QRectF& rect)
{
    if (row < 0 || row >= m_items.size()) {
        return;
    }
    m_panel = panel;
    m_buttonRects.insert(m_items.at(row).key, rect.toAlignedRect());
    sendButtonRects();
}

void TaskbarModel::sendButtonRects()
{
    if (!m_panel) {
        return;
    }
    QList<QPointer<ForeignToplevel>> shown;
    for (const Item& item : std::as_const(m_items)) {
        const auto rect = m_buttonRects.constFind(item.key);
        if (rect == m_buttonRects.constEnd()) {
            continue;
        }
        for (ForeignToplevel* window : item.windows) {
            window->setButtonRect(m_panel, *rect);
            shown.append(window);
        }
    }
    for (const auto& window : std::as_const(m_windowsWithRect)) {
        if (window && !shown.contains(window) && m_windows->windows().contains(window)) {
            window->setButtonRect(m_panel, { });
        }
    }
    m_windowsWithRect = std::move(shown);
}

int TaskbarModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant TaskbarModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) {
        return {};
    }
    const Item& item = m_items.at(index.row());
    switch (role) {
    case KeyRole: return item.key;
    case Qt::DisplayRole:
    case NameRole: return item.name;
    case IconRole: return item.icon;
    case PinnedRole: return item.pinned;
    case WindowCountRole: return int(item.windows.size());
    case DesktopIdRole: return item.desktopId;
    case HasWindowsRole:
        return !item.windows.isEmpty() || (m_source && m_source->recentWindow(m_source->rowForKey(item.key)));
    case AttentionRole: return item.attention;
    case BadgeCountRole:
        return item.badgeCount > 99 ? QStringLiteral("99+")
            : item.badgeCount > 0   ? QString::number(item.badgeCount)
                                    : QString();
    case ProgressRole: return item.progress;
    case ActiveWindowIdRole:
        for (const ForeignToplevel* window : item.windows) {
            if (window->activated) {
                return window->identifier;
            }
        }
        return QString();
    case WindowIdsRole: {
        QStringList ids;
        for (const ForeignToplevel* window : item.windows) {
            if (!window->identifier.isEmpty()) {
                ids << window->identifier;
            }
        }
        return ids;
    }
    case WindowActiveRole:
        return std::any_of(item.windows.cbegin(), item.windows.cend(),
            [](const ForeignToplevel* window) { return window->activated; });
    default: return {};
    }
}

QHash<int, QByteArray> TaskbarModel::roleNames() const
{
    return {
        { KeyRole, "key" },
        { NameRole, "name" },
        { IconRole, "iconName" },
        { PinnedRole, "pinned" },
        { WindowCountRole, "windowCount" },
        { WindowActiveRole, "windowActive" },
        { DesktopIdRole, "desktopId" },
        { WindowIdsRole, "windowIds" },
        { AttentionRole, "attention" },
        { ActiveWindowIdRole, "activeWindowId" },
        { BadgeCountRole, "badgeCount" },
        { ProgressRole, "progress" },
        { HasWindowsRole, "hasWindows" },
    };
}

void TaskbarModel::activate(int row)
{
    if (row < 0 || row >= m_items.size()) {
        return;
    }
    const Item& item = m_items.at(row);
    if (ForeignToplevel* window = attentionWindow(item)) {
        window->requestActivate();
        return;
    }
    if (item.windows.isEmpty()) {
        ForeignToplevel* hidden = m_source ? m_source->recentWindow(m_source->rowForKey(item.key)) : nullptr;
        if (hidden) {
            hidden->requestActivate();
        } else {
            m_apps->launchId(item.desktopId);
        }
        return;
    }

    const auto active = std::find_if(item.windows.cbegin(), item.windows.cend(),
        [](const ForeignToplevel* window) { return window->activated; });
    if (active != item.windows.cend()) {
        if (item.windows.size() == 1) {
            (*active)->requestMinimize();
        } else {
            // Several windows of the same app: move to the next one.
            const qsizetype next = (active - item.windows.cbegin() + 1) % item.windows.size();
            item.windows.at(next)->requestActivate();
        }
        return;
    }
    // Otherwise the app's most recently used window comes back.
    const auto recent = std::max_element(item.windows.cbegin(), item.windows.cend(),
        [](const ForeignToplevel* a, const ForeignToplevel* b) { return a->lastActivated < b->lastActivated; });
    (*recent)->requestActivate();
}

bool TaskbarModel::showApp(const QString& desktopId)
{
    for (const Item& item : std::as_const(m_items)) {
        if (!desktopId.isEmpty() && item.desktopId == desktopId && !item.windows.isEmpty()) {
            const auto recent = std::max_element(item.windows.cbegin(), item.windows.cend(),
                [](const ForeignToplevel* a, const ForeignToplevel* b) { return a->lastActivated < b->lastActivated; });
            (*recent)->requestActivate();
            return true;
        }
    }
    return false;
}

bool TaskbarModel::isLauncherDrop(const QUrl& url) const
{
    return AppModel::isLauncherUrl(url);
}

bool TaskbarModel::pinDropped(const QUrl& url, int row)
{
    TaskbarModel* source = m_source ? m_source : this;
    const QString id = m_apps->importTaskbarLauncher(url);
    if (id.isEmpty()) {
        return false;
    }
    QStringList pins = source->pinnedIds();
    const QString successor =
        row >= 0 && row < m_items.size() && m_items[row].pinned ? m_items[row].desktopId : QString();
    if (successor == id) {
        return true;
    }
    pins.removeAll(id);
    const qsizetype index = pins.indexOf(successor);
    pins.insert(index < 0 ? pins.size() : index, id);
    source->setPinnedIds(pins);
    return source->pinnedIds() == pins;
}

void TaskbarModel::raiseWindow(int row)
{
    if (ForeignToplevel* window = recentWindow(row)) {
        window->requestActivate();
    }
}

void TaskbarModel::launchNew(int row)
{
    if (row >= 0 && row < m_items.size() && !m_items.at(row).desktopId.isEmpty()) {
        m_apps->launchId(m_items.at(row).desktopId);
    }
}

ForeignToplevel* TaskbarModel::recentWindow(int row) const
{
    if (row < 0 || row >= m_items.size() || m_items.at(row).windows.isEmpty()) {
        return nullptr;
    }
    const QList<ForeignToplevel*>& windows = m_items.at(row).windows;
    return *std::max_element(windows.cbegin(), windows.cend(),
        [](const ForeignToplevel* a, const ForeignToplevel* b) { return a->lastActivated < b->lastActivated; });
}

void TaskbarModel::closeWindows(int row)
{
    if (row >= 0 && row < m_items.size()) {
        for (ForeignToplevel* window : m_items.at(row).windows) {
            window->close();
        }
    }
}

void TaskbarModel::endTask(int row)
{
    ForeignToplevel* window = recentWindow(row);
    if (!window && m_source && row >= 0 && row < m_items.size()) {
        window = m_source->recentWindow(m_source->rowForKey(m_items.at(row).key));
    }
    if (window && !window->appId.isEmpty()) {
        ShellController::sendToCompositor("end-task " + window->appId.toUtf8());
    }
}

QVariantMap TaskbarModel::windowState(int row) const
{
    const ForeignToplevel* window = recentWindow(row);
    if (!window) {
        return {};
    }
    return { { QStringLiteral("maximized"), window->maximized }, { QStringLiteral("minimized"), window->minimized } };
}

void TaskbarModel::windowAction(int row, const QString& action)
{
    ForeignToplevel* window = recentWindow(row);
    if (!window) {
        return;
    }
    if (action == QLatin1String("restore")) {
        if (window->minimized) {
            window->requestActivate();
        } else if (window->maximized) {
            window->unset_maximized();
        }
    } else if (action == QLatin1String("minimize")) {
        window->requestMinimize();
    } else if (action == QLatin1String("maximize")) {
        window->set_maximized();
    } else if (action == QLatin1String("close")) {
        window->close();
    } else if (action == QLatin1String("move") || action == QLatin1String("resize")) {
        // From the keyboard, like on Windows: the window comes forward and the
        // compositor moves it with the arrows.
        window->requestActivate();
        ShellController::sendToCompositor("window active " + action.toLatin1());
    }
}

void TaskbarModel::toggleDesktop()
{
    if (m_source) {
        m_source->toggleDesktop();
        return;
    }
    QList<ForeignToplevel*> visible;
    for (ForeignToplevel* window : m_windows->windows()) {
        if (!window->minimized) {
            visible.append(window);
        }
    }
    if (!visible.isEmpty()) {
        m_hiddenByDesktop.clear();
        for (ForeignToplevel* window : std::as_const(visible)) {
            m_hiddenByDesktop.append(window);
            window->requestMinimize();
        }
        return;
    }
    // Everything already minimized: the previous ones come back, the most
    // recent last (in front).
    std::sort(m_hiddenByDesktop.begin(), m_hiddenByDesktop.end(),
        [](const QPointer<ForeignToplevel>& a, const QPointer<ForeignToplevel>& b) {
            return (a ? a->lastActivated : 0) < (b ? b->lastActivated : 0);
        });
    for (const QPointer<ForeignToplevel>& window : std::as_const(m_hiddenByDesktop)) {
        if (window) {
            window->requestActivate();
        }
    }
    m_hiddenByDesktop.clear();
}
