/*
 * Filter an istream through a piped program.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "pipe_filter.hxx"
#include "http_response.hxx"
#include "fork.hxx"
#include "format.h"
#include "stopwatch.hxx"
#include "istream_stopwatch.hxx"
#include "strmap.hxx"
#include "pool.hxx"
#include "istream/istream.hxx"
#include "sigutil.h"
#include "ChildOptions.hxx"
#include "exec.hxx"
#include "PrefixLogger.hxx"
#include "util/ConstBuffer.hxx"
#include "util/djbhash.h"
#include "util/Error.hxx"

#include <daemon/log.h>

#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct pipe_ctx {
    const ChildOptions &options;

    sigset_t signals;

    int stderr_pipe;

    Exec exec;
};

static int
pipe_fn(void *ctx)
{
    struct pipe_ctx *c = (struct pipe_ctx *)ctx;

    install_default_signal_handlers();
    leave_signal_section(&c->signals);

    if (c->stderr_pipe >= 0)
        dup2(c->stderr_pipe, STDERR_FILENO);

    c->options.Apply();

    c->exec.DoExec();
}

static void
pipe_child_callback(int status, void *ctx gcc_unused)
{
    int exit_status = WEXITSTATUS(status);

    if (WIFSIGNALED(status)) {
        int level = 1;
        if (!WCOREDUMP(status) && WTERMSIG(status) == SIGTERM)
            level = 4;

        daemon_log(level, "Pipe program died from signal %d%s\n",
                   WTERMSIG(status),
                   WCOREDUMP(status) ? " (core dumped)" : "");
    } else if (exit_status != 0)
        daemon_log(1, "Pipe program exited with status %d\n",
                   exit_status);
}

static const char *
append_etag(struct pool *pool, const char *in, const char *suffix)
{
    size_t length;

    if (*in != '"')
        /* simple concatenation */
        return p_strcat(pool, in, suffix, nullptr);

    length = strlen(in + 1);
    if (in[length] != '"')
        return p_strcat(pool, in, suffix, nullptr);

    return p_strncat(pool, in, length, suffix, strlen(suffix),
                     "\"", (size_t)1, nullptr);
}

static const char *
make_pipe_etag(struct pool *pool, const char *in,
               const char *path,
               ConstBuffer<const char *> args,
               ConstBuffer<const char *> env)
{
    char suffix[10] = {'-'};

    /* build hash from path and arguments */
    unsigned hash = djb_hash_string(path);

    for (auto i : args)
        hash ^= djb_hash_string(i);

    for (auto i : env)
        hash ^= djb_hash_string(i);

    format_uint32_hex_fixed(suffix + 1, hash);
    suffix[9] = 0;

    /* append the hash to the old ETag */
    return append_etag(pool, in, suffix);
}

void
pipe_filter(struct pool *pool, const char *path,
            ConstBuffer<const char *> args,
            ConstBuffer<const char *> env,
            const ChildOptions &options,
            http_status_t status, struct strmap *headers, struct istream *body,
            const struct http_response_handler *handler,
            void *handler_ctx)
{
    struct stopwatch *stopwatch;
    struct istream *response;
    const char *etag;

    if (body == nullptr) {
        /* if the resource does not have a body (which is different
           from Content-Length:0), don't filter it */
        handler->InvokeResponse(handler_ctx, status, headers, nullptr);
        return;
    }

    assert(!http_status_is_empty(status));

    stopwatch = stopwatch_new(pool, path);

    const auto prefix_logger = CreatePrefixLogger(IgnoreError());

    struct pipe_ctx c = {
        .options = options,
        .stderr_pipe = prefix_logger.second,
    };

    c.exec.Append(path);
    for (auto i : args)
        c.exec.Append(i);
    for (auto i : env)
        c.exec.PutEnv(i);

    const int clone_flags = options.ns.GetCloneFlags(SIGCHLD);

    /* avoid race condition due to libevent signal handler in child
       process */
    enter_signal_section(&c.signals);

    GError *error = nullptr;
    pid_t pid = beng_fork(pool, path, body, &response,
                          clone_flags,
                          pipe_fn, &c,
                          pipe_child_callback, nullptr, &error);
    if (prefix_logger.second >= 0)
        close(prefix_logger.second);
    if (pid < 0) {
        leave_signal_section(&c.signals);
        DeletePrefixLogger(prefix_logger.first);
        handler->InvokeAbort(handler_ctx, error);
        return;
    }

    leave_signal_section(&c.signals);

    if (prefix_logger.first != nullptr)
        PrefixLoggerSetPid(*prefix_logger.first, pid);

    stopwatch_event(stopwatch, "fork");

    etag = headers->Get("etag");
    if (etag != nullptr) {
        /* we cannot pass the original ETag to the client, because the
           pipe has modified the resource (which is what the pipe is
           all about) - append a digest value to the ETag, which is
           built from the program path and its arguments */

        etag = make_pipe_etag(pool, etag, path, args, env);
        assert(etag != nullptr);

        headers = strmap_dup(pool, headers);
        headers->Set("etag", etag);
    }

    response = istream_stopwatch_new(pool, response, stopwatch);

    handler->InvokeResponse(handler_ctx, status, headers, response);
}
