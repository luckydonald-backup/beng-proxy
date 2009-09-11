/*
 * Write an AJP stream.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "ajp-serialize.h"
#include "serialize.h"
#include "growing-buffer.h"
#include "strref.h"

#include <netinet/in.h>
#include <string.h>

void
serialize_ajp_string(struct growing_buffer *gb, const char *s)
{
    size_t length = strlen(s);
    char *p;

    if (length > 0xffff)
        length = 0xffff; /* XXX too long, cut off */

    p = growing_buffer_write(gb, 2 + length + 1);
    *(uint16_t*)p = htons(length);
    memcpy(p + 2, s, length + 1);
}

void
serialize_ajp_integer(struct growing_buffer *gb, int i)
{
    serialize_uint16(gb, i);
}

void
serialize_ajp_bool(struct growing_buffer *gb, bool b)
{
    bool *p;

    p = growing_buffer_write(gb, sizeof(*p));
    *p = b ? 1 : 0;
}

const char *
deserialize_ajp_string(struct strref *input)
{
    size_t length = deserialize_uint16(input);
    const char *value;

    if (input->length <= length || input->data[length] != 0) {
        strref_null(input);
        return NULL;
    }

    value = input->data;
    strref_skip(input, length + 1);
    return value;
}
