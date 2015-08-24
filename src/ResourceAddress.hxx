/*
 * Address of a resource, which might be a local file, a CGI script or
 * a HTTP server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_RESOURCE_ADDRESS_HXX
#define BENG_PROXY_RESOURCE_ADDRESS_HXX

#include "glibfwd.hxx"

#include <inline/compiler.h>

#include <cstddef>

#include <assert.h>

struct pool;
struct strref;
class MatchInfo;
class Error;

enum resource_address_type {
    RESOURCE_ADDRESS_NONE = 0,
    RESOURCE_ADDRESS_LOCAL,
    RESOURCE_ADDRESS_HTTP,
    RESOURCE_ADDRESS_LHTTP,
    RESOURCE_ADDRESS_PIPE,
    RESOURCE_ADDRESS_CGI,
    RESOURCE_ADDRESS_FASTCGI,
    RESOURCE_ADDRESS_WAS,
    RESOURCE_ADDRESS_AJP,
    RESOURCE_ADDRESS_NFS,
};

struct ResourceAddress {
    enum resource_address_type type;

    union U {
        const struct file_address *file;

        const struct http_address *http;

        const struct lhttp_address *lhttp;

        const struct cgi_address *cgi;

        const struct nfs_address *nfs;

        U() = default;
        constexpr U(std::nullptr_t n):file(n) {}
        constexpr U(const struct file_address &_file):file(&_file) {}
        constexpr U(const struct http_address &_http):http(&_http) {}
        constexpr U(const struct lhttp_address &_lhttp):lhttp(&_lhttp) {}
        constexpr U(const struct cgi_address &_cgi):cgi(&_cgi) {}
        constexpr U(const struct nfs_address &_nfs):nfs(&_nfs) {}
    } u;

    ResourceAddress() = default;

    explicit constexpr ResourceAddress(std::nullptr_t n)
      :type(RESOURCE_ADDRESS_NONE), u(n) {}

    explicit constexpr ResourceAddress(enum resource_address_type _type)
      :type(_type), u(nullptr) {}

    explicit constexpr ResourceAddress(const struct file_address &file)
      :type(RESOURCE_ADDRESS_LOCAL), u(file) {}

    constexpr ResourceAddress(enum resource_address_type _type,
                              const struct http_address &http)
      :type(_type), u(http) {}

    explicit constexpr ResourceAddress(const struct lhttp_address &lhttp)
      :type(RESOURCE_ADDRESS_LHTTP), u(lhttp) {}

    constexpr ResourceAddress(enum resource_address_type _type,
                              const struct cgi_address &cgi)
      :type(_type), u(cgi) {}

    explicit constexpr ResourceAddress(const struct nfs_address &nfs)
      :type(RESOURCE_ADDRESS_NFS), u(nfs) {}

    void Clear() {
        type = RESOURCE_ADDRESS_NONE;
    }

    bool Check(GError **error_r) const;

    gcc_pure
    bool HasQueryString() const;

    gcc_pure
    bool IsValidBase() const;

    /**
     * Copies data from #src for storing in the translation cache.
     *
     * @return true if a #base was given and it was applied
     * successfully
     */
    bool CacheStore(struct pool *pool,
                    const ResourceAddress *src,
                    const char *uri, const char *base,
                    bool easy_base, bool expandable);

    /**
     * Load an address from a cached object, and apply any BASE
     * changes (if a BASE is present).
     */
    bool CacheLoad(struct pool *pool, const ResourceAddress &src,
                   const char *uri, const char *base,
                   bool unsafe_base, bool expandable,
                   GError **error_r);
};

/**
 * Is this a CGI address, or a similar protocol?
 */
static inline bool
resource_address_is_cgi_alike(const ResourceAddress *address)
{
    return address->type == RESOURCE_ADDRESS_CGI ||
        address->type == RESOURCE_ADDRESS_FASTCGI ||
        address->type == RESOURCE_ADDRESS_WAS;
}

gcc_const
static inline struct file_address *
resource_address_get_file(ResourceAddress *address)
{
    assert(address->type == RESOURCE_ADDRESS_LOCAL);

    return const_cast<struct file_address *>(address->u.file);
}

gcc_const
static inline struct cgi_address *
resource_address_get_cgi(ResourceAddress *address)
{
    assert(resource_address_is_cgi_alike(address));

    return const_cast<struct cgi_address *>(address->u.cgi);
}

void
resource_address_copy(struct pool &pool, ResourceAddress *dest,
                      const ResourceAddress *src);

gcc_malloc
ResourceAddress *
resource_address_dup(struct pool &pool, const ResourceAddress *src);

/**
 * Duplicate the #resource_address object, but replace the HTTP/AJP
 * URI path component.
 */
gcc_malloc
ResourceAddress *
resource_address_dup_with_path(struct pool &pool,
                               const ResourceAddress *src,
                               const char *path);

/**
 * Duplicate this #resource_address object, and inserts the query
 * string from the specified URI.  If this resource address does not
 * support a query string, or if the URI does not have one, the
 * original #resource_address pointer is returned.
 */
gcc_pure gcc_malloc
const ResourceAddress *
resource_address_insert_query_string_from(struct pool &pool,
                                          const ResourceAddress *src,
                                          const char *uri);

/**
 * Duplicate this #resource_address object, and inserts the URI
 * arguments and the path suffix.  If this resource address does not
 * support the operation, the original #resource_address pointer may
 * be returned.
 */
gcc_pure gcc_malloc
const ResourceAddress *
resource_address_insert_args(struct pool &pool,
                             const ResourceAddress *src,
                             const char *args, size_t args_length,
                             const char *path, size_t path_length);

/**
 * Check if a "base" URI can be generated automatically from this
 * #resource_address.  This applies when the CGI's PATH_INFO matches
 * the end of the specified URI.
 *
 * @param uri the request URI
 * @return a newly allocated base, or NULL if that is not possible
 */
gcc_malloc
char *
resource_address_auto_base(struct pool *pool,
                           const ResourceAddress *address,
                           const char *uri);

/**
 * Duplicate a resource address, but return the base address.
 *
 * @param src the original resource address
 * @param suffix the suffix to be removed from #src
 * @return NULL if the suffix does not match, or if this address type
 * cannot have a base address
 */
gcc_malloc
ResourceAddress *
resource_address_save_base(struct pool *pool, ResourceAddress *dest,
                           const ResourceAddress *src,
                           const char *suffix);

/**
 * Duplicate a resource address, and append a suffix.
 *
 * Warning: this function does not check for excessive "../"
 * sub-strings.
 *
 * @param src the base resource address (must end with a slash)
 * @param suffix the suffix to be addded to #src
 * @return NULL if this address type cannot have a base address
 */
gcc_malloc
ResourceAddress *
resource_address_load_base(struct pool *pool, ResourceAddress *dest,
                           const ResourceAddress *src,
                           const char *suffix);

gcc_pure
const ResourceAddress *
resource_address_apply(struct pool *pool, const ResourceAddress *src,
                       const char *relative, size_t relative_length,
                       ResourceAddress *buffer);

gcc_pure
const struct strref *
resource_address_relative(const ResourceAddress *base,
                          const ResourceAddress *address,
                          struct strref *buffer);

/**
 * Generates a string identifying the address.  This can be used as a
 * key in a hash table.
 */
gcc_pure
const char *
resource_address_id(const ResourceAddress *address, struct pool *pool);

/**
 * Determine the URI path.  May return NULL if unknown or not
 * applicable.
 */
gcc_pure
const char *
resource_address_host_and_port(const ResourceAddress *address);

/**
 * Determine the URI path.  May return NULL if unknown or not
 * applicable.
 */
gcc_pure
const char *
resource_address_uri_path(const ResourceAddress *address);

/**
 * Does this address need to be expanded with
 * resource_address_expand()?
 */
gcc_pure
bool
resource_address_is_expandable(const ResourceAddress *address);

/**
 * Expand the expand_path_info attribute.
 */
bool
resource_address_expand(struct pool *pool, ResourceAddress *address,
                        const MatchInfo &match_info, Error &error_r);

#endif
