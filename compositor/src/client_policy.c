// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "client_policy.h"

#include "alloc/alloc.h"
#include "security_policy.h"
#include "server.h"
#include "x11_root.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/util/log.h>

struct vela_client_policy {
    struct vela_server *server;
    struct wlr_security_context_manager_v1 *security_context;
    struct wl_listener display_destroy;
};

// What was established about a client the first time it was asked, until
// it disconnects.
struct client_trust {
    struct wl_listener destroy;
    bool sandboxed;
};

static void handle_client_destroy(struct wl_listener *listener, void *data)
{
    struct client_trust *trust = wl_container_of(listener, trust, destroy);
    wl_list_remove(&trust->destroy.link);
    free(trust);
}

static struct vela_client_policy *policy_of(struct vela_server *server)
{
    return server->client_policy;
}

bool vela_client_sandboxed(struct vela_server *server, const struct wl_client *client)
{
    struct vela_client_policy *policy = policy_of(server);
    if (!policy || !client) {
        return false;
    }
    if (policy->security_context
        && wlr_security_context_manager_v1_lookup_client(policy->security_context, client)) {
        return true;
    }
    struct wl_client *mutable_client = (struct wl_client *)client;
    struct wl_listener *known = wl_client_get_destroy_listener(mutable_client, handle_client_destroy);
    if (known) {
        struct client_trust *trust = wl_container_of(known, trust, destroy);
        return trust->sandboxed;
    }
    pid_t pid = 0;
    wl_client_get_credentials(mutable_client, &pid, NULL, NULL);
    struct client_trust *trust = VELA_NEW(struct client_trust);
    trust->sandboxed = vela_pid_in_flatpak(pid);
    trust->destroy.notify = handle_client_destroy;
    wl_client_add_destroy_listener(mutable_client, &trust->destroy);
    if (trust->sandboxed) {
        wlr_log(WLR_INFO, "Client (process %d) is a Flatpak app without a security context: "
            "privileged protocols withheld", (int)pid);
    }
    return trust->sandboxed;
}

static bool filter_global(const struct wl_client *client, const struct wl_global *global, void *data)
{
    struct vela_client_policy *policy = data;
    struct vela_server *server = policy->server;
    if (!vela_x11_root_global_visible(server, server->x11_root, client, global)) {
        return false;
    }
    const struct wl_interface *interface = wl_global_get_interface(global);
    if (!vela_global_privileged(interface ? interface->name : NULL)) {
        return true;
    }
    if (vela_x11_root_is_xwayland(server, client)) {
        return false; // X11 apps would reach these through Xwayland
    }
    return !vela_client_sandboxed(server, client);
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    struct vela_client_policy *policy = wl_container_of(listener, policy, display_destroy);
    wl_list_remove(&policy->display_destroy.link);
    wl_display_set_global_filter(policy->server->display, NULL, NULL);
    policy->server->client_policy = NULL;
    free(policy);
}

void vela_client_policy_init(struct vela_server *server)
{
    struct vela_client_policy *policy = VELA_NEW(struct vela_client_policy);
    policy->server = server;
    policy->security_context = wlr_security_context_manager_v1_create(server->display);
    if (!policy->security_context) {
        // Sandboxes then connect like any client: the Flatpak check remains.
        wlr_log(WLR_ERROR, "Can't create the security-context global");
    }
    server->client_policy = policy;
    policy->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(server->display, &policy->display_destroy);
    wl_display_set_global_filter(server->display, filter_global, policy);
}
