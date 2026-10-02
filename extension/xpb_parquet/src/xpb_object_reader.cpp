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

    int64_t read_at(int64_t offset, int64_t nbytes, void *out) override
    {
        clear_error();

        if (fd_ < 0)
            return fail("read on a closed reader");
        if (!range_ok(offset, nbytes))
            return -1;
        if (nbytes == 0)
            return 0;               /* not I/O; deliberately not counted */
        if (out == nullptr)
            return fail("read_at with a null buffer");

        /*
         * Between range reads, never inside one. The hook may longjmp out of
         * here, so nothing is allocated or half-updated before it runs: the
         * counters are touched only after the read returns.
         */
        before_read();

        char   *dst = static_cast<char *>(out);
        int64_t got = 0;

        /*
         * Loop because pread() may return short for reasons that are not EOF
         * -- a signal, or a large request on some filesystems. The contract
         * promises exactness except at end of object, so a short return is
         * retried until it is zero, which is the only thing that means EOF.
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
                return fail(std::string("pread: ") + std::strerror(errno));
            }
            if (n == 0)
                break;              /* end of object */
            got += n;
        }

        account(nbytes, got);
        return got;
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

}   /* namespace xpb */
