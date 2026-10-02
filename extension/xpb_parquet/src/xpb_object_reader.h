/*
 * xpb_object_reader.h -- a byte-range source, and nothing else.
 *
 * The Parquet reader used to be handed a pathname and call
 * arrow::io::ReadableFile::Open() on it, which put local-file mechanics in the
 * same place as Parquet decoding. This is the boundary between them:
 *
 *     ParquetReader decides WHICH ranges are needed
 *     ObjectReader  only fetches bytes
 *
 * It is deliberately NOT an alias for arrow::io::RandomAccessFile. Arrow's
 * interface carries Tell/Seek/Read-from-position, a Status/Result vocabulary
 * and a stream notion that a byte-range object store does not have; inheriting
 * from it would mean a future S3Reader is written against Arrow rather than
 * against this contract, which is the thing this phase exists to avoid. The
 * adapter in the other direction -- ObjectReader presented AS a
 * RandomAccessFile -- lives in xpb_parquet_shim.cpp, is 40 lines, and is the
 * only Arrow-shaped code in the path.
 *
 * C++ rather than C: the one implementation that exists is local, and the one
 * that is expected (S3) would be built on a C++ SDK. Nothing here is on the C
 * ABI, so a virtual call costs nothing a C function pointer would not.
 *
 * ---------------------------------------------------------------- SEMANTICS
 *
 * Spelled out because "obvious" is how the previous boundary acquired its
 * assumptions.
 *
 *   read_at(offset, nbytes, out)
 *       Caller-provided buffer: the reader never allocates and never hands
 *       back memory, so there is no ownership question and no lifetime tied to
 *       a returned buffer.
 *
 *       EXACT, except at end of object. Returns nbytes when nbytes are
 *       available. Returns fewer ONLY because the object ends there, and that
 *       is not an error -- Parquet's footer probe reads backwards from the end
 *       and a short read is its normal answer. Returns -1 on a real failure,
 *       with last_error() set.
 *
 *       offset < 0, nbytes < 0, or offset + nbytes overflowing int64 are
 *       errors, not clamps. A clamped range would read the wrong bytes and
 *       report success.
 *
 *       nbytes == 0 returns 0 without touching the object and without counting
 *       a read. A zero-length read is not I/O.
 *
 *       Reading entirely past the end returns 0, not an error. EOF is "no more
 *       bytes here", never a failure in itself.
 *
 *   size()
 *       The object's size in bytes, or -1 with last_error() set. Fixed for the
 *       life of the reader: a local file that grows underneath is not a case
 *       this supports, and a Parquet file whose footer moved is corrupt by the
 *       time anyone notices.
 *
 *   close()
 *       Idempotent. Releases the underlying handle. A read after close is an
 *       error, not undefined.
 *
 *   last_error()
 *       The reason the most recent call returned -1. Never NULL; empty when
 *       nothing has failed. The string is owned by the reader and valid until
 *       the next call on it.
 *
 *   threads / reentrancy
 *       NOT thread-safe and not reentrant. One reader belongs to one source in
 *       one backend, and a PostgreSQL backend is single-threaded. A reader that
 *       wanted concurrency would need its own synchronisation and would have to
 *       say so here.
 *
 *   cancellation
 *       The reader does not know about PostgreSQL. An interrupt check is
 *       INJECTED: set_interrupt_hook() installs a callback that is invoked
 *       before each physical read, which is the safe boundary -- between range
 *       reads, never inside a pread() already in the kernel. The hook may
 *       longjmp (that is what ereport does), so the reader holds no C++ object
 *       needing destruction across the call: the hook runs first, then the
 *       read.
 *
 *   accounting
 *       The reader is the authoritative place for it, because it is the only
 *       layer that sees physical reads. bytes_requested and bytes_returned are
 *       counted separately: Arrow's range coalescing asks for more than the
 *       projection needs, and the difference is over-read rather than a
 *       rounding artefact. The phase tag is set from above -- "metadata" versus
 *       "data" is a Parquet notion and the reader does not invent it.
 */
#ifndef XPB_OBJECT_READER_H
#define XPB_OBJECT_READER_H

#include <cstdint>
#include <string>

namespace xpb {

/* Invoked before each physical read. May raise/longjmp. */
typedef void (*InterruptHook)(void);

enum class ReadPhase { kMetadata, kData };

class ObjectReader
{
public:
    virtual ~ObjectReader() = default;

    virtual int64_t size() = 0;
    virtual int64_t read_at(int64_t offset, int64_t nbytes, void *out) = 0;
    virtual void    close() = 0;

    const char *last_error() const { return error_.c_str(); }

    void set_interrupt_hook(InterruptHook hook) { interrupt_ = hook; }
    void set_phase(ReadPhase p) { phase_ = p; }

    int64_t read_calls()      const { return read_calls_; }
    int64_t bytes_requested() const { return bytes_requested_; }
    int64_t bytes_returned()  const { return bytes_returned_; }
    int64_t meta_bytes()      const { return meta_bytes_; }
    int64_t data_bytes()      const { return data_bytes_; }
    int64_t meta_calls()      const { return meta_calls_; }
    int64_t data_calls()      const { return data_calls_; }

protected:
    /* Called by an implementation around each physical read. */
    void before_read()
    {
        if (interrupt_ != nullptr)
            interrupt_();
    }

    void account(int64_t requested, int64_t returned)
    {
        read_calls_++;
        bytes_requested_ += requested;
        bytes_returned_  += returned;
        if (phase_ == ReadPhase::kData) { data_bytes_ += returned; data_calls_++; }
        else                            { meta_bytes_ += returned; meta_calls_++; }
    }

    int64_t fail(const std::string &why)
    {
        error_ = why;
        return -1;
    }

    void clear_error() { error_.clear(); }

    /*
     * Shared argument checking, so every implementation refuses the same
     * things in the same way rather than each inventing its own clamping.
     * Returns false and sets the error when the range is not addressable.
     */
    bool range_ok(int64_t offset, int64_t nbytes)
    {
        if (offset < 0 || nbytes < 0)
        {
            fail("negative offset or length");
            return false;
        }
        if (offset > INT64_MAX - nbytes)
        {
            fail("offset + length overflows int64");
            return false;
        }
        return true;
    }

private:
    std::string     error_;
    InterruptHook   interrupt_ = nullptr;
    ReadPhase       phase_ = ReadPhase::kMetadata;
    int64_t         read_calls_ = 0;
    int64_t         bytes_requested_ = 0;
    int64_t         bytes_returned_ = 0;
    int64_t         meta_bytes_ = 0;
    int64_t         data_bytes_ = 0;
    int64_t         meta_calls_ = 0;
    int64_t         data_calls_ = 0;
};

/*
 * The local-file implementation, and the only place in this module that knows
 * about a pathname, a file descriptor, pread() or errno.
 */
ObjectReader *open_file_reader(const char *path, std::string *error);

}   /* namespace xpb */

#endif  /* XPB_OBJECT_READER_H */
