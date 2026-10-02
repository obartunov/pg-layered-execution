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
 *   threads
 *       read_at() IS CALLED FROM MORE THAN ONE THREAD, and the first version of
 *       this header was wrong to say otherwise on the grounds that "a
 *       PostgreSQL backend is single-threaded". The backend is; Arrow is not.
 *       With pre_buffer enabled, parquet's ReadRangeCache dispatches
 *       column-chunk reads to arrow::internal::ThreadPool, so the footer reads
 *       arrive on the backend thread and the data reads arrive on Arrow worker
 *       threads. Measured, with pid/tid logged at the pread: footer on the
 *       backend tid, every column chunk on a worker tid.
 *
 *       So: the counters are atomic, and an implementation must not assume the
 *       calling thread is the backend's. A reader is still owned by one source
 *       and is not reentrant on one thread; what is not true is that only one
 *       thread ever touches it.
 *
 *   cancellation
 *       The reader does not know about PostgreSQL. An interrupt check is
 *       INJECTED: set_interrupt_hook() installs a callback invoked before each
 *       physical read.
 *
 *       THE HOOK FIRES ONLY ON THE THREAD THAT INSTALLED IT. This is not a
 *       refinement, it is the correctness requirement. PostgreSQL's interrupt
 *       machinery is not thread-safe: ProcessInterrupts() raises, ereport
 *       siglongjmps into PG_exception_stack -- which belongs to the backend
 *       thread -- and the FATAL path runs proc_exit() and its exit handlers.
 *       Called from an Arrow worker that is exactly what happened: the worker
 *       ran proc_exit -> __run_exit_handlers -> ThreadPool::Shutdown() on the
 *       pool it belonged to, while the backend thread sat in
 *       ReadRangeCache::Read() waiting for that read. The backend became
 *       unkillable and blocked cluster shutdown. Longjmping into another
 *       thread's stack is undefined behaviour besides.
 *
 *       The consequence is a real limitation, stated rather than hidden: a
 *       cancel arriving while a pool-dispatched read is in flight is not seen
 *       by this layer. It is seen by the backend thread's own reads and by the
 *       per-row-group CHECK_FOR_INTERRUPTS() in the provider. Finer
 *       granularity on the data path would mean not letting Arrow own the
 *       reads, which is a separate decision with its own cost.
 *
 *       The hook may longjmp on its own thread (that is what ereport does), so
 *       the reader holds no C++ object needing destruction across the call: the
 *       hook runs first, then the read.
 *
 *   accounting
 *       The reader is the authoritative place for it, because it is the only
 *       layer that sees physical reads. bytes_requested and bytes_returned are
 *       counted separately: Arrow's range coalescing asks for more than the
 *       projection needs, and the difference is over-read rather than a
 *       rounding artefact. The phase tag is set from above -- "metadata" versus
 *       "data" is a Parquet notion and the reader does not invent it.
 *
 *       The counters are atomic because of the thread note above. Relaxed
 *       ordering: they are an instrument, and no decision is taken on them
 *       mid-scan.
 */
#ifndef XPB_OBJECT_READER_H
#define XPB_OBJECT_READER_H

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

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

    /*
     * Records the installing thread as well as the hook. Only that thread may
     * run it; see the cancellation note above for why this is a correctness
     * requirement and not a tidiness one.
     */
    void set_interrupt_hook(InterruptHook hook)
    {
        interrupt_ = hook;
        interrupt_owner_ = std::this_thread::get_id();
    }

    void set_phase(ReadPhase p) { phase_ = p; }

    /*
     * How many times the hook was NOT run because the read was on another
     * thread. Exposed so a test can assert the off-thread case actually occurs
     * -- otherwise "no interrupt check ran off-thread" would also be satisfied
     * by a build where Arrow never used its pool, and the guard would be
     * untested.
     */
    int64_t interrupt_skipped_offthread() const { return skipped_offthread_; }

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
        if (interrupt_ == nullptr)
            return;
        if (std::this_thread::get_id() != interrupt_owner_)
        {
            skipped_offthread_++;
            return;
        }
        interrupt_();
    }

    void account(int64_t requested, int64_t returned)
    {
        read_calls_.fetch_add(1, std::memory_order_relaxed);
        bytes_requested_.fetch_add(requested, std::memory_order_relaxed);
        bytes_returned_.fetch_add(returned, std::memory_order_relaxed);
        if (phase_ == ReadPhase::kData)
        {
            data_bytes_.fetch_add(returned, std::memory_order_relaxed);
            data_calls_.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            meta_bytes_.fetch_add(returned, std::memory_order_relaxed);
            meta_calls_.fetch_add(1, std::memory_order_relaxed);
        }
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
    std::thread::id interrupt_owner_{};
    ReadPhase       phase_ = ReadPhase::kMetadata;

    /* Atomic: written from Arrow worker threads as well as the backend's. */
    std::atomic<int64_t> read_calls_{0};
    std::atomic<int64_t> bytes_requested_{0};
    std::atomic<int64_t> bytes_returned_{0};
    std::atomic<int64_t> meta_bytes_{0};
    std::atomic<int64_t> data_bytes_{0};
    std::atomic<int64_t> meta_calls_{0};
    std::atomic<int64_t> data_calls_{0};
    std::atomic<int64_t> skipped_offthread_{0};
};

/*
 * The local-file implementation, and the only place in this module that knows
 * about a pathname, a file descriptor, pread() or errno.
 */
ObjectReader *open_file_reader(const char *path, std::string *error);

}   /* namespace xpb */

#endif  /* XPB_OBJECT_READER_H */
