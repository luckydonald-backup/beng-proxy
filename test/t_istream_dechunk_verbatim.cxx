#include "istream/istream_dechunk.hxx"
#include "istream/istream_byte.hxx"
#include "istream/istream_four.hxx"
#include "istream/istream_string.hxx"
#include "istream/istream.hxx"

#include <stdio.h>

#define EXPECTED_RESULT "3\r\nfoo\r\n0\r\n\r\n"

/* add space at the end so we don't run into an assertion failure when
   istream_string reports EOF but istream_dechunk has already cleared
   its handler */
#define INPUT EXPECTED_RESULT " "

static Istream *
create_input(struct pool *pool)
{
    return istream_string_new(pool, INPUT);
}

class MyDechunkHandler final : public DechunkHandler {
    void OnDechunkEndSeen() override {}

    bool OnDechunkEnd() override {
        return false;
    }
};

static Istream *
create_test(struct pool *pool, Istream *input)
{
    auto *handler = NewFromPool<MyDechunkHandler>(*pool);
    input = istream_dechunk_new(pool, *input, *handler);
    istream_dechunk_check_verbatim(*input);
#ifdef T_BYTE
    input = istream_byte_new(*pool, *input);
#endif
#ifdef T_FOUR
    input = istream_four_new(pool, *input);
#endif
    return input;
}

#include "t_istream_filter.hxx"
