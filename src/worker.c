/*
 * Child process management.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "worker.h"
#include "http-server.h"
#include "pool.h"
#include "instance.h"
#include "connection.h"
#include "session.h"
#include "child.h"
#include "listener.h"
#include "control-handler.h"

#include <daemon/log.h>

#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

static void
schedule_respawn(struct instance *instance);

static void
respawn_event_callback(int fd __attr_unused, short event __attr_unused,
                       void *ctx)
{
    struct instance *instance = (struct instance*)ctx;
    pid_t pid;

    if (instance->should_exit ||
        instance->num_workers >= instance->config.num_workers)
        return;

    daemon_log(2, "respawning child\n");

    pid = worker_new(instance);
    if (pid != 0)
        schedule_respawn(instance);
}

static void
schedule_respawn(struct instance *instance)
{
    if (!instance->should_exit &&
        instance->num_workers < instance->config.num_workers &&
        evtimer_pending(&instance->respawn_event, NULL) == 0) {
        static struct timeval tv = {
            .tv_sec = 1,
            .tv_usec = 0,
        };

        evtimer_set(&instance->respawn_event, respawn_event_callback, instance);
        evtimer_add(&instance->respawn_event, &tv);
    }
}

static void
worker_child_callback(int status, void *ctx)
{
    struct worker *worker = ctx;
    struct instance *instance = worker->instance;
    int exit_status = WEXITSTATUS(status);

    if (WIFSIGNALED(status)) {
        daemon_log(1, "worker %d died from signal %d%s\n",
                   worker->pid, WTERMSIG(status),
                   WCOREDUMP(status) ? " (core dumped)" : "");
    } else if (exit_status == 0)
        daemon_log(1, "worker %d exited with success\n",
                   worker->pid);
    else
        daemon_log(1, "worker %d exited with status %d\n",
                   worker->pid, exit_status);

    crash_deinit(&worker->crash);
    list_remove(&worker->siblings);
    assert(instance->num_workers > 0);
    --instance->num_workers;

    p_free(instance->pool, worker);

    if (WIFSIGNALED(status) && !instance->should_exit &&
        !crash_is_safe(&worker->crash)) {
        /* a worker has died due to a signal - this is dangerous for
           all other processes (including us), because the worker may
           have corrupted shared memory.  Our only hope to recover is
           to immediately free all shared memory, kill all workers
           still using it, and spawn new workers with fresh shared
           memory. */
        bool ret;

        daemon_log(1, "abandoning shared memory, preparing to kill and respawn all workers\n");

        session_manager_abandon();

        ret = session_manager_init(instance->config.cluster_size,
                                   instance->config.cluster_node);
        if (!ret) {
            daemon_log(1, "session_manager_init() failed\n");
            _exit(2);
        }

        worker_killall(instance);
    }

    schedule_respawn(instance);
}

pid_t
worker_new(struct instance *instance)
{
    assert(!crash_in_unsafe());

    pid_t pid;
    bool ret __attr_unused;

    deinit_signals(instance);
    children_event_del();

    int distribute_socket = -1;
    if (instance->config.control_listen != NULL) {
        distribute_socket = global_control_handler_add_fd();
        if (distribute_socket < 0) {
            daemon_log(1, "udp_distribute_add() failed: %s\n",
                       strerror(errno));
            return -1;
        }
    }

    struct crash crash;
    if (!crash_init(&crash))
        return -1;

    pid = fork();
    if (pid < 0) {
        daemon_log(1, "fork() failed: %s\n", strerror(errno));

        if (distribute_socket >= 0)
            close(distribute_socket);

        crash_deinit(&crash);
    } else if (pid == 0) {
        event_reinit(instance->event_base);

        crash_deinit(&global_crash);
        global_crash = crash;

        if (distribute_socket >= 0)
            global_control_handler_set_fd(distribute_socket);

        instance->config.num_workers = 0;

        list_init(&instance->workers);
        instance->num_workers = 0;

        all_listeners_event_del(instance);

        while (!list_empty(&instance->connections))
            close_connection((struct client_connection*)instance->connections.next);

        init_signals(instance);
        children_init(instance->pool);

        session_manager_event_del();

        ret = session_manager_init(instance->config.cluster_size,
                                   instance->config.cluster_node);
        assert(ret);

        all_listeners_event_add(instance);
    } else {
        struct worker *worker;

        if (distribute_socket >= 0)
            close(distribute_socket);

        event_reinit(instance->event_base);

        worker = p_malloc(instance->pool, sizeof(*worker));
        worker->instance = instance;
        worker->pid = pid;
        worker->crash = crash;

        list_add(&worker->siblings, &instance->workers);
        ++instance->num_workers;

        init_signals(instance);
        children_event_add();

        child_register(pid, worker_child_callback, worker);
    }

    return pid;
}

void
worker_killall(struct instance *instance)
{
    struct worker *worker;
    int ret;

    for (worker = (struct worker*)instance->workers.next;
         worker != (struct worker*)&instance->workers;
         worker = (struct worker*)worker->siblings.next) {
        ret = kill(worker->pid, SIGTERM);
        if (ret < 0)
            daemon_log(1, "failed to kill worker: %s\n", strerror(errno));
    }
}
