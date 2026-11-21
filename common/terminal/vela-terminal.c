// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-terminal: opens the terminal of docs/keyboard.md §11, empty or
// running a command. The compositor starts it for Win+Enter, so that the
// lookup's directory reads stay out of its event loop.
//
//   vela-terminal                  an empty terminal
//   vela-terminal -- COMMAND...    the terminal running COMMAND
//   vela-terminal --print [-- ...] what it would run, one word per line,
//                                  after "source <n>" and "id <desktop ID>"

#include "terminal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    bool print = false;
    int first = argc;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--print") == 0) {
            print = true;
        } else if (strcmp(argv[i], "--") == 0) {
            first = i + 1;
            break;
        } else {
            fprintf(stderr, "Usage: vela-terminal [--print] [-- COMMAND...]\n");
            return 2;
        }
    }
    struct vela_terminal_env env;
    vela_terminal_env_get(&env);
    struct vela_terminal terminal;
    if (!vela_terminal_find(&env, &terminal)) {
        fprintf(stderr, "vela-terminal: no terminal is installed\n");
        return 1;
    }
    char **command = vela_terminal_argv(&terminal, (const char *const *)argv + first);
    if (print) {
        printf("source %d\nid %s\n", (int)terminal.source, terminal.id ? terminal.id : "");
        for (char **word = command; *word; ++word) {
            printf("%s\n", *word);
        }
        vela_terminal_strv_free(command);
        vela_terminal_free(&terminal);
        return 0;
    }
    execv(command[0], command);
    perror(command[0]);
    vela_terminal_strv_free(command);
    vela_terminal_free(&terminal);
    return 127;
}
