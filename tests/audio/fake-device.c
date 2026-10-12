// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// A fake sound card for the audio tests (docs/audio.md §11): a PipeWire
// device (Audio/Device) with profiles, exported by this client, so that
// WirePlumber and Vela see it like a real card or headset. The fixture's
// null sinks have no device, hence no profiles.
//
// Its profiles: 0 off, 1 output:analog-stereo (the active one at first),
// 2 output:hdmi-stereo (nothing plugged in), 3 a2dp-sink-ldac (a codec).
//
// It speaks lines of text. On stdout:
//   ready <device id>            it exists (sent once)
//   profile <index> <save>       a profile was set; save: 1 if it is to be
//                                remembered (chosen by the user)
// On stdin:
//   available <index> yes|no     that profile can (not) be used now
//   quit (or end of input)       exit
//
// Usage: vela-audio-fake-device [--prop key=value]...

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/param/param.h>
#include <spa/param/profile.h>
#include <spa/pod/builder.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/utils/result.h>

struct profile {
    const char* name;
    const char* description;
    int priority;
    uint32_t available;
};

static struct profile profiles[] = {
    { "off", "Off", 0, SPA_PARAM_AVAILABILITY_yes },
    { "output:analog-stereo", "Analog Stereo Output", 6500, SPA_PARAM_AVAILABILITY_yes },
    { "output:hdmi-stereo", "Digital Stereo (HDMI) Output", 5900, SPA_PARAM_AVAILABILITY_no },
    { "a2dp-sink-ldac", "High Fidelity Playback (A2DP Sink, codec LDAC)", 128, SPA_PARAM_AVAILABILITY_yes },
};
#define N_PROFILES SPA_N_ELEMENTS(profiles)

enum { PARAM_ENUM_PROFILE, PARAM_PROFILE, N_PARAMS };

struct fake {
    struct pw_main_loop* loop;
    struct spa_device device;
    struct spa_hook_list hooks;
    struct pw_properties* props;
    struct spa_param_info params[N_PARAMS];
    uint32_t current;
    bool saved;
    struct pw_proxy* proxy;
    struct spa_hook proxy_listener;
    char input[256];
    size_t input_length;
};

static void emit_info(struct fake* fake, bool full)
{
    struct spa_device_info info = SPA_DEVICE_INFO_INIT();
    info.change_mask = SPA_DEVICE_CHANGE_MASK_PARAMS;
    if (full) {
        info.change_mask |= SPA_DEVICE_CHANGE_MASK_FLAGS | SPA_DEVICE_CHANGE_MASK_PROPS;
        info.props = &fake->props->dict;
    }
    info.params = fake->params;
    info.n_params = N_PARAMS;
    spa_device_emit_info(&fake->hooks, &info);
}

static struct spa_pod* build_profile(struct spa_pod_builder* b, uint32_t id, uint32_t index, bool current, bool save)
{
    const struct profile* p = &profiles[index];
    struct spa_pod_frame f;
    spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_ParamProfile, id);
    spa_pod_builder_add(b, SPA_PARAM_PROFILE_index, SPA_POD_Int(index), SPA_PARAM_PROFILE_name,
        SPA_POD_String(p->name), SPA_PARAM_PROFILE_description, SPA_POD_String(p->description),
        SPA_PARAM_PROFILE_priority, SPA_POD_Int(p->priority), SPA_PARAM_PROFILE_available, SPA_POD_Id(p->available),
        0);
    if (current) {
        spa_pod_builder_add(b, SPA_PARAM_PROFILE_save, SPA_POD_Bool(save), 0);
    }
    return spa_pod_builder_pop(b, &f);
}

static int impl_add_listener(void* object, struct spa_hook* listener, const struct spa_device_events* events, void* data)
{
    struct fake* fake = object;
    struct spa_hook_list save;
    spa_hook_list_isolate(&fake->hooks, &save, listener, events, data);
    emit_info(fake, true);
    spa_hook_list_join(&fake->hooks, &save);
    return 0;
}

static int impl_sync(void* object, int seq)
{
    struct fake* fake = object;
    spa_device_emit_result(&fake->hooks, seq, 0, 0, NULL);
    return 0;
}

static int impl_enum_params(void* object, int seq, uint32_t id, uint32_t start, uint32_t num, const struct spa_pod* filter)
{
    struct fake* fake = object;
    struct spa_result_device_params result = { .id = id, .next = start };
    uint32_t count = 0;
    for (;;) {
        uint8_t buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        struct spa_pod* param;
        result.index = result.next++;
        if (id == SPA_PARAM_EnumProfile && result.index < N_PROFILES) {
            param = build_profile(&b, id, result.index, false, false);
        } else if (id == SPA_PARAM_Profile && result.index == 0) {
            param = build_profile(&b, id, fake->current, true, fake->saved);
        } else {
            return id == SPA_PARAM_EnumProfile || id == SPA_PARAM_Profile ? 0 : -ENOENT;
        }
        if (spa_pod_filter(&b, &result.param, param, filter) < 0) {
            continue;
        }
        spa_device_emit_result(&fake->hooks, seq, 0, SPA_RESULT_TYPE_DEVICE_PARAMS, &result);
        if (++count == num) {
            return 0;
        }
    }
}

static int impl_set_param(void* object, uint32_t id, uint32_t flags, const struct spa_pod* param)
{
    struct fake* fake = object;
    (void)flags;
    if (id != SPA_PARAM_Profile) {
        return -ENOENT;
    }
    int32_t index = -1;
    bool save = false;
    if (!param
        || spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamProfile, NULL, SPA_PARAM_PROFILE_index,
               SPA_POD_Int(&index), SPA_PARAM_PROFILE_save, SPA_POD_OPT_Bool(&save))
            < 0) {
        return -EINVAL;
    }
    if (index < 0 || (uint32_t)index >= N_PROFILES) {
        return -EINVAL;
    }
    fake->current = (uint32_t)index;
    fake->saved = save;
    printf("profile %d %d\n", index, save ? 1 : 0);
    fflush(stdout);
    fake->params[PARAM_PROFILE].flags ^= SPA_PARAM_INFO_SERIAL;
    emit_info(fake, false);
    return 0;
}

static const struct spa_device_methods device_methods = {
    SPA_VERSION_DEVICE_METHODS,
    .add_listener = impl_add_listener,
    .sync = impl_sync,
    .enum_params = impl_enum_params,
    .set_param = impl_set_param,
};

static void on_bound(void* data, uint32_t global_id)
{
    (void)data;
    printf("ready %u\n", global_id);
    fflush(stdout);
}

static void on_proxy_error(void* data, int seq, int res, const char* message)
{
    struct fake* fake = data;
    (void)seq;
    fprintf(stderr, "vela-audio-fake-device: %s (%s)\n", message ? message : "error", spa_strerror(res));
    pw_main_loop_quit(fake->loop);
}

static const struct pw_proxy_events proxy_events = {
    PW_VERSION_PROXY_EVENTS,
    .bound = on_bound,
    .error = on_proxy_error,
};

static void command(struct fake* fake, char* line)
{
    char state[8];
    unsigned index;
    if (strcmp(line, "quit") == 0) {
        pw_main_loop_quit(fake->loop);
    } else if (sscanf(line, "available %u %7s", &index, state) == 2 && index < N_PROFILES) {
        profiles[index].available = strcmp(state, "yes") == 0 ? SPA_PARAM_AVAILABILITY_yes : SPA_PARAM_AVAILABILITY_no;
        fake->params[PARAM_ENUM_PROFILE].flags ^= SPA_PARAM_INFO_SERIAL;
        emit_info(fake, false);
    } else if (line[0]) {
        fprintf(stderr, "vela-audio-fake-device: unknown command '%s'\n", line);
    }
}

static void on_stdin(void* data, int fd, uint32_t mask)
{
    struct fake* fake = data;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP) && !(mask & SPA_IO_IN)) {
        pw_main_loop_quit(fake->loop);
        return;
    }
    const ssize_t n = read(fd, fake->input + fake->input_length, sizeof(fake->input) - 1 - fake->input_length);
    if (n <= 0) {
        if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
            pw_main_loop_quit(fake->loop);
        }
        return;
    }
    fake->input_length += (size_t)n;
    fake->input[fake->input_length] = '\0';
    char* start = fake->input;
    for (char* end; (end = strchr(start, '\n'));) {
        *end = '\0';
        command(fake, start);
        start = end + 1;
    }
    fake->input_length = strlen(start);
    memmove(fake->input, start, fake->input_length + 1);
    if (fake->input_length == sizeof(fake->input) - 1) {
        fake->input_length = 0; // a line that long is no command
    }
}

static void on_signal(void* data, int signal)
{
    (void)signal;
    pw_main_loop_quit(((struct fake*)data)->loop);
}

int main(int argc, char** argv)
{
    struct fake fake = { .current = 1 };
    pw_init(&argc, &argv);
    fake.props = pw_properties_new(PW_KEY_MEDIA_CLASS, "Audio/Device", PW_KEY_DEVICE_NAME, "vela-test-card",
        PW_KEY_DEVICE_DESCRIPTION, "Test Card", PW_KEY_DEVICE_API, "vela-test", NULL);
    for (int i = 1; i < argc; i++) {
        const char* value = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(argv[i], "--prop") == 0 && value && strchr(value, '=')) {
            const char* equals = strchr(value, '=');
            char* key = strndup(value, (size_t)(equals - value));
            pw_properties_set(fake.props, key, equals + 1);
            free(key);
            i++;
        } else {
            fprintf(stderr, "usage: vela-audio-fake-device [--prop key=value]...\n");
            pw_properties_free(fake.props);
            return 2;
        }
    }

    spa_hook_list_init(&fake.hooks);
    fake.device.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Device, SPA_VERSION_DEVICE, &device_methods, &fake);
    fake.params[PARAM_ENUM_PROFILE] = SPA_PARAM_INFO(SPA_PARAM_EnumProfile, SPA_PARAM_INFO_READ);
    fake.params[PARAM_PROFILE] = SPA_PARAM_INFO(SPA_PARAM_Profile, SPA_PARAM_INFO_READWRITE);

    fake.loop = pw_main_loop_new(NULL);
    struct pw_loop* loop = pw_main_loop_get_loop(fake.loop);
    struct pw_context* context = pw_context_new(loop, NULL, 0);
    struct pw_core* core = context ? pw_context_connect(context, NULL, 0) : NULL;
    if (!core) {
        fprintf(stderr, "vela-audio-fake-device: can't connect to PipeWire\n");
        return 1;
    }
    struct spa_source* sources[] = {
        pw_loop_add_signal(loop, SIGINT, on_signal, &fake),
        pw_loop_add_signal(loop, SIGTERM, on_signal, &fake),
        pw_loop_add_io(loop, STDIN_FILENO, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false, on_stdin, &fake),
    };
    fake.proxy = pw_core_export(core, SPA_TYPE_INTERFACE_Device, &fake.props->dict, &fake.device, 0);
    if (!fake.proxy) {
        fprintf(stderr, "vela-audio-fake-device: can't export the device\n");
        return 1;
    }
    pw_proxy_add_listener(fake.proxy, &fake.proxy_listener, &proxy_events, &fake);
    pw_main_loop_run(fake.loop);

    spa_hook_remove(&fake.proxy_listener);
    pw_proxy_destroy(fake.proxy);
    for (size_t i = 0; i < SPA_N_ELEMENTS(sources); i++) {
        pw_loop_destroy_source(loop, sources[i]);
    }
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(fake.loop);
    pw_properties_free(fake.props);
    pw_deinit();
    return 0;
}
