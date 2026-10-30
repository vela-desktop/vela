// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "server.h"
#include "error_screen.h"
#include "supervisor.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <wlr/util/log.h>

static void print_usage(const char *program)
{
    printf("Usage: %s [--supervise] [-s command]\n"
           "\n"
           "  -s command   runs the command at startup (e.g. the shell: -s vela-shell)\n"
           "               and restarts it if it exits with an error\n"
           "  --supervise  the real session (vela-session): a separate process holds\n"
           "               the Wayland socket and restarts the compositor if it crashes;\n"
           "               Qt and KDE apps reconnect\n"
           "  -h           shows this help\n"
           "\n"
           "Environment variables:\n"
           "  VELA_TERMINAL      terminal for Alt/Super+Enter (default: konsole)\n"
           "  VELA_SCALE         output scale (e.g. 1.25, or DP-1=1.5,HDMI-A-1=1);\n"
           "                     without it, Vela picks one from each output's DPI\n"
           "  VELA_VRR=1|0       variable refresh rate always on, or never\n"
           "  VELA_LATCH=0       draw as soon as the vblank arrives, instead of as late\n"
           "                     as possible before the next one (late latching)\n"
           "  VELA_LATCH_MARGIN  minimum late-latching margin in ms (default 1)\n"
           "  VELA_SCANOUT=0     no direct scanout for full-screen apps\n"
           "  VELA_READY_WAIT=0  apply app commits right away, even if their GPU hasn't\n"
           "                     finished (the frame waits for it: for comparison)\n"
           "  VELA_REALTIME=0    no realtime scheduling for the compositor\n"
           "  VELA_SCREEN_OFF    idle minutes before locking and turning screens off (default 10, 0: never;\n"
           "                     otherwise screen-off= in ~/.config/vela/vela.conf)\n"
           "  VELA_LOCK_ON_IDLE=0  when idle, turn screens off without locking\n"
           "  VELA_LOCK          locker to use instead of vela-lock\n"
           "  VELA_POLKIT_AGENT  polkit agent to start instead of vela-polkit-agent, 0: none\n"
           "                     (by default only in a real session; polkit-agent= in vela.conf)\n"
           "  VELA_BLUETOOTH     Bluetooth daemon to start instead of vela-bluetooth, 0: none\n"
           "                     (by default only in a real session)\n"
           "  VELA_VULKAN_VALIDATION=1  Vulkan validation layers (for development)\n"
           "  VELA_SKIP_VULKAN_CLEANUP=1  diagnostic: skip explicit Vulkan cleanup on exit\n"
           "  VELA_NATURAL_SCROLL=0  classic touchpad scrolling\n"
           "  VELA_STATS=1       every 2 s, per output: fps, frame cost, latency, missed vblanks\n"
           "  VELA_DEBUG=1       verbose log\n"
           "  XKB_DEFAULT_LAYOUT keyboard layout (e.g. it)\n",
        program);
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--supervise") == 0) {
            return vela_supervise(argc, argv);
        }
    }

    const char *startup = NULL;
    int option;
    while ((option = getopt(argc, argv, "s:h")) != -1) {
        switch (option) {
        case 's':
            startup = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    // wl_event_loop_add_signal() blocks a signal only on the calling thread.
    // Vulkan/DRM may start worker threads *before* the event loop registers
    // its handlers. Block SIGUSR1 (redraw) and SIGINT/SIGTERM (clean exit,
    // server.c) before initializing those libraries, so their threads inherit
    // the mask and process-directed signals reach Wayland's signalfd instead
    // of taking the default action (terminating without a clean exit).
    // Vela's fork/exec helpers clear signal masks in launched applications.
    sigset_t handled_signals;
    sigemptyset(&handled_signals);
    sigaddset(&handled_signals, SIGUSR1);
    sigaddset(&handled_signals, SIGINT);
    sigaddset(&handled_signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &handled_signals, NULL) != 0) {
        perror("vela-compositor: cannot block signals");
        return 1;
    }

    // Late latching wakes the compositor at a precise moment before the
    // vblank: the kernel must not round timers (50 µs by default).
    prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);

    const char *debug = getenv("VELA_DEBUG");
    wlr_log_init(debug && *debug ? WLR_DEBUG : WLR_INFO, NULL);

    bool unsupported_gpu = false;
    struct vela_server *server = vela_server_create(&unsupported_gpu);
    if (unsupported_gpu) {
        return VELA_EXIT_UNSUPPORTED_GPU;
    }
    if (!server || !vela_server_start(server, startup)) {
        return 1;
    }
    vela_server_run(server);
    bool device_lost = vela_vulkan_device_lost(server->vulkan);
    vela_server_destroy(server);
    return device_lost ? VELA_EXIT_DEVICE_LOST : 0;
}
