// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#undef NDEBUG
extern "C" {
#include "buffer.h"
void vela_test_state_json(bool has_focus, struct vela_buffer *json);
}

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <cassert>

int main()
{
    for (bool focused : { false, true }) {
        struct vela_buffer json = {};
        vela_test_state_json(focused, &json);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(QByteArray(vela_buffer_text(&json)), &error);
        assert(error.error == QJsonParseError::NoError && document.isObject());
        const auto state = document.object();
        assert(state["locked"].toBool() && state["workspace"].toInt() == 2 && state["workspaces"].toInt() == 4);
        assert(focused ? state["focused"].toString() == QStringLiteral("window-\"quoted\"\nidentifier")
                       : state["focused"].isNull());
        assert(state["capabilities"].toObject()["svg_icons"].toBool() == bool(VELA_HAVE_RSVG));
        assert(state["outputs"].toArray().isEmpty() && state["layers"].toArray().isEmpty());
        assert(state["focusedLayer"].isNull());
        const auto windows = state["windows"].toArray();
        assert(windows.size() == int(focused));
        if (focused) {
            const auto window = windows.first().toObject();
            assert(window["id"] == state["focused"]);
            assert(window["upload"].toObject()["frames"].toInt() == 2);
            assert(window["upload"].toObject()["overlapped"].toBool());
        }
        vela_buffer_finish(&json);
    }
}
