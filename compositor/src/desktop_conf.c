// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "desktop_conf.h"

#include "config.h"

#include <stdlib.h>
#include <string.h>

bool vela_desktop_conf_read(const char *path, const struct vela_desktop_conf_reader *reader)
{
    bool missing = false;
    FILE *in = vela_config_open(path, &missing);
    if (!in) {
        return missing;
    }
    char *line = NULL;
    size_t size = 0;
    ssize_t length = 0;
    int status;
    while ((status = vela_read_line(in, &line, &size, &length)) > 0) {
        if (length > 0 && line[length - 1] == '\n') {
            line[--length] = '\0';
        }
        if (strncmp(line, "desktop=", 8) == 0) {
            reader->desktop(reader->data, line + 8);
        } else if (strncmp(line, "sticky-app=", 11) == 0 && length > 11) {
            reader->sticky_app(reader->data, line + 11);
        } else if (strncmp(line, "app-ovunque=", 12) == 0 && length > 12) {
            reader->sticky_app(reader->data, line + 12);
        }
    }
    free(line);
    fclose(in);
    return status == 0;
}
