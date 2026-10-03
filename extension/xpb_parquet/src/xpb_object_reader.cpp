/*
 * xpb_object_reader.cpp -- FileReader: local files, and nothing above them.
 *
 * The only code in this module that knows a pathname, a file descriptor,
 * pread() or errno. Everything above it sees ObjectReader.
 *
 * pread() rather than read()+lseek: no file position means no shared mutable
 * state between calls, which is what lets the reader be used for the footer
 * probe and for column chunks without a seek dance. It also makes the
 * "not thread-safe" caveat in the header a statement about the counters rather
 * than about the file.
 */
#include "xpb_object_reader.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xpb {

namespace {

/*
 * The identity of an open local file.
 *
 * CHOSEN FROM A MEASUREMENT, not from intuition. The matrix in
 * test/local_identity_matrix.cpp runs eight mutation scenarios and records, for
 * each, what moves when observed through the READER'S OWN descriptor:
 *
 *   scenario                       ino size mtime ctime | fd sees new bytes
 *   1 same-size overwrite           no   no   YES   yes | YES   <- must detect
 *   2 different-size rewrite        no  yes   YES   yes | YES   <- must detect
 *   3 append                        no  yes   yes   yes | no
 *   4 truncate + rewrite same size  no   no   YES   yes | YES   <- must detect
 *   5 rename away, new file at path no   no    no   YES | no    <- must NOT flag
 *   6 atomic replace via rename     no   no    no   YES | no    <- must NOT flag
 *   7 unlink while reader open      no   no    no   YES | no    <- must NOT flag
 *   8 overwrite right after open    no   no   YES   yes | YES   <- must detect
 *
 * What that rules out:
 *
 *   st_ctim  changes in ALL EIGHT, including the three where the reader's
 *            bytes are untouched -- rename and unlink both touch it. Using
 *            ctime would condemn an atomic replace, which is the safe idiom.
 *            A "catch more by including ctime" instinct is wrong here, and
 *            only the matrix says so.
 *   st_size  alone misses 1, 4 and 8, all same-size mutations.
 *   st_ino   never moves: fstat follows the descriptor to its inode, so it
 *            cannot be a change detector. It is still carried, because it
 *            states WHICH inode is pinned.
 *   statx change cookie -- designed for exactly this question, and NOT
 *            supported by this kernel and filesystem. Probed, not assumed.
 *
 * So: (st_dev, st_ino, st_size, st_mtim), via fstat on our own fd.
 *
 * THE LIMIT OF THE GUARANTEE, stated rather than implied. mtime is the only
 * detector for a same-size overwrite, so the guarantee is only as fine as the
 * filesystem's timestamp. Measured here: 200 of 200 back-to-back writes moved
 * st_mtim, at nanosecond granularity. On a filesystem with coarse timestamps a
 * same-size overwrite inside one tick would go unnoticed, and this token would
 * be unsound there. It is not a guarantee about all filesystems.
 *
 * Append (3) is detected and refused although the bytes already read are
 * intact. That is deliberate and conservative: the object is no longer the one
 * that was opened, and a reader holding a footer from a file of size N cannot
 * claim it is authoritative for a file of size N + M.
 */
struct FileIdentity
{
    dev_t    dev = 0;
    ino_t    ino = 0;
    int64_t  size = -1;
    int64_t  mtime_sec = 0;
    long     mtime_nsec = 0;

    static FileIdentity of(const struct stat &st)
    {
        FileIdentity id;
        id.dev = st.st_dev;
        id.ino = st.st_ino;
        id.size = static_cast<int64_t>(st.st_size);
        id.mtime_sec = static_cast<int64_t>(st.st_mtim.tv_sec);
        id.mtime_nsec = static_cast<long>(st.st_mtim.tv_nsec);
        return id;
    }

    bool operator==(const FileIdentity &o) const
    {
        return dev == o.dev && ino == o.ino && size == o.size &&
               mtime_sec == o.mtime_sec && mtime_nsec == o.mtime_nsec;
    }

    /* For the error message. Deliberately not exposed above ObjectReader. */
    std::string str() const
    {
        return "dev " + std::to_string((unsigned long long) dev) +
               " ino " + std::to_string((unsigned long long) ino) +
               " size " + std::to_string(size) +
               " mtime " + std::to_string(mtime_sec) + "." +
               std::to_string(mtime_nsec);
    }
};

class FileReader : public ObjectReader
{
public:
    FileReader(int fd, int64_t size, const FileIdentity &id)
        : fd_(fd), size_(size), identity_(id) {}

    ~FileReader() override { close(); }

    int64_t size() override
    {
        if (fd_ < 0)
            return fail("size() on a closed reader");
        return size_;
    }

    ReadResult read_at_most(int64_t offset, int64_t nbytes, void *out) override
    {
        clear_error();

        if (fd_ < 0)
        {
            fail("read on a closed reader");
            return ReadResult::failure();
        }
        if (!range_ok(offset, nbytes))
            return ReadResult::failure();
        if (nbytes == 0)
            return ReadResult::got(0, false);   /* not I/O; not counted */
        if (out == nullptr)
        {
            fail("read with a null buffer");
            return ReadResult::failure();
        }

        /*
         * Between range reads, never inside one. The hook may longjmp out of
         * here on its own thread, so nothing is allocated or half-updated
         * before it runs: the counters are touched only after the read returns.
         */
        /*
         * The identity check sits with the interrupt hook, before the physical
         * read, so a read is never issued against an object that has already
         * moved. Checked on EVERY read rather than every Nth: the overhead is
         * one fstat, and a sampled check would leave a window in which a
         * mixed-incarnation read is served -- which is the whole thing this
         * prevents.
         */
        if (!identity_still_ours())
            return ReadResult::failure();

        before_read();

        char   *dst = static_cast<char *>(out);
        int64_t got = 0;

        /*
         * Loop because pread() may return short for reasons that are not the
         * end of the file -- a signal, or a large request on some filesystems.
         * Only a zero return means the file ended, and that is the ONLY thing
         * this reader will call eof.
         */
        while (got < nbytes)
        {
            ssize_t n = ::pread(fd_, dst + got, static_cast<size_t>(nbytes - got),
                                static_cast<off_t>(offset + got));
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                account(nbytes, got);
                fail(std::string("pread: ") + std::strerror(errno));
                return ReadResult::failure();
            }
            if (n == 0)
            {
                /*
                 * End of file, and for a LOCAL file this is authoritative: the
                 * kernel is not a transport that can lose bytes. A remote
                 * implementation may not reason this way -- see the header.
                 */
                account(nbytes, got);
                return ReadResult::got(got, true);
            }
            got += n;
        }

        account(nbytes, got);
        return ReadResult::got(got, false);
    }

    void close() override
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    /*
     * fstat on OUR descriptor, never stat on a pathname. That distinction is
     * the whole reason an atomic rename replacement is safe here: the
     * descriptor still refers to the inode it was opened on, so its identity
     * does not move, while "what the pathname names now" has changed and is
     * none of this reader's business. A path-based check would report
     * corruption for the one replacement idiom that is correct.
     */
    bool identity_still_ours()
    {
        struct stat st;

        if (::fstat(fd_, &st) != 0)
        {
            fail(std::string("fstat while verifying object identity: ") +
                 std::strerror(errno));
            return false;
        }

        FileIdentity now = FileIdentity::of(st);
        if (now == identity_)
            return true;

        fail_identity_moved("local file was " + identity_.str() +
                            " at open, is now " + now.str());
        return false;
    }

    int             fd_;
    int64_t         size_;
    FileIdentity    identity_;
};

}   /* namespace */

ObjectReader *
open_file_reader(const char *path, std::string *error)
{
    if (path == nullptr)
    {
        if (error) *error = "no path";
        return nullptr;
    }

    int fd = ::open(path, O_RDONLY);
    if (fd < 0)
    {
        if (error) *error = std::string("open ") + path + ": " + std::strerror(errno);
        return nullptr;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0)
    {
        if (error) *error = std::string("fstat ") + path + ": " + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    if (!S_ISREG(st.st_mode))
    {
        /*
         * Refused rather than read: a directory, a fifo or a device has no
         * stable size, and Parquet needs to read its footer from the end.
         */
        if (error) *error = std::string(path) + " is not a regular file";
        ::close(fd);
        return nullptr;
    }

    /*
     * The fd is not owned by anything until the FileReader exists. operator new
     * can throw, and this function is the only place in the module that holds a
     * descriptor with no destructor behind it, so the failure is closed here
     * rather than left to the caller's handler, which cannot see the fd.
     */
    try
    {
        /*
     * Size and identity come from the SAME fstat, so there is no window in
     * which the reader holds a length from one incarnation and pins another.
     */
    return new FileReader(fd, static_cast<int64_t>(st.st_size),
                          FileIdentity::of(st));
    }
    catch (...)
    {
        ::close(fd);
        throw;
    }
}

/* ------------------------------------------------------------------ faults -- */

namespace {

class FaultyReader : public ObjectReader
{
public:
    FaultyReader(ObjectReader *inner, const FaultSpec &spec)
        : inner_(inner), spec_(spec) {}

    ~FaultyReader() override { delete inner_; }

    int64_t size() override { return inner_->size(); }
    void    close() override { inner_->close(); }

    int64_t physical_attempts() const { return attempts_; }
    int64_t injected() const { return injected_; }

    ReadResult read_at_most(int64_t offset, int64_t nbytes, void *out) override
    {
        clear_error();
        if (!range_ok(offset, nbytes))
            return ReadResult::failure();
        if (nbytes == 0)
            return ReadResult::got(0, false);

        const int64_t ordinal = ++logical_reads_;
        const bool    damage  = (spec_.mode != FaultMode::kNone &&
                                 ordinal == spec_.at_read);

        if (!damage)
            return pass_through(offset, nbytes, out);

        injected_++;

        switch (spec_.mode)
        {
            case FaultMode::kShortNoEof:
            {
                /*
                 * The whole point of the phase. Deliver part of the range and
                 * say nothing about the end of the object, which is what a
                 * truncated transfer looks like. read_exact() must turn this
                 * into an error; a caller that inferred eof from the short
                 * count would have accepted a hole in a column chunk.
                 */
                int64_t give = spec_.nbytes > 0 && spec_.nbytes < nbytes
                               ? spec_.nbytes : nbytes / 2;
                if (give <= 0)
                    give = 1;
                ReadResult r = pass_through(offset, give, out);
                if (!r.ok)
                    return r;
                return ReadResult::got(r.bytes, false);
            }

            case FaultMode::kTransientFirst:
            {
                /*
                 * Retry is the implementation's business, so it happens HERE
                 * and the caller never learns of it. The logical request is
                 * unchanged; the physical traffic is not, which is exactly the
                 * property the acceptance criteria name.
                 *
                 * The failing attempt does real work and is then discarded,
                 * because that is what a transient transport failure costs. An
                 * earlier version skipped it, which made the retry free and
                 * therefore invisible in the attempt count -- the test would
                 * have shown nothing.
                 */
                for (int attempt = 0; attempt <= spec_.retries; attempt++)
                {
                    ReadResult r = pass_through(offset, nbytes, out);
                    if (!r.ok)
                        return r;
                    if (attempt == 0)
                    {
                        clear_error();  /* the injected failure: bytes paid for,
                                         * result thrown away, round again */
                        continue;
                    }
                    return r;
                }
                fail("transient failure, retries exhausted");
                return ReadResult::failure();
            }

            case FaultMode::kFailAfterNBytes:
            {
                /* Bytes land in the buffer and THEN it fails, so a caller that
                 * trusted the buffer without checking ok would read a partly
                 * filled range. */
                int64_t give = spec_.nbytes > 0 && spec_.nbytes < nbytes
                               ? spec_.nbytes : nbytes / 2;
                (void) pass_through(offset, give, out);
                fail("I/O error after " + std::to_string(give) + " bytes");
                return ReadResult::failure();
            }

            case FaultMode::kGenuineEof:
            {
                /* Truthful: ask the inner reader beyond the end and relay what
                 * it says, eof included. This is the control -- a real EOF must
                 * still be an EOF, or the fix would just be "never trust
                 * anything". */
                int64_t sz = inner_->size();
                int64_t off = sz > 0 ? sz : 0;
                return pass_through(off, nbytes, out);
            }

            case FaultMode::kChunked:
            {
                /*
                 * Several short pieces, none claiming eof. Covers the shape a
                 * chunked transfer has, and is the one fault that read_exact
                 * is allowed to absorb -- it must assemble the range, because
                 * nothing was lost.
                 */
                char   *dst = static_cast<char *>(out);
                int64_t done = 0;
                int64_t piece = nbytes / 4 > 0 ? nbytes / 4 : 1;
                while (done < nbytes)
                {
                    int64_t want = nbytes - done < piece ? nbytes - done : piece;
                    ReadResult r = pass_through(offset + done, want, dst + done);
                    if (!r.ok)
                        return r;
                    done += r.bytes;
                    if (r.bytes == 0)
                        return ReadResult::got(done, r.eof);
                    if (r.eof && done < nbytes)
                        return ReadResult::got(done, true);
                }
                return ReadResult::got(done, false);
            }

            case FaultMode::kNone:
                break;
        }
        return pass_through(offset, nbytes, out);
    }

    /*
     * kChunked is assembled above, so read_exact's default is correct for every
     * mode and is deliberately NOT overridden: the refusal logic under test is
     * the one in the base class, not a copy of it.
     */

private:
    ReadResult pass_through(int64_t offset, int64_t nbytes, void *out)
    {
        attempts_++;
        ReadResult r = inner_->read_at_most(offset, nbytes, out);
        if (!r.ok)
            fail(inner_->last_error());
        else
            account(nbytes, r.bytes);
        return r;
    }

    ObjectReader   *inner_;
    FaultSpec       spec_;
    int64_t         logical_reads_ = 0;
    int64_t         attempts_ = 0;
    int64_t         injected_ = 0;
};

}   /* namespace */

ObjectReader *
open_faulty_reader(ObjectReader *inner, const FaultSpec &spec, std::string *error)
{
    if (inner == nullptr)
    {
        if (error) *error = "no inner reader";
        return nullptr;
    }
    try
    {
        return new FaultyReader(inner, spec);
    }
    catch (...)
    {
        delete inner;
        throw;
    }
}

int64_t
faulty_physical_attempts(const ObjectReader *r)
{
    const FaultyReader *f = dynamic_cast<const FaultyReader *>(r);
    return f ? f->physical_attempts() : -1;
}

int64_t
faulty_injected(const ObjectReader *r)
{
    const FaultyReader *f = dynamic_cast<const FaultyReader *>(r);
    return f ? f->injected() : -1;
}

}   /* namespace xpb */
