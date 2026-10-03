/*
 * Phase 1 direct-API checks: the things that are clearer below SQL than
 * through it -- close idempotence, read after close, a refused endpoint, a
 * malformed URI, and exact-or-error at the contract level.
 */
#include "xpb_s3_reader.h"
#include "xpb_object_reader.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int pass = 0, fail = 0;
static void ok(const char *w)  { printf("  ok      %s\n", w); pass++; }
static void bad(const char *w, const std::string &d = "")
{ printf("  FAIL    %s%s%s\n", w, d.empty() ? "" : " -- ", d.c_str()); fail++; }

int main()
{
    std::string err;
    const char *uri = "s3://xpb/reg_buh.parquet";

    /* ---- a malformed URI is refused, not guessed at ---- */
    err.clear();
    if (xpb::open_s3_reader("s3://nokey", &err) == nullptr && err.find("expected s3://bucket/key") != std::string::npos)
        ok("a URI with no key is refused with a usable message");
    else bad("a URI with no key", err);

    err.clear();
    if (xpb::open_s3_reader("/tmp/x.parquet", &err) == nullptr)
        ok("a local path is not accepted as an S3 URI");
    else bad("a local path was accepted as S3");

    /* ---- https is refused rather than downgraded to cleartext ---- */
    {
        const char *saved = getenv("XPB_S3_ENDPOINT");
        std::string keep = saved ? saved : "";
        setenv("XPB_S3_ENDPOINT", "https://s3.amazonaws.com", 1);
        err.clear();
        xpb::ObjectReader *r = xpb::open_s3_reader(uri, &err);
        if (r == nullptr && err.find("no TLS") != std::string::npos)
            ok("an https endpoint is refused, not silently downgraded");
        else { bad("https endpoint", err); delete r; }
        if (keep.empty()) unsetenv("XPB_S3_ENDPOINT");
        else setenv("XPB_S3_ENDPOINT", keep.c_str(), 1);
    }

    /* ---- the real thing ---- */
    err.clear();
    xpb::ObjectReader *r = xpb::open_s3_reader(uri, &err);
    if (r == nullptr) { bad("open", err); printf("\n%d ok, %d wrong\n", pass, fail); return 1; }
    ok("opened s3://xpb/reg_buh.parquet");

    int64_t sz = r->size();
    if (sz == 180143345) ok("size() via HEAD returns the object's length");
    else bad("size()", std::to_string(sz));

    /* size() is fixed for the reader's life and must not re-HEAD */
    int64_t head1 = xpb::s3_head_calls(r);
    (void) r->size(); (void) r->size();
    if (xpb::s3_head_calls(r) == head1)
        ok("size() is cached, as the contract requires (no extra HEAD)");
    else bad("size() issued another HEAD");

    /* ---- a single exact range read ---- */
    std::vector<char> buf(16);
    if (r->read_exact(0, 16, buf.data()) && memcmp(buf.data(), "PAR1", 4) == 0)
        ok("read_exact of the first 16 bytes returns the Parquet magic");
    else bad("read_exact at 0", r->last_error());

    /* the last 4 bytes are the trailing magic */
    if (r->read_exact(sz - 4, 4, buf.data()) && memcmp(buf.data(), "PAR1", 4) == 0)
        ok("read_exact at the very end returns the trailing magic");
    else bad("read_exact at the end", r->last_error());

    /* ---- exact-or-error: a range straddling the end must FAIL ---- */
    if (!r->read_exact(sz - 4, 100, buf.data()))
    {
        std::string e = r->last_error();
        if (e.find("past end of object") != std::string::npos)
            ok("a range running past the end is an error naming the end, not a short read");
        else bad("straddling read failed with the wrong reason", e);
    }
    else bad("a range past the end was accepted by read_exact");

    /* ---- read_at_most past the end: eof TRUE, and stated by the server ---- */
    {
        xpb::ReadResult rr = r->read_at_most(sz + 1000, 100, buf.data());
        if (rr.ok && rr.bytes == 0 && rr.eof)
            ok("read_at_most entirely past the end: 0 bytes and eof STATED (HTTP 416)");
        else bad("read_at_most past the end",
                 "ok=" + std::to_string(rr.ok) + " bytes=" + std::to_string(rr.bytes) +
                 " eof=" + std::to_string(rr.eof) + " " + r->last_error());
    }

    /* ---- a straddling read_at_most is short WITH eof set ---- */
    {
        std::vector<char> big(200);
        xpb::ReadResult rr = r->read_at_most(sz - 4, 100, big.data());
        if (rr.ok && rr.bytes == 4 && rr.eof)
            ok("read_at_most straddling the end: 4 bytes and eof set from Content-Range");
        else bad("straddling read_at_most",
                 "ok=" + std::to_string(rr.ok) + " bytes=" + std::to_string(rr.bytes) +
                 " eof=" + std::to_string(rr.eof) + " " + r->last_error());
    }

    /* ---- negative and overflowing ranges are errors, not clamps ---- */
    if (!r->read_exact(-1, 10, buf.data())) ok("a negative offset is refused");
    else bad("negative offset accepted");
    if (!r->read_exact(1, INT64_MAX, buf.data())) ok("an overflowing range is refused");
    else bad("overflowing range accepted");

    /* ---- close is idempotent, and a read after close is an error ---- */
    r->close();
    r->close();
    r->close();
    ok("close() three times does not crash");
    if (!r->read_exact(0, 16, buf.data()))
        ok("a read after close is an error, not undefined");
    else bad("a read after close succeeded");
    if (r->size() < 0) ok("size() after close is an error");
    else bad("size() after close succeeded");

    delete r;
    ok("destructor after an explicit close does not double-free");

    printf("\n############ %d ok, %d wrong ############\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
