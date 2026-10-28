// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The agent (docs/bluetooth.md §6.4) and its question (§7.2).
//
// The agent is registered, and made default, while BlueZ is on the bus and
// the session is active (§6.7). Every Agent1 call becomes the question:
// BlueZ sends an agent one request at a time (§6.5), so there is at most
// one. BlueZ waits for the answer to confirm-passkey, authorize-pairing,
// authorize-service, enter-passkey and enter-pin: the call stays open until
// the user answers, BlueZ cancels, or the question closes for another
// reason. DisplayPasskey expects no reply, and DisplayPinCode is answered
// at once, as BlueZ gives the PIN to the kernel only then: show-passkey and
// show-pin stay open until the pairing ends.
//
// Questions, and the signals that carry them, are for Vela's UI only
// (§7.3): the signals go to each listener, never broadcast.

#include "daemon.h"

#include "caller.h"
#include "describe.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#define AGENT_PATH "/org/vela/BluetoothAgent"
#define AGENT_MANAGER "org.bluez.AgentManager1"
#define REJECTED "org.bluez.Error.Rejected"
#define CANCELED "org.bluez.Error.Canceled"
// BlueZ gives up on a request after 60 s (REQUEST_TIMEOUT in src/agent.c).
#define QUESTION_US (60 * 1000000ULL)

struct vela_bt_question *vela_bt_question_on(struct vela_bt_daemon *d, const char *device)
{
    struct vela_bt_question *question = d->question;
    return question && (!device || strcmp(question->device, device) == 0) ? question : NULL;
}

// ------------------------------------------------------------ signals --

typedef void (*append_body)(sd_bus_message *m, struct vela_bt_daemon *d, const void *data);

// Each listener gets its own copy, addressed to it.
static void signal_listeners(struct vela_bt_daemon *d, const char *member, append_body append, const void *data)
{
    for (const char *name = sd_bus_track_first(d->listeners); name; name = sd_bus_track_next(d->listeners)) {
        sd_bus_message *m = NULL;
        if (sd_bus_message_new_signal(d->session, &m, VELA_BT_PATH, VELA_BT_NAME, member) >= 0
            && sd_bus_message_set_destination(m, name) >= 0) {
            append(m, d, data);
            sd_bus_send(d->session, m, NULL);
        }
        sd_bus_message_unref(m);
    }
}

static const char *drawn_by(struct vela_bt_daemon *d, const struct vela_bt_question *question)
{
    struct vela_bt_device *device = vela_bt_device_find(d, question->device);
    struct vela_bt_operation *operation = device ? device->operation : NULL;
    bool settings = !question->unsolicited && operation && operation->kind == VELA_BT_ADD && operation->prompters
        && sd_bus_track_count(operation->prompters) > 0;
    return settings ? "settings" : "shell";
}

static void append_details(sd_bus_message *m, struct vela_bt_daemon *d, const struct vela_bt_question *question)
{
    sd_bus_message_open_container(m, 'a', "{sv}");
    vela_bt_append_entry(m, "kind", "s", vela_bt_question_kind_name(question->kind));
    vela_bt_append_entry(m, "name", "s", question->name);
    vela_bt_append_entry(m, "icon", "s", question->icon);
    vela_bt_append_entry(m, "address", "s", question->address);
    vela_bt_append_entry(m, "unsolicited", "b", (int)question->unsolicited);
    vela_bt_append_entry(m, "drawnBy", "s", drawn_by(d, question));
    vela_bt_append_entry(m, "expires", "t", question->expires);
    if (question->kind == VELA_BT_CONFIRM_PASSKEY || question->kind == VELA_BT_SHOW_PASSKEY) {
        vela_bt_append_entry(m, "passkey", "u", question->passkey);
    }
    if (question->kind == VELA_BT_SHOW_PASSKEY) {
        vela_bt_append_entry(m, "entered", "q", question->entered);
    }
    if (question->kind == VELA_BT_SHOW_PIN) {
        vela_bt_append_entry(m, "pin", "s", question->pin);
    }
    if (question->kind == VELA_BT_AUTHORIZE_SERVICE) {
        vela_bt_append_entry(m, "uuid", "s", question->uuid);
        vela_bt_append_entry(m, "service", "s", vela_bt_service_name(question->uuid));
    }
    sd_bus_message_close_container(m);
}

static void append_question(sd_bus_message *m, struct vela_bt_daemon *d, const void *data)
{
    const struct vela_bt_question *question = data;
    sd_bus_message_append(m, "to", question->id, question->device);
    append_details(m, d, question);
}

static void append_entered(sd_bus_message *m, struct vela_bt_daemon *d, const void *data)
{
    const struct vela_bt_question *question = data;
    sd_bus_message_append(m, "t", question->id);
    sd_bus_message_append(m, "a{sv}", 1, "entered", "q", question->entered);
}

struct closed {
    uint64_t id;
    const char *reason;
};

static void append_closed(sd_bus_message *m, struct vela_bt_daemon *d, const void *data)
{
    const struct closed *closed = data;
    sd_bus_message_append(m, "ts", closed->id, closed->reason);
}

void vela_bt_question_resend(struct vela_bt_daemon *d)
{
    if (d->question) {
        signal_listeners(d, "Question", append_question, d->question);
    }
}

int vela_bt_present_questions(struct vela_bt_daemon *d, sd_bus_message *call)
{
    if (!d->presenters && sd_bus_track_new(d->session, &d->presenters, NULL, NULL) < 0) {
        return -ENOMEM;
    }
    int r = sd_bus_track_add_sender(d->presenters, call);
    if (r < 0) {
        return r;
    }
    vela_bt_listen(d, call);
    return sd_bus_reply_method_return(call, NULL);
}

void vela_bt_listen(struct vela_bt_daemon *d, sd_bus_message *call)
{
    if (!d->listeners && sd_bus_track_new(d->session, &d->listeners, NULL, NULL) < 0) {
        return;
    }
    sd_bus_track_add_sender(d->listeners, call);
}

// ---------------------------------------------------------- questions --

static void free_question(struct vela_bt_question *question)
{
    sd_bus_message_unref(question->request);
    sd_event_source_disable_unref(question->timer);
    free(question->device);
    free(question->name);
    free(question->icon);
    free(question->address);
    free(question->uuid);
    free(question);
}

// Closes the question without telling its operation. Its device's path,
// for the caller to free.
static char *finish(struct vela_bt_daemon *d, const char *reason)
{
    struct vela_bt_question *question = d->question;
    d->question = NULL;
    if (question->request) {
        sd_bus_reply_method_error(question->request, &SD_BUS_ERROR_MAKE_CONST(CANCELED, "Canceled"));
    }
    vela_bt_log("question %llu closed: %s", (unsigned long long)question->id, reason);
    struct closed closed = { question->id, reason };
    signal_listeners(d, "QuestionClosed", append_closed, &closed);
    char *device = question->device;
    question->device = NULL;
    free_question(question);
    return device;
}

void vela_bt_question_close(struct vela_bt_daemon *d, const char *device, const char *reason)
{
    if (!vela_bt_question_on(d, device)) {
        return;
    }
    char *path = finish(d, reason);
    struct vela_bt_device *asked = path ? vela_bt_device_find(d, path) : NULL;
    if (asked) {
        vela_bt_operations_question_closed(d, asked, reason);
    }
    free(path);
}

static int on_expired(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_bt_daemon *d = data;
    // BlueZ gave up too, and its Cancel didn't come.
    vela_bt_question_close(d, NULL, "cancelled-by-device");
    return 0;
}

static void ask(struct vela_bt_daemon *d, struct vela_bt_device *device, enum vela_bt_question_kind kind,
    sd_bus_message *request, uint32_t passkey, uint16_t entered, const char *pin, const char *uuid)
{
    struct vela_bt_question *question = calloc(1, sizeof(*question));
    if (!question) {
        if (request) {
            sd_bus_reply_method_error(request, &SD_BUS_ERROR_MAKE_CONST(REJECTED, "Rejected"));
        }
        return;
    }
    question->id = ++d->question_id;
    question->kind = kind;
    question->device = strdup(device->path);
    question->name = strdup(device->name ? device->name : "");
    question->icon = strdup(device->icon && *device->icon ? device->icon : "bluetooth");
    question->address = strdup(device->address ? device->address : "");
    question->uuid = strdup(uuid ? uuid : "");
    question->unsolicited = !(device->operation && device->operation->kind == VELA_BT_ADD);
    question->expires = vela_bt_now() + QUESTION_US;
    question->passkey = passkey;
    question->entered = entered;
    snprintf(question->pin, sizeof(question->pin), "%s", pin ? pin : "");
    question->request = sd_bus_message_ref(request);
    if (!question->device || !question->name || !question->icon || !question->address || !question->uuid) {
        if (request) {
            sd_bus_reply_method_error(request, &SD_BUS_ERROR_MAKE_CONST(REJECTED, "Rejected"));
        }
        free_question(question);
        return;
    }
    sd_event_add_time(d->event, &question->timer, CLOCK_MONOTONIC, question->expires, 0, on_expired, d);
    d->question = question;
    vela_bt_log("question %llu: %s for %s%s", (unsigned long long)question->id, vela_bt_question_kind_name(kind),
        device->path, question->unsolicited ? " (unsolicited)" : "");
    signal_listeners(d, "Question", append_question, question);
}

int vela_bt_questions(struct vela_bt_daemon *d, sd_bus_message *call)
{
    sd_bus_message *reply = NULL;
    int r = sd_bus_message_new_method_return(call, &reply);
    if (r < 0) {
        return r;
    }
    vela_bt_listen(d, call);
    sd_bus_message_open_container(reply, 'a', "(toa{sv})");
    if (d->question) {
        sd_bus_message_open_container(reply, 'r', "toa{sv}");
        append_question(reply, d, d->question);
        sd_bus_message_close_container(reply);
    }
    sd_bus_message_close_container(reply);
    r = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return r;
}

int vela_bt_answer(struct vela_bt_daemon *d, sd_bus_message *call, uint64_t id, const char *response,
    const char *value, sd_bus_error *error)
{
    struct vela_bt_question *question = d->question;
    if (id == 0 || id > d->question_id) {
        return sd_bus_error_set(error, VELA_BT_ERROR("UnknownQuestion"), "No such question.");
    }
    // Answered, or cancelled, already (§6.5, rule 7).
    if (!question || question->id != id) {
        return sd_bus_error_set(error, VELA_BT_ERROR("QuestionClosed"), "The question is closed.");
    }
    enum vela_bt_response chosen;
    uint32_t passkey = 0;
    switch (vela_bt_check_answer(question->kind, response, value, &chosen, &passkey)) {
    case VELA_BT_WRONG_RESPONSE:
        return sd_bus_error_set(error, VELA_BT_ERROR("WrongResponse"), "Not an answer to this question.");
    case VELA_BT_INVALID_VALUE:
        return sd_bus_error_set(error, VELA_BT_ERROR("InvalidValue"), "Not a valid code.");
    case VELA_BT_ANSWER_OK:
        break;
    }
    sd_bus_message *request = question->request;
    question->request = NULL;
    if (request) {
        if (chosen == VELA_BT_REJECT) {
            sd_bus_reply_method_error(request, &SD_BUS_ERROR_MAKE_CONST(REJECTED, "Rejected"));
        } else if (chosen == VELA_BT_CANCEL) {
            sd_bus_reply_method_error(request, &SD_BUS_ERROR_MAKE_CONST(CANCELED, "Canceled"));
        } else if (question->kind == VELA_BT_ENTER_PASSKEY) {
            sd_bus_reply_method_return(request, "u", passkey);
        } else if (question->kind == VELA_BT_ENTER_PIN) {
            sd_bus_reply_method_return(request, "s", value);
        } else {
            sd_bus_reply_method_return(request, NULL);
        }
        sd_bus_message_unref(request);
    }
    vela_bt_log("question %llu answered: %s", (unsigned long long)id, response);
    // Reject and cancel end the user's Add, with no message (§7.2).
    struct vela_bt_device *device = vela_bt_device_find(d, question->device);
    if (chosen != VELA_BT_ACCEPT && device && device->operation && device->operation->kind == VELA_BT_ADD
        && !question->unsolicited) {
        vela_bt_operation_cancel(d, device, NULL);
    }
    vela_bt_question_close(d, NULL, "answered");
    return sd_bus_reply_method_return(call, NULL);
}

// ---------------------------------------------------------- the agent --

static bool from_bluez(struct vela_bt_daemon *d, sd_bus_message *m)
{
    const char *sender = sd_bus_message_get_sender(m);
    return d->bluez && sender && strcmp(sender, d->bluez) == 0;
}

static int refuse(sd_bus_message *call, const char *path, const char *why)
{
    vela_bt_log("refused %s for %s: %s", sd_bus_message_get_member(call), path ? path : "?", why);
    return sd_bus_reply_method_error(call, &SD_BUS_ERROR_MAKE_CONST(REJECTED, "Rejected"));
}

// A request from BlueZ about `path`: the device, or NULL once refused.
static struct vela_bt_device *requester(struct vela_bt_daemon *d, sd_bus_message *call, const char *path)
{
    if (!from_bluez(d, call)) {
        refuse(call, path, "not from BlueZ");
        return NULL;
    }
    struct vela_bt_device *device = vela_bt_device_find(d, path);
    bool solicited = device && device->operation && device->operation->kind == VELA_BT_ADD;
    const char *why = NULL;
    if (!device || !vela_bt_device_listed(d, device)) {
        why = "not a device of the adapter in use";
    } else if (!d->session_active) {
        why = "the session is in the background";
    } else if (!solicited && (!d->presenters || sd_bus_track_count(d->presenters) == 0)) {
        // B3 has no shell dialog yet. Only a UI that explicitly promises to
        // draw spontaneous prompts may make them wait for an answer (B4).
        // No presenter: refuse immediately, including on a locked screen
        // before the compositor propagates LockedHint (B5).
        why = "no presenter for unsolicited pairing";
    } else if (d->session_locked) {
        // Nobody is there to answer (§6.4).
        why = "the screen is locked";
    } else if (d->question) {
        // A passkey or PIN still shown for the same device: BlueZ moved on.
        if (!d->question->request && strcmp(d->question->device, path) == 0) {
            free(finish(d, "cancelled-by-device"));
        } else {
            why = "another question is open";
        }
    }
    if (why) {
        refuse(call, path, why);
        return NULL;
    }
    return device;
}

static int agent_release(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    if (!from_bluez(d, call)) {
        return refuse(call, NULL, "not from BlueZ");
    }
    // BlueZ forgot the agent: it's registered again when BlueZ comes back.
    vela_bt_log("agent released by BlueZ");
    if (!d->agent_call) {
        d->agent_registered = false;
        d->agent_default = false;
        vela_bt_agent_update(d);
    }
    return sd_bus_reply_method_return(call, NULL);
}

static int agent_request_pin_code(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *path;
    int r = sd_bus_message_read(call, "o", &path);
    if (r < 0) {
        return r;
    }
    struct vela_bt_device *device = requester(d, call, path);
    if (!device) {
        return 1;
    }
    bool solicited = device->operation && device->operation->kind == VELA_BT_ADD;
    switch (vela_bt_legacy_pin(vela_bt_device_type(device->icon, device->class_of_device), solicited)) {
    case VELA_BT_LEGACY_0000:
        vela_bt_log("%s: legacy PIN 0000", path);
        return sd_bus_reply_method_return(call, "s", "0000");
    case VELA_BT_LEGACY_SHOW_RANDOM: {
        uint32_t random = 0;
        if (getrandom(&random, sizeof(random), 0) != sizeof(random)) {
            return refuse(call, path, "no random PIN");
        }
        char pin[7];
        snprintf(pin, sizeof(pin), "%06u", random % 1000000);
        r = sd_bus_reply_method_return(call, "s", pin);
        ask(d, device, VELA_BT_SHOW_PIN, NULL, 0, 0, pin, NULL);
        return r;
    }
    case VELA_BT_LEGACY_ASK:
        break;
    }
    ask(d, device, VELA_BT_ENTER_PIN, call, 0, 0, NULL, NULL);
    return 1;
}

static int agent_display_pin_code(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *path, *pin;
    int r = sd_bus_message_read(call, "os", &path, &pin);
    if (r < 0) {
        return r;
    }
    struct vela_bt_device *device = requester(d, call, path);
    if (!device) {
        return 1;
    }
    r = sd_bus_reply_method_return(call, NULL);
    ask(d, device, VELA_BT_SHOW_PIN, NULL, 0, 0, pin, NULL);
    return r;
}

static int agent_request_passkey(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *path;
    int r = sd_bus_message_read(call, "o", &path);
    if (r < 0) {
        return r;
    }
    struct vela_bt_device *device = requester(d, call, path);
    if (device) {
        ask(d, device, VELA_BT_ENTER_PASSKEY, call, 0, 0, NULL, NULL);
    }
    return 1;
}

static int agent_display_passkey(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *path;
    uint32_t passkey;
    uint16_t entered;
    int r = sd_bus_message_read(call, "ouq", &path, &passkey, &entered);
    if (r < 0) {
        return r;
    }
    // Again, as keys are typed on the device: the digits so far.
    struct vela_bt_question *open = vela_bt_question_on(d, path);
    if (open && from_bluez(d, call) && open->kind == VELA_BT_SHOW_PASSKEY && open->passkey == passkey) {
        if (open->entered != entered) {
            open->entered = entered;
            signal_listeners(d, "QuestionChanged", append_entered, open);
        }
        return sd_bus_reply_method_return(call, NULL);
    }
    struct vela_bt_device *device = requester(d, call, path);
    if (!device) {
        return 1;
    }
    r = sd_bus_reply_method_return(call, NULL);
    ask(d, device, VELA_BT_SHOW_PASSKEY, NULL, passkey, entered, NULL, NULL);
    return r;
}

static int agent_request_confirmation(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *path;
    uint32_t passkey;
    int r = sd_bus_message_read(call, "ou", &path, &passkey);
    if (r < 0) {
        return r;
    }
    struct vela_bt_device *device = requester(d, call, path);
    if (device) {
        ask(d, device, VELA_BT_CONFIRM_PASSKEY, call, passkey, 0, NULL, NULL);
    }
    return 1;
}

static int agent_request_authorization(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *path;
    int r = sd_bus_message_read(call, "o", &path);
    if (r < 0) {
        return r;
    }
    struct vela_bt_device *device = requester(d, call, path);
    if (device) {
        ask(d, device, VELA_BT_AUTHORIZE_PAIRING, call, 0, 0, NULL, NULL);
    }
    return 1;
}

static int agent_authorize_service(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *path, *uuid;
    int r = sd_bus_message_read(call, "os", &path, &uuid);
    if (r < 0) {
        return r;
    }
    struct vela_bt_device *device = requester(d, call, path);
    if (device) {
        ask(d, device, VELA_BT_AUTHORIZE_SERVICE, call, 0, 0, NULL, uuid);
    }
    return 1;
}

static int agent_cancel(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    if (!from_bluez(d, call)) {
        return refuse(call, NULL, "not from BlueZ");
    }
    // After Vela's reply there's nothing left to cancel (§6.5, rule 7).
    if (d->question) {
        d->question->request = sd_bus_message_unref(d->question->request);
        vela_bt_question_close(d, NULL, "cancelled-by-device");
    }
    return sd_bus_reply_method_return(call, NULL);
}

static const sd_bus_vtable agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", agent_release, 0),
    SD_BUS_METHOD("RequestPinCode", "o", "s", agent_request_pin_code, 0),
    SD_BUS_METHOD("DisplayPinCode", "os", "", agent_display_pin_code, 0),
    SD_BUS_METHOD("RequestPasskey", "o", "u", agent_request_passkey, 0),
    SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent_display_passkey, 0),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", agent_request_confirmation, 0),
    SD_BUS_METHOD("RequestAuthorization", "o", "", agent_request_authorization, 0),
    SD_BUS_METHOD("AuthorizeService", "os", "", agent_authorize_service, 0),
    SD_BUS_METHOD("Cancel", "", "", agent_cancel, 0),
    SD_BUS_VTABLE_END,
};

// ----------------------------------------------------- registration --

// Do not spin on persistent BlueZ errors. Retry registration and default
// ownership with bounded exponential backoff, never while in the background.
static int on_agent_retry(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_bt_daemon *d = data;
    d->agent_retry = sd_event_source_disable_unref(d->agent_retry);
    vela_bt_agent_update(d);
    return 0;
}

static void retry_agent(struct vela_bt_daemon *d)
{
    if (!d->session_active || !d->bluez || d->agent_retry) {
        return;
    }
    uint64_t delay = d->agent_backoff_us ? d->agent_backoff_us : 1000000ULL;
    d->agent_backoff_us = delay < 15000000ULL ? delay * 2 : 30000000ULL;
    int r = sd_event_add_time_relative(d->event, &d->agent_retry, CLOCK_MONOTONIC,
        delay, 0, on_agent_retry, d);
    if (r < 0) {
        vela_bt_log("agent retry timer: %s", strerror(-r));
    }
}

static int on_default(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    d->agent_call = sd_bus_slot_unref(d->agent_call);
    if (sd_bus_message_is_method_error(reply, NULL)) {
        d->agent_default = false;
        vela_bt_log("RequestDefaultAgent: %s", sd_bus_message_get_error(reply)->message);
        retry_agent(d);
    } else {
        d->agent_default = true;
        d->agent_backoff_us = 0;
        vela_bt_log("agent registered, and the default one");
    }
    vela_bt_agent_update(d);
    return 0;
}

static int on_registered(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    d->agent_call = sd_bus_slot_unref(d->agent_call);
    if (!sd_bus_message_is_method_error(reply, "org.bluez.Error.AlreadyExists")
        && sd_bus_message_is_method_error(reply, NULL)) {
        vela_bt_log("RegisterAgent: %s", sd_bus_message_get_error(reply)->message);
        retry_agent(d);
        return 0;
    }
    d->agent_registered = true;
    d->agent_default = false;
    vela_bt_agent_update(d);
    return 0;
}

static int on_unregistered(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    d->agent_call = sd_bus_slot_unref(d->agent_call);
    if (sd_bus_message_is_method_error(reply, NULL)) {
        vela_bt_log("UnregisterAgent: %s", sd_bus_message_get_error(reply)->message);
    }
    vela_bt_agent_update(d);
    return 0;
}

void vela_bt_agent_update(struct vela_bt_daemon *d)
{
    if (!d->bluez || !d->session_active) {
        d->agent_retry = sd_event_source_disable_unref(d->agent_retry);
        d->agent_backoff_us = 0;
    }
    if (!d->bluez) {
        d->agent_call = sd_bus_slot_unref(d->agent_call);
        d->agent_registered = false;
        d->agent_default = false;
        return;
    }
    if (d->agent_call) {
        return;
    }
    if (!d->session_active) {
        if (d->agent_registered) {
            d->agent_registered = false;
            d->agent_default = false;
            vela_bt_log("agent unregistered: the session is in the background");
            if (vela_bt_call(d, &d->agent_call, "/org/bluez", AGENT_MANAGER, "UnregisterAgent",
                    VELA_BT_STEP_TIMEOUT_US, on_unregistered, d, "o", AGENT_PATH) < 0) {
                vela_bt_log("could not unregister Bluetooth agent");
            }
        }
        return;
    }
    if (d->agent_retry) {
        return;
    }
    if (!d->agent_registered) {
        if (vela_bt_call(d, &d->agent_call, "/org/bluez", AGENT_MANAGER, "RegisterAgent",
                VELA_BT_STEP_TIMEOUT_US, on_registered, d, "os", AGENT_PATH, "KeyboardDisplay") < 0) {
            retry_agent(d);
        }
    } else if (!d->agent_default) {
        if (vela_bt_call(d, &d->agent_call, "/org/bluez", AGENT_MANAGER, "RequestDefaultAgent",
                VELA_BT_STEP_TIMEOUT_US, on_default, d, "o", AGENT_PATH) < 0) {
            retry_agent(d);
        }
    }
}

int vela_bt_agent_start(struct vela_bt_daemon *d)
{
    return sd_bus_add_object_vtable(d->system, NULL, AGENT_PATH, "org.bluez.Agent1", agent_vtable, d);
}

void vela_bt_agent_free(struct vela_bt_daemon *d)
{
    if (d->question) {
        free(finish(d, "bluetooth-gone"));
    }
    d->agent_call = sd_bus_slot_unref(d->agent_call);
    d->agent_retry = sd_event_source_disable_unref(d->agent_retry);
    d->listeners = sd_bus_track_unref(d->listeners);
    d->presenters = sd_bus_track_unref(d->presenters);
}
