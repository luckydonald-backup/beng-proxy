/*
 * Session management.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_SESSION_H
#define __BENG_SESSION_H

#include "lock.h"

#include <inline/list.h>
#include <inline/compiler.h>

#include <time.h>
#include <stdbool.h>

struct dpool;
struct dhashmap;

typedef unsigned session_id_t;

/**
 * Session data associated with a widget instance (struct widget).
 */
struct widget_session {
    struct widget_session *parent;

    struct session *session;

    /** local id of this widget; must not be NULL since widgets
        without an id cannot have a session */
    const char *id;
    struct dhashmap *children;

    /** last relative URI */
    char *path_info;

    /** last query string */
    char *query_string;
};

/**
 * A session associated with a user.
 */
struct session {
    struct list_head hash_siblings;

    struct dpool *pool;

    /** this lock protects the bit fields, all widget session hash
        maps and the cookie jar */
    struct lock lock;

    /** identification number of this session in the URI */
    session_id_t uri_id;

    /** identification number of this session in the cookie */
    session_id_t cookie_id;

    /** when will this session expire? */
    time_t expires;

    /** has a HTTP cookie with this session id already been sent? */
    bool cookie_sent:1;

    /** has a HTTP cookie with this session id already been received? */
    bool cookie_received:1;

    /** an opaque string for the translation server */
    const char *translate;

    /** the user name which is logged in (NULL if anonymous), provided
        by the translation server */
    const char *user;

    /** optional  for the "Accept-Language" header, provided
        by the translation server */
    const char *language;

    /** a map of widget path to struct widget_session */
    struct dhashmap *widgets;

    /** all cookies received by widget servers */
    struct cookie_jar *cookies;
};

/**
 * Initialize the global session manager or increase the reference
 * counter.
 */
bool
session_manager_init(void);

/**
 * Decrease the reference counter and destroy the global session
 * manager if it has become zero.
 */
void
session_manager_deinit(void);

/**
 * Release the session manager and try not to access the shared
 * memory, because we assume it may be corrupted.
 */
void
session_manager_abandon(void);

/**
 * Re-add all libevent events after session_manager_event_del().
 */
void
session_manager_event_add(void);

/**
 * Removes all libevent events.  Call this before fork(), or before
 * creating a new event base.  Don't forget to call
 * session_manager_event_add() afterwards.
 */
void
session_manager_event_del(void);

session_id_t
session_id_parse(const char *p);

void
session_id_format(char dest[9], session_id_t id);

struct session * __attr_malloc
session_new(void);

struct session * __attr_malloc
session_dup(const struct session *src);

struct session *
session_get(session_id_t id);

struct widget_session *
session_get_widget(struct session *session, const char *id, bool create);

struct widget_session *
widget_session_get_child(struct widget_session *parent, const char *id,
                         bool create);

#endif
