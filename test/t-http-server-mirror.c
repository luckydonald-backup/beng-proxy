#include "http-server.h"
#include "duplex.h"

#include <stdio.h>
#include <stdlib.h>
#include <event.h>

static void
my_request(struct http_server_request *request, void *ctx)
{
    (void)ctx;

    http_server_response(request, HTTP_STATUS_OK,
                         NULL,
                         request->content_length,
                         request->body);
}

static void
my_free(void *ctx)
{
    (void)ctx;
}

static struct http_server_connection_handler handler = {
    .request = my_request,
    .free = my_free,
};

int main(int argc, char **argv) {
    pool_t pool;
    int sockfd;
    http_server_connection_t connection;

    (void)argc;
    (void)argv;

    event_init();

    pool = pool_new_libc(NULL, "root");

    sockfd = duplex_new(pool, 0, 1);
    if (sockfd < 0) {
        perror("duplex_new() failed");
        exit(2);
    }

    http_server_connection_new(pool, sockfd,
                               "localhost", &handler, NULL,
                               &connection);

    event_dispatch();

    pool_unref(pool);
    pool_commit();
    pool_recycler_clear();
}
