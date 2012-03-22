/*
 * Definitions for the beng-proxy translation protocol.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_PROXY_TRANSLATION_H
#define __BENG_PROXY_TRANSLATION_H

#include <stdint.h>

enum beng_translation_command {
    TRANSLATE_BEGIN = 1,
    TRANSLATE_END = 2,
    TRANSLATE_HOST = 3,
    TRANSLATE_URI = 4,
    TRANSLATE_STATUS = 5,
    TRANSLATE_PATH = 6,
    TRANSLATE_CONTENT_TYPE = 7,
    TRANSLATE_PROXY = 8,
    TRANSLATE_REDIRECT = 9,
    TRANSLATE_FILTER = 10,
    TRANSLATE_PROCESS = 11,
    TRANSLATE_SESSION = 12,
    TRANSLATE_PARAM = 13,
    TRANSLATE_USER = 14,
    TRANSLATE_LANGUAGE = 15,
    TRANSLATE_REMOTE_HOST = 16,
    TRANSLATE_PATH_INFO = 17,
    TRANSLATE_SITE = 18,
    TRANSLATE_CGI = 19,
    TRANSLATE_DOCUMENT_ROOT = 20,
    TRANSLATE_WIDGET_TYPE = 21,
    TRANSLATE_CONTAINER = 22,
    TRANSLATE_ADDRESS = 23,
    TRANSLATE_ADDRESS_STRING = 24,
    TRANSLATE_JAILCGI = 26,
    TRANSLATE_INTERPRETER = 27,
    TRANSLATE_ACTION = 28,
    TRANSLATE_SCRIPT_NAME = 29,
    TRANSLATE_AJP = 30,

    /** deprecated */
    TRANSLATE_DOMAIN = 31,

    TRANSLATE_STATEFUL = 32,
    TRANSLATE_FASTCGI = 33,
    TRANSLATE_VIEW = 34,
    TRANSLATE_USER_AGENT = 35,
    TRANSLATE_MAX_AGE = 36,
    TRANSLATE_VARY = 37,
    TRANSLATE_QUERY_STRING = 38,
    TRANSLATE_PIPE = 39,
    TRANSLATE_BASE = 40,
    TRANSLATE_DELEGATE = 41,
    TRANSLATE_INVALIDATE = 42,
    TRANSLATE_LOCAL_ADDRESS = 43,
    TRANSLATE_LOCAL_ADDRESS_STRING = 44,
    TRANSLATE_APPEND = 45,
    TRANSLATE_DISCARD_SESSION = 46,
    TRANSLATE_SCHEME = 47,
    TRANSLATE_REQUEST_HEADER_FORWARD = 48,
    TRANSLATE_RESPONSE_HEADER_FORWARD = 49,
    TRANSLATE_DEFLATED = 50,
    TRANSLATE_GZIPPED = 51,
    TRANSLATE_PAIR = 52,
    TRANSLATE_UNTRUSTED = 53,
    TRANSLATE_BOUNCE = 54,
    TRANSLATE_ARGS = 55,

    /**
     * The value of the "WWW-Authenticate" HTTP response header.
     */
    TRANSLATE_WWW_AUTHENTICATE = 56,

    /**
     * The value of the "Authentication-Info" HTTP response header.
     */
    TRANSLATE_AUTHENTICATION_INFO = 57,

    /**
     * The value of the "Authorization" HTTP request header.
     */
    TRANSLATE_AUTHORIZATION = 58,

    /**
     * A custom HTTP response header sent to the client.
     */
    TRANSLATE_HEADER = 59,

    TRANSLATE_UNTRUSTED_PREFIX = 60,

    /**
     * Set the "secure" flag on the session cookie.
     */
    TRANSLATE_SECURE_COOKIE = 61,

    /**
     * Enable filtering of client errors (status 4xx).  Without this
     * flag, only successful responses (2xx) are filtered.  Only
     * useful when at least one FILTER was specified.
     */
    TRANSLATE_FILTER_4XX = 62,

    /**
     * Support for custom error documents.  In the response, this is a
     * flag which enables custom error documents (i.e. if the HTTP
     * response is not successful, the translation server is asked to
     * provide a custom error document).  In a request, it queries the
     * location of the error document.
     */
    TRANSLATE_ERROR_DOCUMENT = 63,

    /**
     * Response: causes beng-proxy to submit the same translation
     * request again, with this packet appended.  The current response
     * is remembered, to be used when the second response contains the
     * PREVIOUS packet.
     *
     * Request: repeated request after CHECK was received.  The server
     * may respond with PREVIOUS.
     */
    TRANSLATE_CHECK = 64,

    /**
     * Tells beng-proxy to use the resource address of the previous
     * translation response.
     */
    TRANSLATE_PREVIOUS = 65,

    /**
     * Launch a WAS application to handle the request.
     */
    TRANSLATE_WAS = 66,

    /**
     * The absolute location of the home directory of the site owner
     * (hosting account).
     */
    TRANSLATE_HOME = 67,

    /**
     * Specifies the session realm.  An existing session matches only
     * if its realm matches the current request's realm.
     */
    TRANSLATE_REALM = 68,

    TRANSLATE_UNTRUSTED_SITE_SUFFIX = 69,

    /**
     * Transparent proxy: forward URI arguments to the request handler
     * instead of using them.
     */
    TRANSLATE_TRANSPARENT = 70,

    /**
     * Make the resource address "sticky", i.e. attempt to forward all
     * requests of a session to the same worker.
     */
    TRANSLATE_STICKY = 71,

    /**
     * Enable header dumps for the widget: on a HTTP request, the
     * request and response headers will be logged.  Only for
     * debugging purposes.
     */
    TRANSLATE_DUMP_HEADERS = 72,

    /**
     * Override the cookie host name.  This host name is used for
     * storing and looking up cookies in the jar.  It is especially
     * useful for protocols that don't have a host name, such as CGI.
     */
    TRANSLATE_COOKIE_HOST = 73,

    /**
     * Run the CSS processor.
     */
    TRANSLATE_PROCESS_CSS = 74,

    /**
     * Rewrite CSS class names with a leading underscore?
     */
    TRANSLATE_PREFIX_CSS_CLASS = 75,

    /**
     * Default URI rewrite mode is base=widget mode=focus.
     */
    TRANSLATE_FOCUS_WIDGET = 76,

    /**
     * Absolute URI paths are considered relative to the base URI of
     * the widget.
     */
    TRANSLATE_ANCHOR_ABSOLUTE = 77,

    /**
     * Rewrite XML ids with a leading underscore?
     */
    TRANSLATE_PREFIX_XML_ID = 78,

    /**
     * Reuse a cached response only if the request \verb|URI| matches
     * the specified regular expression (Perl compatible).
     */
    TRANSLATE_REGEX = 79,

    /**
     * Don't apply the cached response if the request URI matches the
     * specified regular expression (Perl compatible).
     */
    TRANSLATE_INVERSE_REGEX = 80,

    /**
     * Run the text processor to expand entity references.
     */
    TRANSLATE_PROCESS_TEXT = 81,

    /**
     * Send widget metadata (id, prefix, type) to the widget server.
     */
    TRANSLATE_WIDGET_INFO = 82,

    /**
     * Expand #TRANSLATE_REGEX match strings in this PATH_INFO value.
     * Sub-strings in the form "\1" will be replaced.  It can be used
     * to copy URI parts to a filter.
     */
    TRANSLATE_EXPAND_PATH_INFO = 83,

    /**
     * Expand #TRANSLATE_REGEX match strings in this PATH value (only
     * CGI, FastCGI, WAS).  Sub-strings in the form "\1" will be
     * replaced.
     */
    TRANSLATE_EXPAND_PATH = 84,

    /**
     * Set the session cookie's "Domain" attribute.
     */
    TRANSLATE_COOKIE_DOMAIN = 85,

    /**
     * The URI of the "local" location of a widget class.  This may
     * refer to a location that serves static resources.  It is used
     * by the processor for rewriting URIs.
     */
    TRANSLATE_LOCAL_URI = 86,

    /**
     * Enable CGI auto-base.
     */
    TRANSLATE_AUTO_BASE = 87,

    /**
     * A classification for the User-Agent request header.
     */
    TRANSLATE_UA_CLASS = 88,

    /**
     * Mark the current view as "secure", i.e. it may not be requested
     * by the client.
     */
    TRANSLATE_SECURE_VIEW = 89,
};

struct beng_translation_header {
    uint16_t length;
    uint16_t command;
};

#endif
