/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef ISTREAM_OO_HXX
#define ISTREAM_OO_HXX

#include "pool.hxx"
#include "Struct.hxx"
#include "istream_invoke.hxx"
#include "istream_new.hxx"
#include "util/Cast.hxx"

class Istream {
    struct istream output;

protected:
    explicit Istream(struct pool &pool)
        :output(pool) {}

    Istream(const Istream &) = delete;
    Istream &operator=(const Istream &) = delete;

    virtual ~Istream() {
        istream_deinit(&output);
    }

    struct pool &GetPool() {
        return *output.pool;
    }

    bool HasHandler() const {
        return istream_has_handler(&output);
    }

    FdTypeMask GetHandlerDirect() const {
        return output.handler_direct;
    }

    bool CheckDirect(FdType type) const {
        return (output.handler_direct & FdTypeMask(type)) != 0;
    }

    size_t InvokeData(const void *data, size_t length) {
        return istream_invoke_data(&output, data, length);
    }

    ssize_t InvokeDirect(FdType type, int fd, size_t max_length) {
        return istream_invoke_direct(&output, type, fd, max_length);
    }

    void InvokeEof() {
        istream_invoke_eof(&output);
    }

    void InvokeError(GError *error) {
        istream_invoke_abort(&output, error);
    }

    void Destroy() {
        this->~Istream();
        /* no need to free memory from the pool */
    }

    void DestroyEof() {
        InvokeEof();
        Destroy();
    }

    void DestroyError(GError *error) {
        InvokeError(error);
        Destroy();
    }

    /**
     * @return the number of bytes still in the buffer
     */
    template<typename Buffer>
    size_t ConsumeFromBuffer(Buffer &buffer) {
        auto r = buffer.Read().ToVoid();
        if (r.IsEmpty())
            return 0;

        size_t consumed = InvokeData(r.data, r.size);
        if (consumed > 0)
            buffer.Consume(consumed);
        return r.size - consumed;
    }

    /**
     * @return the number of bytes consumed
     */
    template<typename Buffer>
    size_t SendFromBuffer(Buffer &buffer) {
        auto r = buffer.Read().ToVoid();
        if (r.IsEmpty())
            return 0;

        size_t consumed = InvokeData(r.data, r.size);
        if (consumed > 0)
            buffer.Consume(consumed);
        return consumed;
    }

public:
    struct istream *Cast() {
        return &output;
    }

    static constexpr Istream &Cast(struct istream &i) {
        return ContainerCast2(i, &Istream::output);
    }

    /* istream */

    /**
     * How much data is available?
     *
     * @param partial if false, the stream must provide the data size
     * until the end of the stream; for partial, a minimum estimate is
     * ok
     * @return the number of bytes available or -1 if the object does
     * not know
     */
    virtual off_t GetAvailable(gcc_unused bool partial) {
        return -1;
    }

    /**
     * Skip data without processing it.  By skipping 0 bytes, you can
     * test whether the stream is able to skip at all.
     *
     * @return the number of bytes skipped or -1 if skipping is not supported
     */
    virtual off_t Skip(gcc_unused off_t length) {
        return -1;
    }

    /**
     * Try to read from the stream.  If the stream can read data
     * without blocking, it must provide data.  It may invoke the
     * callbacks any number of times, supposed that the handler itself
     * doesn't block.
     *
     * If the stream does not provide data immediately (and it is not
     * at EOF yet), it must install an event and invoke the handler
     * later, whenever data becomes available.
     *
     * Whenever the handler reports it is blocking, the responsibility
     * for calling back (and calling this function) is handed back to
     * the istream handler.
     */
    virtual void Read() = 0;

    /**
     * Close the istream object, and return the remaining data as a
     * file descriptor.  This fd can be read until end-of-stream.
     * Returns -1 if this is not possible (the stream object is still
     * usable).
     */
    virtual int AsFd() {
        return -1;
    }

    /**
     * Close the stream and free resources.  This must not be called
     * after the handler's eof() / abort() callbacks were invoked.
     */
    virtual void Close() {
        Destroy();
    }

private:
    static off_t GetAvailable(struct istream *istream, bool partial) {
        return Cast(*istream).GetAvailable(partial);
    }

    static off_t Skip(struct istream *istream, off_t length) {
        return Cast(*istream).Skip(length);
    }

    static void Read(struct istream *istream) {
        Cast(*istream).Read();
    }

    static int AsFd(struct istream *istream) {
        return Cast(*istream).AsFd();
    }

    static void Close(struct istream *istream) {
        Cast(*istream).Close();
    }
};

template<typename T, typename... Args>
static inline struct istream *
NewIstream(struct pool &pool, Args&&... args)
{
    return NewFromPool<T>(pool, pool,
                          std::forward<Args>(args)...)->Cast();
}

#endif
