// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "buffer.h"
#include "alloc/alloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void reserve(struct vela_buffer *buffer, size_t more)
{
    // Overflow aborts here instead of wrapping to a small size.
    size_t needed = vela_size_add(vela_size_add(buffer->length, more), 1);
    buffer->data = vela_grow(buffer->data, &buffer->capacity, needed, 1);
}

void vela_buffer_finish(struct vela_buffer *buffer)
{
    free(buffer->data);
    *buffer = (struct vela_buffer) { 0 };
}

void vela_buffer_append(struct vela_buffer *buffer, const char *text)
{
    size_t length = strlen(text);
    reserve(buffer, length);
    memcpy(buffer->data + buffer->length, text, length + 1);
    buffer->length += length;
}

void vela_buffer_appendf(struct vela_buffer *buffer, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int length = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (length < 0) {
        return;
    }
    reserve(buffer, (size_t)length);
    va_start(args, format);
    vsnprintf(buffer->data + buffer->length, (size_t)length + 1, format, args);
    va_end(args);
    buffer->length += (size_t)length;
}

void vela_buffer_append_json(struct vela_buffer *buffer, const char *text)
{
    vela_buffer_append(buffer, "\"");
    for (const char *c = text ? text : ""; *c; ++c) {
        switch (*c) {
        case '"':
            vela_buffer_append(buffer, "\\\"");
            break;
        case '\\':
            vela_buffer_append(buffer, "\\\\");
            break;
        case '\n':
            vela_buffer_append(buffer, "\\n");
            break;
        case '\t':
            vela_buffer_append(buffer, "\\t");
            break;
        default:
            if ((unsigned char)*c < 0x20) {
                vela_buffer_appendf(buffer, "\\u%04x", *c);
            } else {
                char one[2] = { *c, '\0' };
                vela_buffer_append(buffer, one);
            }
        }
    }
    vela_buffer_append(buffer, "\"");
}

const char *vela_buffer_text(const struct vela_buffer *buffer)
{
    return buffer->data ? buffer->data : "";
}
