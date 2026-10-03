/*
 * Does FileReader hold one incarnation of a local file, and only complain when
 * it should?
 *
 *   local_identity_guard [workdir]
 *
 * Every case is deterministic -- open, read, mutate, read again -- with no
 * race, because the question is about the guard and not about timing.
 *
 * The controls matter as much as the detections. A guard that refused
 * everything would pass the first four cases and be useless: an atomic rename
 * replacement is the CORRECT way to publish a new file, and a reader already
 * holding a descriptor on the old inode can finish safely. Flagging that would
 * turn a safe idiom into a failure, so cases C, D and E are the ones that keep
 * the guard honest.
 */
#include "xpb_object_reader.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int pass = 0, fail = 0;
void ok(const std::string &w)  { printf("  ok      %s\n", w.c_str()); pass++; }
void bad(const std::string &w, const std::string &d = "")
{ printf("  FAIL    %s%s%s\n", w.c_str(), d.empty() ? "" : " -- ", d.c_str()); fail++; }

std::string WORK = "/tmp/xpb_guard";
std::string P(const char *n) { return WORK + "/" + n; }

void write_file(const std::string &p, size_t n, char fill, const char *tag, int flags = 0)
{
    std::vector<char> buf(n, fill);
    memcpy(buf.data(), tag, 4);
    int fd = ::open(p.c_str(), O_CREAT | O_WRONLY | flags, 0644);
    if (fd < 0) { perror("open for write"); return; }
    if (::write(fd, buf.data(), buf.size()) < 0) perror("write");
    ::close(fd);
}

/* Overwrite in place through a SECOND descriptor: the inode keeps its
 * identity as far as the path is concerned, only the bytes move. */
void overwrite_in_place(const std::string &p, size_t n, char fill, const char *tag)
{
    std::vector<char> buf(n, fill);
    memcpy(buf.data(), tag, 4);
    int fd = ::open(p.c_str(), O_WRONLY);
    if (fd < 0) { perror("open for overwrite"); return; }
    if (::pwrite(fd, buf.data(), buf.size(), 0) < 0) perror("pwrite");
    ::close(fd);
}

std::string read4(xpb::ObjectReader *r, bool *okout)
{
    char b[5] = {0};
    *okout = r->read_exact(0, 4, b);
    return std::string(b, 4);
}

bool is_identity_error(xpb::ObjectReader *r)
{
    return std::string(r->last_error()).find("the object changed since this read began")
           != std::string::npos;
}

/* ---- cases that MUST be detected ---- */

void case_same_size_overwrite()
{
    std::string p = P("a");
    write_file(p, 65536, 'a', "AAAA", O_TRUNC);
    std::string err;
    xpb::ObjectReader *r = xpb::open_file_reader(p.c_str(), &err);
    if (!r) { bad("A open", err); return; }

    bool got = false;
    std::string first = read4(r, &got);
    if (!got || first != "AAAA") { bad("A first read", first); delete r; return; }

    overwrite_in_place(p, 65536, 'b', "BBBB");

    std::string second = read4(r, &got);
    if (!got && is_identity_error(r))
        ok("A same-size in-place overwrite is refused, not served");
    else if (got && second == "BBBB")
        bad("A the reader served the NEW bytes -- a mixed-incarnation read", second);
    else if (got)
        bad("A the read succeeded unexpectedly", second);
    else
        bad("A refused for another reason", r->last_error());
    delete r;
}

void case_different_size()
{
    std::string p = P("b");
    write_file(p, 65536, 'a', "AAAA", O_TRUNC);
    std::string err;
    xpb::ObjectReader *r = xpb::open_file_reader(p.c_str(), &err);
    if (!r) { bad("B open", err); return; }
    bool got = false;
    (void) read4(r, &got);

    overwrite_in_place(p, 131072, 'b', "BBBB");

    (void) read4(r, &got);
    if (!got && is_identity_error(r))
        ok("B a different-size rewrite is refused");
    else
        bad("B different-size rewrite", got ? "served" : r->last_error());
    delete r;
}

void case_truncate_rewrite()
{
    std::string p = P("c");
    write_file(p, 65536, 'a', "AAAA", O_TRUNC);
    std::string err;
    xpb::ObjectReader *r = xpb::open_file_reader(p.c_str(), &err);
    if (!r) { bad("B2 open", err); return; }
    bool got = false;
    (void) read4(r, &got);

    /* Truncate and write the same LENGTH back: size ends up identical, so only
     * the timestamp distinguishes it. This is the case a size-only token
     * misses. */
    write_file(p, 65536, 'b', "BBBB", O_TRUNC);

    (void) read4(r, &got);
    if (!got && is_identity_error(r))
        ok("B2 truncate-and-rewrite to the SAME size is refused");
    else
        bad("B2 truncate+rewrite same size", got ? "served" : r->last_error());
    delete r;
}

void case_append()
{
    std::string p = P("d");
    write_file(p, 65536, 'a', "AAAA", O_TRUNC);
    std::string err;
    xpb::ObjectReader *r = xpb::open_file_reader(p.c_str(), &err);
    if (!r) { bad("B3 open", err); return; }
    bool got = false;
    (void) read4(r, &got);

    {
        std::vector<char> tail(4096, 'z');
        int fd = ::open(p.c_str(), O_WRONLY | O_APPEND);
        if (::write(fd, tail.data(), tail.size()) < 0) perror("append");
        ::close(fd);
    }

    (void) read4(r, &got);
    /* Conservative by design: the bytes already read are intact, but the
     * object is not the one that was opened, and a footer read from a file of
     * size N is not authoritative for size N+M. */
    if (!got && is_identity_error(r))
        ok("B3 an append is refused, conservatively (the object grew)");
    else
        bad("B3 append was served, so a grown file is treated as the same object",
            got ? "served" : r->last_error());
    delete r;
}

/* ---- controls that must NOT be flagged ---- */

void case_atomic_rename_replace()
{
    std::string p = P("e");
    write_file(p, 65536, 'a', "AAAA", O_TRUNC);
    std::string err;
    xpb::ObjectReader *r = xpb::open_file_reader(p.c_str(), &err);
    if (!r) { bad("C open", err); return; }
    bool got = false;
    std::string first = read4(r, &got);

    /* The safe publishing idiom: write elsewhere, rename over. Our descriptor
     * keeps the old inode, so its bytes cannot change. */
    std::string tmp = P("e.tmp");
    write_file(tmp, 131072, 'b', "BBBB", O_TRUNC);
    ::rename(tmp.c_str(), p.c_str());

    std::string second = read4(r, &got);
    if (got && second == first && second == "AAAA")
        ok("C an atomic rename replacement does NOT invalidate the open reader");
    else if (!got && is_identity_error(r))
        bad("C the guard wrongly condemned an atomic replace -- the safe idiom");
    else
        bad("C atomic replace", got ? second : r->last_error());

    /* And it must keep working for the rest of the object, not just offset 0. */
    std::vector<char> big(1024);
    if (r->read_exact(32768, 1024, big.data()) && big[0] == 'a')
        ok("C and keeps reading the original inode further in");
    else
        bad("C later read after rename", r->last_error());
    delete r;
}

void case_rename_away_then_new()
{
    std::string p = P("f");
    write_file(p, 65536, 'a', "AAAA", O_TRUNC);
    std::string err;
    xpb::ObjectReader *r = xpb::open_file_reader(p.c_str(), &err);
    if (!r) { bad("D open", err); return; }
    bool got = false;
    (void) read4(r, &got);

    ::rename(p.c_str(), P("f.moved").c_str());
    write_file(p, 65536, 'b', "BBBB", O_TRUNC);   /* a NEW inode at the path */

    std::string second = read4(r, &got);
    if (got && second == "AAAA")
        ok("D a new file appearing at the pathname does not disturb the reader");
    else if (!got && is_identity_error(r))
        bad("D the guard followed the PATHNAME instead of the descriptor");
    else
        bad("D rename away", got ? second : r->last_error());
    delete r;
}

void case_unlink_while_open()
{
    std::string p = P("g");
    write_file(p, 65536, 'a', "AAAA", O_TRUNC);
    std::string err;
    xpb::ObjectReader *r = xpb::open_file_reader(p.c_str(), &err);
    if (!r) { bad("E open", err); return; }
    bool got = false;
    (void) read4(r, &got);

    ::unlink(p.c_str());

    std::string second = read4(r, &got);
    if (got && second == "AAAA")
        ok("E unlinking the pathname does not disturb an already-open reader");
    else if (!got && is_identity_error(r))
        bad("E the guard flagged an unlink, which does not change the reader's bytes");
    else
        bad("E unlink", got ? second : r->last_error());
    delete r;
}

/* ---- the invariant counter is transport-general ---- */

void case_conflict_counter()
{
    std::string p = P("h");
    write_file(p, 65536, 'a', "AAAA", O_TRUNC);
    std::string err;
    xpb::ObjectReader *r = xpb::open_file_reader(p.c_str(), &err);
    if (!r) { bad("F open", err); return; }
    bool got = false;
    (void) read4(r, &got);
    int64_t before = r->identity_conflicts();

    overwrite_in_place(p, 65536, 'b', "BBBB");
    (void) read4(r, &got);
    int64_t after = r->identity_conflicts();

    if (before == 0 && after == 1)
        ok("F the refusal is counted on the base class, not per transport");
    else
        bad("F identity_conflicts", std::to_string(before) + " -> " + std::to_string(after));

    /* A refused reader stays refused: it must not recover by itself. */
    (void) read4(r, &got);
    if (!got && r->identity_conflicts() == 2)
        ok("F a refused reader does not re-pin itself on a later read");
    else
        bad("F the reader recovered on its own",
            got ? "a later read succeeded" : std::to_string(r->identity_conflicts()));
    delete r;
}

/* ---- the guard must not break ordinary reading ---- */

void case_untouched_file()
{
    std::string p = P("i");
    write_file(p, 1 << 20, 'a', "AAAA", O_TRUNC);
    std::string err;
    xpb::ObjectReader *r = xpb::open_file_reader(p.c_str(), &err);
    if (!r) { bad("G open", err); return; }

    bool allgood = true;
    std::vector<char> buf(4096);
    for (int i = 0; i < 64; i++)
    {
        if (!r->read_exact(i * 4096, 4096, buf.data()))
        { allgood = false; break; }
        /* The first four bytes are the tag, not the fill. Checking buf[0]
         * against the fill failed on i == 0 for that reason, and the failure
         * was in the test rather than the guard. */
        char want = (i == 0) ? 'A' : 'a';
        if (buf[0] != want)
        { allgood = false; break; }
    }

    if (allgood && r->identity_conflicts() == 0)
        ok("G 64 reads of an untouched file all succeed, no false conflicts");
    else
        bad("G untouched file", r->last_error());
    delete r;
}

}   /* namespace */

int main(int argc, char **argv)
{
    if (argc > 1)
        WORK = argv[1];
    std::string mk = "mkdir -p " + WORK;
    if (system(mk.c_str()) != 0)
    { fprintf(stderr, "cannot create %s\n", WORK.c_str()); return 2; }

    printf("=== must be detected ===\n");
    case_same_size_overwrite();
    case_different_size();
    case_truncate_rewrite();
    case_append();

    printf("=== must NOT be flagged (the controls) ===\n");
    case_atomic_rename_replace();
    case_rename_away_then_new();
    case_unlink_while_open();

    printf("=== the invariant itself ===\n");
    case_conflict_counter();
    case_untouched_file();

    printf("\n############ %d ok, %d wrong ############\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
