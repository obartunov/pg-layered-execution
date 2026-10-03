/*
 * What actually changes when a local file is mutated underneath an open reader.
 *
 *   local_identity_matrix [workdir]
 *
 * Phase 1 of ObjectReader v2 exists because the identity token for FileReader
 * must be CHOSEN FROM MEASUREMENT, not from intuition. The question is narrow:
 * which observable attributes of an already-open descriptor move when the
 * bytes under it change, and which do not move when the pathname merely starts
 * naming something else?
 *
 * Everything is observed through fstat() on the READER'S OWN DESCRIPTOR, not
 * stat() on the pathname, because those are different questions and conflating
 * them is how a guard ends up calling an atomic rename "corruption".
 *
 * Also probed: statx()'s change cookie, which exists precisely to answer "has
 * this file changed" and would be a better token than a timestamp if the
 * kernel and filesystem supply it. Probed by raw constant rather than assumed
 * absent, since this kernel's headers do not define it.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/* Not in this kernel's linux/stat.h; the syscall may still honour it. */
#ifndef XPB_STATX_CHANGE_COOKIE
#define XPB_STATX_CHANGE_COOKIE 0x40000000U
#endif

namespace {

struct Snap
{
    bool        ok = false;
    dev_t       dev = 0;
    ino_t       ino = 0;
    off_t       size = 0;
    timespec    mtim {};
    timespec    ctim {};
    bool        cookie_ok = false;
    uint64_t    cookie = 0;
};

/*
 * statx probe, mask only.
 *
 * An earlier version declared its own struct statx to reach the change-cookie
 * field. That was guesswork about a kernel ABI and the kernel wrote past the
 * end of it -- the program died with "stack smashing detected". The question
 * being asked does not need the field: it needs to know whether the kernel
 * SETS the mask bit. So the buffer is oversized and opaque, and only the
 * leading stx_mask is read. If a kernel ever reports support, locating the
 * field becomes a separate, deliberate step.
 */
bool statx_change_cookie_supported(int fd)
{
    alignas(8) unsigned char buf[1024];

    memset(buf, 0, sizeof(buf));
    long rc = ::syscall(SYS_statx, fd, "", AT_EMPTY_PATH,
                        XPB_STATX_CHANGE_COOKIE, buf);
    if (rc != 0)
        return false;
    uint32_t mask;
    memcpy(&mask, buf, sizeof(mask));
    return (mask & XPB_STATX_CHANGE_COOKIE) != 0;
}

Snap snap(int fd)
{
    Snap s;
    struct stat st;

    if (::fstat(fd, &st) != 0)
        return s;
    s.ok = true;
    s.dev = st.st_dev;
    s.ino = st.st_ino;
    s.size = st.st_size;
    s.mtim = st.st_mtim;
    s.ctim = st.st_ctim;

    /* Only whether it is supported; the value is deliberately not read. */
    s.cookie_ok = statx_change_cookie_supported(fd);
    s.cookie = 0;
    return s;
}

bool same_ts(const timespec &a, const timespec &b)
{ return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec; }

std::string first4(int fd)
{
    char b[5] = {0};
    if (::pread(fd, b, 4, 0) != 4)
        return "(short)";
    return std::string(b, 4);
}

std::string WORK = "/tmp/xpb_ident";

std::string path_of(const char *name) { return WORK + "/" + name; }

/* A fresh file of `n` bytes filled with `fill`, first four bytes `tag`. */
void make_file(const std::string &p, size_t n, char fill, const char *tag)
{
    std::vector<char> buf(n, fill);
    memcpy(buf.data(), tag, 4);
    int fd = ::open(p.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    ::write(fd, buf.data(), buf.size());
    ::close(fd);
}

/* Which inode does the PATHNAME name right now? */
ino_t path_ino(const std::string &p)
{
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 ? st.st_ino : 0;
}

struct Row
{
    const char *name;
    bool ino_changed, size_changed, mtime_changed, ctime_changed, cookie_changed;
    bool fd_sees_new_bytes;
    bool path_same_object;
    bool cookie_supported;
    std::string note;
};

std::vector<Row> rows;

void record(const char *name, const Snap &a, const Snap &b,
            const std::string &before_bytes, const std::string &after_bytes,
            ino_t fd_ino, ino_t p_ino, const std::string &note = "")
{
    Row r;
    r.name = name;
    r.ino_changed = (a.ino != b.ino);
    r.size_changed = (a.size != b.size);
    r.mtime_changed = !same_ts(a.mtim, b.mtim);
    r.ctime_changed = !same_ts(a.ctim, b.ctim);
    r.cookie_supported = a.cookie_ok && b.cookie_ok;
    r.cookie_changed = r.cookie_supported && (a.cookie != b.cookie);
    r.fd_sees_new_bytes = (before_bytes != after_bytes);
    r.path_same_object = (p_ino != 0 && p_ino == fd_ino);
    r.note = note;
    rows.push_back(r);
}

/* ---- the scenarios ---- */

void scenario_same_size_overwrite()
{
    std::string p = path_of("s1");
    make_file(p, 65536, 'a', "AAAA");
    int fd = ::open(p.c_str(), O_RDONLY);
    Snap a = snap(fd);
    std::string b1 = first4(fd);

    /* Same length, different content, written through a SECOND descriptor.
     * Nothing about the file's size or identity changes -- only its bytes. */
    std::vector<char> buf(65536, 'b');
    memcpy(buf.data(), "BBBB", 4);
    int wfd = ::open(p.c_str(), O_WRONLY);
    ::pwrite(wfd, buf.data(), buf.size(), 0);
    ::fsync(wfd);
    ::close(wfd);

    Snap b = snap(fd);
    std::string b2 = first4(fd);
    record("1 same-size overwrite in place", a, b, b1, b2, a.ino, path_ino(p));
    ::close(fd);
}

void scenario_different_size()
{
    std::string p = path_of("s2");
    make_file(p, 65536, 'a', "AAAA");
    int fd = ::open(p.c_str(), O_RDONLY);
    Snap a = snap(fd);
    std::string b1 = first4(fd);

    std::vector<char> buf(131072, 'b');
    memcpy(buf.data(), "BBBB", 4);
    int wfd = ::open(p.c_str(), O_WRONLY);
    ::pwrite(wfd, buf.data(), buf.size(), 0);
    ::fsync(wfd);
    ::close(wfd);

    Snap b = snap(fd);
    std::string b2 = first4(fd);
    record("2 different-size rewrite", a, b, b1, b2, a.ino, path_ino(p));
    ::close(fd);
}

void scenario_append()
{
    std::string p = path_of("s3");
    make_file(p, 65536, 'a', "AAAA");
    int fd = ::open(p.c_str(), O_RDONLY);
    Snap a = snap(fd);
    std::string b1 = first4(fd);

    std::vector<char> buf(4096, 'c');
    int wfd = ::open(p.c_str(), O_WRONLY | O_APPEND);
    ::write(wfd, buf.data(), buf.size());
    ::fsync(wfd);
    ::close(wfd);

    Snap b = snap(fd);
    std::string b2 = first4(fd);
    record("3 append", a, b, b1, b2, a.ino, path_ino(p),
           "leading bytes untouched; only the tail grew");
    ::close(fd);
}

void scenario_truncate_rewrite()
{
    std::string p = path_of("s4");
    make_file(p, 65536, 'a', "AAAA");
    int fd = ::open(p.c_str(), O_RDONLY);
    Snap a = snap(fd);
    std::string b1 = first4(fd);

    /* Truncate to nothing and write the same LENGTH back: the end state looks
     * identical in size, which is what makes size alone a poor token. */
    std::vector<char> buf(65536, 'b');
    memcpy(buf.data(), "BBBB", 4);
    int wfd = ::open(p.c_str(), O_WRONLY | O_TRUNC);
    ::write(wfd, buf.data(), buf.size());
    ::fsync(wfd);
    ::close(wfd);

    Snap b = snap(fd);
    std::string b2 = first4(fd);
    record("4 truncate + rewrite same size", a, b, b1, b2, a.ino, path_ino(p));
    ::close(fd);
}

void scenario_rename_away_then_new()
{
    std::string p = path_of("s5");
    make_file(p, 65536, 'a', "AAAA");
    int fd = ::open(p.c_str(), O_RDONLY);
    Snap a = snap(fd);
    std::string b1 = first4(fd);

    ::rename(p.c_str(), path_of("s5.moved").c_str());
    make_file(p, 65536, 'b', "BBBB");

    Snap b = snap(fd);
    std::string b2 = first4(fd);
    record("5 rename away, new file at path", a, b, b1, b2, a.ino, path_ino(p),
           "the fd still holds the ORIGINAL inode; the path names a new one");
    ::close(fd);
}

void scenario_atomic_rename_replace()
{
    std::string p = path_of("s6");
    make_file(p, 65536, 'a', "AAAA");
    int fd = ::open(p.c_str(), O_RDONLY);
    Snap a = snap(fd);
    std::string b1 = first4(fd);

    std::string tmp = path_of("s6.tmp");
    make_file(tmp, 131072, 'b', "BBBB");
    ::rename(tmp.c_str(), p.c_str());          /* the safe idiom */

    Snap b = snap(fd);
    std::string b2 = first4(fd);
    record("6 atomic replace via rename", a, b, b1, b2, a.ino, path_ino(p),
           "the reader's bytes are UNCHANGED; this must not be flagged");
    ::close(fd);
}

void scenario_unlink_while_open()
{
    std::string p = path_of("s7");
    make_file(p, 65536, 'a', "AAAA");
    int fd = ::open(p.c_str(), O_RDONLY);
    Snap a = snap(fd);
    std::string b1 = first4(fd);

    ::unlink(p.c_str());

    Snap b = snap(fd);
    std::string b2 = first4(fd);
    record("7 unlink while reader open", a, b, b1, b2, a.ino, path_ino(p),
           "inode survives for the open fd; the path names nothing");
    ::close(fd);
}

void scenario_immediate_overwrite()
{
    std::string p = path_of("s8");
    make_file(p, 65536, 'a', "AAAA");
    int fd = ::open(p.c_str(), O_RDONLY);
    Snap a = snap(fd);
    std::string b1 = first4(fd);

    /* The worst case for any timestamp-based token: overwrite as fast as
     * possible after capturing, with no fsync and no sleep, so the write may
     * land inside the same timestamp granularity as the open. If mtime is
     * unchanged here while the bytes differ, a timestamp token is unsound. */
    std::vector<char> buf(65536, 'b');
    memcpy(buf.data(), "BBBB", 4);
    int wfd = ::open(p.c_str(), O_WRONLY);
    ::pwrite(wfd, buf.data(), buf.size(), 0);
    ::close(wfd);

    Snap b = snap(fd);
    std::string b2 = first4(fd);
    record("8 overwrite immediately after open", a, b, b1, b2, a.ino, path_ino(p),
           "no sleep, no fsync: the timestamp-granularity worst case");
    ::close(fd);
}

/* How fine is mtime here, and can two writes share a timestamp? */
void probe_timestamp_resolution()
{
    std::string p = path_of("res");
    make_file(p, 4096, 'a', "AAAA");
    int fd = ::open(p.c_str(), O_RDONLY);

    int same = 0;
    const int tries = 200;
    char one = 'x';
    for (int i = 0; i < tries; i++)
    {
        Snap a = snap(fd);
        int wfd = ::open(p.c_str(), O_WRONLY);
        ::pwrite(wfd, &one, 1, i % 4096);
        ::close(wfd);
        Snap b = snap(fd);
        if (same_ts(a.mtim, b.mtim))
            same++;
    }
    printf("\ntimestamp resolution probe: %d of %d back-to-back writes left "
           "st_mtim UNCHANGED\n", same, tries);
    if (same > 0)
        printf("  -> a timestamp alone CANNOT be the token: a write can land "
               "inside one tick.\n");
    else
        printf("  -> every write moved st_mtim at this resolution.\n");

    Snap s = snap(fd);
    printf("  st_mtim granularity seen: %ld.%09ld\n",
           (long) s.mtim.tv_sec, (long) s.mtim.tv_nsec);
    printf("  statx change cookie: %s\n",
           s.cookie_ok ? "supported" : "NOT supported by this kernel/filesystem");
    ::close(fd);
}

}   /* namespace */

int main(int argc, char **argv)
{
    if (argc > 1)
        WORK = argv[1];
    std::string mk = "mkdir -p " + WORK;
    if (system(mk.c_str()) != 0)
    { fprintf(stderr, "cannot create %s\n", WORK.c_str()); return 2; }

    scenario_same_size_overwrite();
    scenario_different_size();
    scenario_append();
    scenario_truncate_rewrite();
    scenario_rename_away_then_new();
    scenario_atomic_rename_replace();
    scenario_unlink_while_open();
    scenario_immediate_overwrite();

    printf("observed through fstat() on the READER'S OWN fd, not stat() on the path\n\n");
    printf("%-38s %4s %5s %6s %6s %7s %9s %9s\n", "scenario", "ino", "size",
           "mtime", "ctime", "cookie", "fd sees", "path same");
    printf("%-38s %4s %5s %6s %6s %7s %9s %9s\n", "", "chg", "chg", "chg",
           "chg", "chg", "new bytes", "object");
    for (const Row &r : rows)
        printf("%-38s %4s %5s %6s %6s %7s %9s %9s\n", r.name,
               r.ino_changed ? "yes" : "no",
               r.size_changed ? "yes" : "no",
               r.mtime_changed ? "yes" : "no",
               r.ctime_changed ? "yes" : "no",
               r.cookie_supported ? (r.cookie_changed ? "yes" : "no") : "n/a",
               r.fd_sees_new_bytes ? "YES" : "no",
               r.path_same_object ? "yes" : "no");

    printf("\nnotes:\n");
    for (const Row &r : rows)
        if (!r.note.empty())
            printf("  %-36s %s\n", r.name, r.note.c_str());

    probe_timestamp_resolution();

    /* The decision this matrix has to support. */
    printf("\nwhat must be DETECTED (the fd's bytes changed):\n");
    for (const Row &r : rows)
        if (r.fd_sees_new_bytes)
            printf("  %s\n", r.name);
    printf("what must NOT be flagged (the fd's bytes did not change):\n");
    for (const Row &r : rows)
        if (!r.fd_sees_new_bytes)
            printf("  %s\n", r.name);
    return 0;
}
