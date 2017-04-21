#include "PInstance.hxx"
#include "pool.hxx"
#include "net/Ping.hxx"
#include "net/AllocatedSocketAddress.hxx"
#include "net/Parser.hxx"
#include "util/Cancellable.hxx"
#include "util/PrintException.hxx"

#include <stdio.h>
#include <stdlib.h>

static bool success;
static CancellablePointer my_cancel_ptr;

class MyPingClientHandler final : public PingClientHandler {
public:
    void PingResponse() override {
        success = true;
        printf("ok\n");
    }

    void PingTimeout() override {
        fprintf(stderr, "timeout\n");
    }

    void PingError(std::exception_ptr ep) override {
        PrintException(ep);
    }
};

int main(int argc, char **argv)
try {
    if (argc != 2) {
        fprintf(stderr, "usage: run-ping IP\n");
        return EXIT_FAILURE;
    }

    PInstance instance;
    LinearPool pool(instance.root_pool, "test", 8192);

    const auto address = ParseSocketAddress(argv[1], 0, false);

    MyPingClientHandler handler;
    ping(instance.event_loop, *pool, address,
         handler,
         my_cancel_ptr);

    instance.event_loop.Dispatch();

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (const std::exception &e) {
    PrintException(e);
    return EXIT_FAILURE;
}
