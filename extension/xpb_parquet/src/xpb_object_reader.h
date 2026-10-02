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
 *   TWO OPERATIONS, because one could not tell EOF from a truncated transfer
 *
 *       v0 had a single read_at() that was "exact except at end of object", so
 *       a caller inferred EOF from a short return. That inference is correct
 *       for a local file -- pread() returning 0 means the file ended -- and
 *       WRONG for anything over a transport. A ranged GET can return fewer
 *       bytes than asked while the object continues, so the v0 contract would
 *       have reported a dropped connection as a clean end of object. For a
 *       column store that is not a crash but a silently short column: the
 *       worst failure shape there is.
 *
 *       EOF is therefore never inferred. It is either stated by the
 *       implementation or it is not EOF.
 *
 *   read_exact(offset, nbytes, out) -> bool
 *       Exactly nbytes, or failure. Never short, never partially satisfied,
 *       and no EOF concept at all: a request that runs off the end of the
 *       object is an ERROR, because the caller asked for bytes that do not
 *       exist. Returns false with last_error() set.
 *
 *       This is the operation Parquet needs and the only one the Arrow adapter
 *       uses for ranges. Arrow asks for ranges it computed from the footer, so
 *       they are known to be inside the object; measured on the benchmark file,
 *       every range Arrow requested was inside it and no read was ever short.
 *       Giving that caller "exact or error" means a transport failure can no
 *       longer arrive as a shortened column chunk.
 *
 *       Retry is the IMPLEMENTATION's business, underneath this call. A reader
 *       that can retry does so before returning false. The adapter has no
 *       retry policy and must not grow one.
 *
 *   read_at_most(offset, nbytes, out) -> ReadResult{bytes, eof, ok}
 *       Up to nbytes, for the one caller that legitimately does not know how
 *       much is there. `eof` is AUTHORITATIVE: an implementation sets it only
 *       when it knows the object ended at that point, and must never set it
 *       merely because it received less than it asked for. A short result with
 *       eof == false is a failure of the transport, not an end of object, and
 *       read_exact built on top of it reports an error rather than success.
 *
 *   both
 *       Caller-provided buffer: the reader never allocates and never hands
 *       back memory, so there is no ownership question and no lifetime tied to
 *       a returned buffer.
 *
 *       offset < 0, nbytes < 0, or offset + nbytes overflowing int64 are
 *       errors, not clamps. A clamped range would read the wrong bytes and
 *       report success.
 *
 *       nbytes == 0 succeeds without touching the object and without counting
 *       a read. A zero-length read is not I/O.
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

/*
 * The result of a read that is allowed to be short.
 *
 * Three fields rather than a signed count, because "how many bytes" and "did
 * the object end" are independent facts and the v0 contract's single int64
 * forced the caller to guess the second from the first.
 */
struct ReadResult
{
    int64_t bytes = 0;      /* placed in the caller's buffer          */
    bool    eof   = false;  /* the object ends here. Stated, never inferred */
    bool    ok    = false;  /* false: see last_error()                 */

    static ReadResult failure()              { return ReadResult{0, false, false}; }
    static ReadResult got(int64_t n, bool e) { return ReadResult{n, e, true}; }
};

class ObjectReader
{
public:
    virtual ~ObjectReader() = default;

    virtual int64_t size() = 0;
    virtual void    close() = 0;

    /*
     * Up to nbytes. eof is set only when the implementation KNOWS the object
     * ended there; see the semantics note above.
     */
    virtual ReadResult read_at_most(int64_t offset, int64_t nbytes, void *out) = 0;

    /*
     * Exactly nbytes or failure. Default implementation is the only one that
     * should ever be needed: ask for the bytes, and treat anything less as an
     * error whether or not the implementation called it EOF. An implementation
     * overrides this only if its transport has a cheaper exact-read primitive.
     */
    virtual bool read_exact(int64_t offset, int64_t nbytes, void *out)
    {
        ReadResult r = read_at_most(offset, nbytes, out);
        if (!r.ok)
            return false;
        if (r.bytes == nbytes)
            return true;
        /*
         * Short. Both reasons are errors here, and they are reported
         * differently because they are different faults: one means the caller
         * asked beyond the object, the other means the transport lost data and
         * the implementation did not recover it.
         */
        if (r.eof)
            fail("read past end of object: wanted " + std::to_string(nbytes) +
                 " at " + std::to_string(offset) + ", object ends after " +
                 std::to_string(r.bytes));
        else
            fail("short read with no end of object: wanted " + std::to_string(nbytes) +
                 " at " + std::to_string(offset) + ", got " + std::to_string(r.bytes) +
                 " -- truncated transfer, not EOF");
        short_reads_without_eof_ += (r.eof ? 0 : 1);
        return false;
    }

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

    /*
     * Reads that came back short while the implementation did NOT claim end of
     * object. Exactly the case v0 would have accepted as a clean EOF. Counted
     * so a test can show the fault was injected and refused, rather than
     * asserting the absence of something that never happened.
     */
    int64_t short_reads_without_eof() const { return short_reads_without_eof_; }

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
    std::atomic<int64_t> short_reads_without_eof_{0};
};

/*
 * The local-file implementation, and the only place in this module that knows
 * about a pathname, a file descriptor, pread() or errno.
 */
ObjectReader *open_file_reader(const char *path, std::string *error);

/* ------------------------------------------------------------------ faults --
 *
 * A reader that fails on purpose, so failure semantics are tested without a
 * network. It exists because the question "what does a read mean when storage
 * is unreliable" has to be answered before a remote reader is written, not
 * during -- otherwise transport, semantics and accounting all get invented at
 * once and the first two leak into the Parquet shim.
 *
 * It wraps a real reader, so the bytes are real and the answer is checkable
 * against the PostgreSQL oracle. Only the delivery is damaged.
 *
 * Deterministic by construction: the trigger is a read ordinal, not a clock or
 * a random number, so a failing case is a failing case on every run.
 */
enum class FaultMode
{
    kNone = 0,
    kShortNoEof,      /* return fewer bytes and do NOT claim eof -- the case
                       * v0's contract would have accepted as a clean end */
    kTransientFirst,  /* fail the first attempt of a read, succeed on retry */
    kFailAfterNBytes, /* deliver n bytes of the range, then fail             */
    kGenuineEof,      /* report the real end of the object, truthfully       */
    kChunked          /* satisfy a read in several short pieces, eof unset,
                       * so read_exact must still come back whole or error   */
};

struct FaultSpec
{
    FaultMode mode = FaultMode::kNone;
    int64_t   at_read = 1;     /* 1-based ordinal of the read to damage      */
    int64_t   nbytes = 0;      /* for kFailAfterNBytes / kShortNoEof         */
    int       retries = 0;     /* attempts the reader makes before giving up */
};

/*
 * Wraps `inner` and takes ownership of it. `physical_attempts()` counts calls
 * made to the inner reader, which is how "retry does not change the logical
 * request but may change the physical traffic" is demonstrated rather than
 * asserted.
 */
ObjectReader *open_faulty_reader(ObjectReader *inner, const FaultSpec &spec,
                                 std::string *error);

/* Physical attempts against the inner reader, for a reader from
 * open_faulty_reader(); -1 for anything else. */
int64_t faulty_physical_attempts(const ObjectReader *r);
/* Faults actually injected, so a test can prove the fault fired. */
int64_t faulty_injected(const ObjectReader *r);

}   /* namespace xpb */

#endif  /* XPB_OBJECT_READER_H */
