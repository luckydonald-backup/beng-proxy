/*
 * Configuration.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_CONFIG_H
#define __BENG_CONFIG_H

#include <sys/types.h>

#ifdef NDEBUG
static const int debug_mode = 0;
#else
extern int debug_mode;
#endif

struct config {
    unsigned port;

    const char *document_root;

    const char *translation_socket;

    unsigned num_workers;
};

void
parse_cmdline(struct config *config, int argc, char **argv);

#endif
