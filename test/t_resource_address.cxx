#include "ResourceAddress.hxx"
#include "file_address.hxx"
#include "cgi_address.hxx"
#include "pool.hxx"

#include <assert.h>
#include <string.h>

static void
test_auto_base(struct pool *pool)
{
    static const struct cgi_address cgi0 = {
        .path = "/usr/lib/cgi-bin/foo.pl",
        .path_info = "/",
    };
    static constexpr ResourceAddress ra0(RESOURCE_ADDRESS_CGI, cgi0);

    assert(resource_address_auto_base(pool, &ra0, "/") == NULL);
    assert(resource_address_auto_base(pool, &ra0, "/foo") == NULL);

    static const struct cgi_address cgi1 = {
        .path = "/usr/lib/cgi-bin/foo.pl",
        .path_info = "foo/bar",
    };
    static constexpr ResourceAddress ra1(RESOURCE_ADDRESS_CGI, cgi1);

    assert(resource_address_auto_base(pool, &ra1, "/") == NULL);
    assert(resource_address_auto_base(pool, &ra1, "/foo/bar") == NULL);

    static const struct cgi_address cgi2 = {
        .path = "/usr/lib/cgi-bin/foo.pl",
        .path_info = "/bar/baz",
    };
    static constexpr ResourceAddress ra2(RESOURCE_ADDRESS_CGI, cgi2);

    assert(resource_address_auto_base(pool, &ra2, "/") == NULL);
    assert(resource_address_auto_base(pool, &ra2, "/foobar/baz") == NULL);

    char *a = resource_address_auto_base(pool, &ra2, "/foo/bar/baz");
    assert(a != NULL);
    assert(strcmp(a, "/foo/") == 0);
}

static void
test_base_no_path_info(struct pool *pool)
{
    static const struct cgi_address cgi0 = {
        .path = "/usr/lib/cgi-bin/foo.pl",
    };
    static constexpr ResourceAddress ra0(RESOURCE_ADDRESS_CGI, cgi0);

    ResourceAddress dest, *b;

    b = resource_address_save_base(pool, &dest, &ra0, "");
    assert(b != nullptr);
    assert(b->type == RESOURCE_ADDRESS_CGI);
    assert(strcmp(b->u.cgi->path, ra0.u.cgi->path) == 0);
    assert(b->u.cgi->path_info == nullptr ||
           strcmp(b->u.cgi->path_info, "") == 0);

    b = resource_address_load_base(pool, &dest, &ra0, "foo/bar");
    assert(b != nullptr);
    assert(b->type == RESOURCE_ADDRESS_CGI);
    assert(strcmp(b->u.cgi->path, ra0.u.cgi->path) == 0);
    assert(strcmp(b->u.cgi->path_info, "foo/bar") == 0);
}

static void
test_cgi_apply(struct pool *pool)
{
    static const struct cgi_address cgi0 = {
        .path = "/usr/lib/cgi-bin/foo.pl",
        .path_info = "/foo/",
    };
    static constexpr ResourceAddress ra0(RESOURCE_ADDRESS_CGI, cgi0);

    ResourceAddress buffer;
    const ResourceAddress *result;

    result = resource_address_apply(pool, &ra0, "", 0, &buffer);
    assert(result == &ra0);

    result = resource_address_apply(pool, &ra0, "bar", 3, &buffer);
    assert(strcmp(result->u.cgi->path_info, "/foo/bar") == 0);

    result = resource_address_apply(pool, &ra0, "/bar", 4, &buffer);
    assert(strcmp(result->u.cgi->path_info, "/bar") == 0);

    /* PATH_INFO is unescaped (RFC 3875 4.1.5) */
    result = resource_address_apply(pool, &ra0, "bar%2etxt", 9, &buffer);
    assert(strcmp(result->u.cgi->path_info, "/foo/bar.txt") == 0);

    result = resource_address_apply(pool, &ra0, "http://localhost/", 17, &buffer);
    assert(result == nullptr);
}

/*
 * main
 *
 */

int main(int argc, char **argv) {
    struct pool *pool;
    static const struct file_address file1("/var/www/foo/bar.html");
    static constexpr ResourceAddress ra1(file1);

    static const struct file_address file2("/var/www/foo/space .txt");
    static constexpr ResourceAddress ra2(file2);

    static const struct cgi_address cgi3 = {
        .path = "/usr/lib/cgi-bin/foo.pl",
        .uri = "/foo/bar/baz",
        .path_info = "/bar/baz",
    };
    static constexpr ResourceAddress ra3(RESOURCE_ADDRESS_CGI, cgi3);

    ResourceAddress *a, *b, dest, dest2;

    (void)argc;
    (void)argv;

    pool = pool_new_libc(NULL, "root");

    a = resource_address_save_base(pool, &dest2, &ra1, "bar.html");
    assert(a != NULL);
    assert(a->type == RESOURCE_ADDRESS_LOCAL);
    assert(strcmp(a->u.file->path, "/var/www/foo/") == 0);

    b = resource_address_load_base(pool, &dest, a, "index.html");
    assert(b != NULL);
    assert(b->type == RESOURCE_ADDRESS_LOCAL);
    assert(strcmp(b->u.file->path, "/var/www/foo/index.html") == 0);

    a = resource_address_save_base(pool, &dest2, &ra2, "space%20.txt");
    assert(a != NULL);
    assert(a->type == RESOURCE_ADDRESS_LOCAL);
    assert(strcmp(a->u.file->path, "/var/www/foo/") == 0);

    b = resource_address_load_base(pool, &dest, a, "index%2ehtml");
    assert(b != NULL);
    assert(b->type == RESOURCE_ADDRESS_LOCAL);
    assert(strcmp(b->u.file->path, "/var/www/foo/index.html") == 0);

    a = resource_address_save_base(pool, &dest2, &ra3, "bar/baz");
    assert(a != NULL);
    assert(a->type == RESOURCE_ADDRESS_CGI);
    assert(strcmp(a->u.cgi->path, ra3.u.cgi->path) == 0);
    assert(strcmp(a->u.cgi->path_info, "/") == 0);

    b = resource_address_load_base(pool, &dest, a, "");
    assert(b != NULL);
    assert(b->type == RESOURCE_ADDRESS_CGI);
    assert(strcmp(b->u.cgi->path, ra3.u.cgi->path) == 0);
    assert(strcmp(b->u.cgi->uri, "/foo/") == 0);
    assert(strcmp(b->u.cgi->path_info, "/") == 0);

    b = resource_address_load_base(pool, &dest, a, "xyz");
    assert(b != NULL);
    assert(b->type == RESOURCE_ADDRESS_CGI);
    assert(strcmp(b->u.cgi->path, ra3.u.cgi->path) == 0);
    assert(strcmp(b->u.cgi->uri, "/foo/xyz") == 0);
    assert(strcmp(b->u.cgi->path_info, "/xyz") == 0);

    a = resource_address_save_base(pool, &dest2, &ra3, "baz");
    assert(a != NULL);
    assert(a->type == RESOURCE_ADDRESS_CGI);
    assert(strcmp(a->u.cgi->path, ra3.u.cgi->path) == 0);
    assert(strcmp(a->u.cgi->uri, "/foo/bar/") == 0);
    assert(strcmp(a->u.cgi->path_info, "/bar/") == 0);

    b = resource_address_load_base(pool, &dest, a, "bar/");
    assert(b != NULL);
    assert(b->type == RESOURCE_ADDRESS_CGI);
    assert(strcmp(b->u.cgi->path, ra3.u.cgi->path) == 0);
    assert(strcmp(b->u.cgi->uri, "/foo/bar/bar/") == 0);
    assert(strcmp(b->u.cgi->path_info, "/bar/bar/") == 0);

    b = resource_address_load_base(pool, &dest, a, "bar/xyz");
    assert(b != NULL);
    assert(b->type == RESOURCE_ADDRESS_CGI);
    assert(strcmp(b->u.cgi->path, ra3.u.cgi->path) == 0);
    assert(strcmp(b->u.cgi->uri, "/foo/bar/bar/xyz") == 0);
    assert(strcmp(b->u.cgi->path_info, "/bar/bar/xyz") == 0);

    test_auto_base(pool);
    test_base_no_path_info(pool);
    test_cgi_apply(pool);

    pool_unref(pool);
    pool_commit();

    pool_recycler_clear();
}
