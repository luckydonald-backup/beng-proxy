/*
 * Dumping widget information to the log file.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_WIDGET_DUMP_HXX
#define BENG_PROXY_WIDGET_DUMP_HXX

struct pool;
class Istream;
struct Widget;

/**
 * Dump the widget tree to the log file after the istream is done.
 */
Istream *
widget_dump_tree_after_istream(struct pool &pool, Istream &istream,
                               Widget &widget);

#endif