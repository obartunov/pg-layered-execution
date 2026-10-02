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

class FileReader : public ObjectReader
{
public:
    FileReader(int fd, int64_t size) : fd_(fd), size_(size) {}

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
    int     fd_;
    int64_t size_;
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
        return new FileReader(fd, static_cast<int64_t>(st.st_size));
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
