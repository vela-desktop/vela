// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// A fake app for the audio tests (docs/audio.md §11): one PipeWire stream,
// playing a sine of known amplitude or recording, with the properties the
// test chooses (program, app name, portal app id, media role, target...).
//
// It speaks lines of text. On stdout:
//   ready <node id>              the stream exists (sent once)
//   state <paused|streaming|...> every state change
//   peaks <ch0> <ch1>...         recording: each channel's peak, ~10 times
//                                a second
// On stdin:
//   pause | resume               deactivate or activate the stream
//   quit (or end of input)       exit
//
// Usage: vela-audio-fake-stream [--record] [--channels N] [--amplitude A]
//                               [--frequency HZ] [--rate HZ] [--prop key=value]...
//   --amplitude  playback only: 0 (default) plays silence
//   --rate       the stream's sample rate (48000), asked of the graph
//                (node.rate) like pw-cat does
//   --prop       any stream property; repeatable

#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#define DEFAULT_RATE 48000
#define MAX_CHANNELS 8

struct fake {
    struct pw_main_loop* loop;
    struct pw_stream* stream;
    bool record;
    unsigned channels;
    unsigned rate;
    float amplitude;
    float frequency;
    double phase;
    bool ready;
    // Recording: the peaks since the last report.
    float peaks[MAX_CHANNELS];
    unsigned frames;
    char input[256];
    size_t input_length;
};

static void report_ready(struct fake* fake)
{
    const uint32_t id = pw_stream_get_node_id(fake->stream);
    if (!fake->ready && id != SPA_ID_INVALID) {
        fake->ready = true;
        printf("ready %u\n", id);
        fflush(stdout);
    }
}

static void on_state_changed(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error)
{
    struct fake* fake = data;
    (void)old;
    report_ready(fake);
    printf("state %s\n", pw_stream_state_as_string(state));
    fflush(stdout);
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "vela-audio-fake-stream: %s\n", error ? error : "stream error");
        pw_main_loop_quit(fake->loop);
    }
}

static void play(struct fake* fake, struct spa_buffer* buffer)
{
    struct spa_data* d = &buffer->datas[0];
    float* samples = d->data;
    if (!samples) {
        return;
    }
    const uint32_t stride = sizeof(float) * fake->channels;
    const uint32_t frames = d->maxsize / stride;
    const double step = 2.0 * M_PI * fake->frequency / fake->rate;
    for (uint32_t i = 0; i < frames; i++) {
        const float value = fake->amplitude * (float)sin(fake->phase);
        fake->phase = fmod(fake->phase + step, 2.0 * M_PI);
        for (unsigned c = 0; c < fake->channels; c++) {
            samples[i * fake->channels + c] = value;
        }
    }
    d->chunk->offset = 0;
    d->chunk->stride = stride;
    d->chunk->size = frames * stride;
}

static void measure(struct fake* fake, struct spa_buffer* buffer)
{
    const struct spa_data* d = &buffer->datas[0];
    const float* samples = d->data;
    if (!samples) {
        return;
    }
    const uint32_t offset = SPA_MIN(d->chunk->offset, d->maxsize);
    const uint32_t size = SPA_MIN(d->chunk->size, d->maxsize - offset);
    const uint32_t frames = size / (sizeof(float) * fake->channels);
    samples = SPA_PTROFF(samples, offset, const float);
    for (uint32_t i = 0; i < frames; i++) {
        for (unsigned c = 0; c < fake->channels; c++) {
            fake->peaks[c] = fmaxf(fake->peaks[c], fabsf(samples[i * fake->channels + c]));
        }
    }
    fake->frames += frames;
    if (fake->frames >= fake->rate / 10) {
        printf("peaks");
        for (unsigned c = 0; c < fake->channels; c++) {
            printf(" %.4f", fake->peaks[c]);
            fake->peaks[c] = 0.0f;
        }
        printf("\n");
        fflush(stdout);
        fake->frames = 0;
    }
}

static void on_process(void* data)
{
    struct fake* fake = data;
    struct pw_buffer* b = pw_stream_dequeue_buffer(fake->stream);
    if (!b) {
        return;
    }
    if (fake->record) {
        measure(fake, b->buffer);
    } else {
        play(fake, b->buffer);
    }
    pw_stream_queue_buffer(fake->stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .process = on_process,
};

static void command(struct fake* fake, const char* line)
{
    if (strcmp(line, "pause") == 0) {
        pw_stream_set_active(fake->stream, false);
    } else if (strcmp(line, "resume") == 0) {
        pw_stream_set_active(fake->stream, true);
    } else if (strcmp(line, "quit") == 0) {
        pw_main_loop_quit(fake->loop);
    } else if (line[0]) {
        fprintf(stderr, "vela-audio-fake-stream: unknown command '%s'\n", line);
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

static void usage(void)
{
    fprintf(stderr,
        "usage: vela-audio-fake-stream [--record] [--channels N] [--amplitude A]\n"
        "                              [--frequency HZ] [--rate HZ] [--prop key=value]...\n");
}

int main(int argc, char** argv)
{
    struct fake fake = { .channels = 2, .rate = DEFAULT_RATE, .frequency = 440.0f };
    pw_init(&argc, &argv);
    struct pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", NULL);

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(arg, "--record") == 0) {
            fake.record = true;
        } else if (strcmp(arg, "--channels") == 0 && value) {
            fake.channels = (unsigned)atoi(value);
            i++;
        } else if (strcmp(arg, "--amplitude") == 0 && value) {
            fake.amplitude = strtof(value, NULL);
            i++;
        } else if (strcmp(arg, "--rate") == 0 && value) {
            fake.rate = (unsigned)atoi(value);
            pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", fake.rate);
            i++;
        } else if (strcmp(arg, "--frequency") == 0 && value) {
            fake.frequency = strtof(value, NULL);
            i++;
        } else if (strcmp(arg, "--prop") == 0 && value && strchr(value, '=')) {
            const char* equals = strchr(value, '=');
            char* key = strndup(value, (size_t)(equals - value));
            pw_properties_set(props, key, equals + 1);
            free(key);
            i++;
        } else {
            usage();
            pw_properties_free(props);
            return 2;
        }
    }
    if (fake.channels < 1 || fake.channels > MAX_CHANNELS || !(fake.amplitude >= 0.0f && fake.amplitude <= 1.0f)
        || fake.rate < 8000 || fake.rate > 384000 || !(fake.frequency > 0.0f && fake.frequency < fake.rate / 2)) {
        usage();
        pw_properties_free(props);
        return 2;
    }
    if (!pw_properties_get(props, PW_KEY_MEDIA_CATEGORY)) {
        pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, fake.record ? "Capture" : "Playback");
    }
    if (!pw_properties_get(props, PW_KEY_APP_NAME)) {
        pw_properties_set(props, PW_KEY_APP_NAME, "vela-audio-fake-stream");
    }

    fake.loop = pw_main_loop_new(NULL);
    if (!fake.loop) {
        fprintf(stderr, "vela-audio-fake-stream: can't create the loop\n");
        pw_properties_free(props);
        return 1;
    }
    struct pw_loop* loop = pw_main_loop_get_loop(fake.loop);
    struct spa_source* sources[] = {
        pw_loop_add_signal(loop, SIGINT, on_signal, &fake),
        pw_loop_add_signal(loop, SIGTERM, on_signal, &fake),
        pw_loop_add_io(loop, STDIN_FILENO, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false, on_stdin, &fake),
    };

    fake.stream = pw_stream_new_simple(loop, "vela-audio-fake-stream", props, &stream_events, &fake);
    if (!fake.stream) {
        fprintf(stderr, "vela-audio-fake-stream: can't create the stream\n");
        for (size_t i = 0; i < SPA_N_ELEMENTS(sources); i++) {
            pw_loop_destroy_source(loop, sources[i]);
        }
        pw_main_loop_destroy(fake.loop);
        return 1;
    }

    uint8_t buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_audio_info_raw info = {
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = fake.rate,
        .channels = fake.channels,
    };
    // Real positions, so that channel mixing keeps left and right apart.
    static const uint32_t stereo[] = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR };
    static const uint32_t surround[] = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC,
        SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR };
    for (unsigned c = 0; c < fake.channels; c++) {
        info.position[c] = fake.channels == 1 ? SPA_AUDIO_CHANNEL_MONO
            : fake.channels == 2              ? stereo[c]
            : fake.channels == 6              ? surround[c]
                                              : SPA_AUDIO_CHANNEL_AUX0 + c;
    }
    const struct spa_pod* params[1] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info) };
    const int result = pw_stream_connect(fake.stream, fake.record ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
        PW_ID_ANY, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1);
    if (result < 0) {
        fprintf(stderr, "vela-audio-fake-stream: can't connect: %s\n", spa_strerror(result));
    } else {
        pw_main_loop_run(fake.loop);
    }

    pw_stream_destroy(fake.stream);
    for (size_t i = 0; i < SPA_N_ELEMENTS(sources); i++) {
        pw_loop_destroy_source(loop, sources[i]);
    }
    pw_main_loop_destroy(fake.loop);
    pw_deinit();
    return result < 0 ? 1 : 0;
}
