/* Phase 2: the transport fails on purpose, through a real HTTP endpoint. */
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

static const char *CONTROL = nullptr;

static void mode(const char *m)
{
    FILE *f = fopen(CONTROL, "w");
    if (f) { fprintf(f, "%s\n", m); fclose(f); }
}

static xpb::ObjectReader *open_reader(int attempts, std::string *err)
{
    char buf[8]; snprintf(buf, sizeof(buf), "%d", attempts);
    setenv("XPB_S3_MAX_ATTEMPTS", buf, 1);
    return xpb::open_s3_reader("s3://xpb/reg_buh.parquet", err);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: fault_unit <control-file>\n"); return 2; }
    CONTROL = argv[1];
    std::string err;
    std::vector<char> buf(4096);

    /* ---------- control: the proxy is transparent when told to be ---------- */
    mode("ok");
    {
        xpb::ObjectReader *r = open_reader(1, &err);
        if (!r) { bad("open through the proxy", err); printf("\n%d ok, %d wrong\n", pass, fail); return 1; }
        int64_t sz = r->size();
        if (sz == 180143345 && r->read_exact(0, 16, buf.data()) && memcmp(buf.data(), "PAR1", 4) == 0)
            ok("through the proxy in pass-through mode, reads are normal");
        else bad("pass-through", r->last_error());
        delete r;
    }

    /* ---------- a truncated response is NOT an end of object -------------- */
    /* No retry (1 attempt), so the raw failure is visible. */
    mode("truncate_all");
    {
        xpb::ObjectReader *r = open_reader(1, &err);
        if (!r) { bad("open", err); }
        else
        {
            xpb::ReadResult rr = r->read_at_most(0, 4096, buf.data());
            std::string e = r->last_error();
            if (!rr.ok && !rr.eof && e.find("truncated response, not EOF") != std::string::npos)
                ok("a cut transfer is an error that says it is not EOF");
            else
                bad("truncated response",
                    "ok=" + std::to_string(rr.ok) + " eof=" + std::to_string(rr.eof) +
                    " bytes=" + std::to_string(rr.bytes) + " [" + e + "]");

            if (!r->read_exact(0, 4096, buf.data()))
                ok("read_exact refuses a cut transfer");
            else
                bad("read_exact accepted a cut transfer");
            delete r;
        }
    }

    /* ---------- a cut transfer on an END-OF-OBJECT range is not EOF ------- *
     *
     * The regression this file exists for most. A range whose Content-Range
     * reaches the end of the object -- which is every Parquet footer probe,
     * since the footer is read backwards from the end -- was classified as a
     * clean end of object when its transfer had been cut, because eof was
     * decided from Content-Range before the body was checked against its own
     * Content-Length. read_exact refused it, so nothing wrong escaped there;
     * read_at_most is what Arrow's sequential footer read uses, and it would
     * have been handed eof = true with half a footer.
     */
    mode("truncate_all");
    {
        xpb::ObjectReader *r = open_reader(1, &err);
        if (!r) { bad("open", err); }
        else
        {
            /* Ask for a range that genuinely ends at the object's end. */
            const int64_t at = 180143345 - 65536;
            std::vector<char> big(65536);
            xpb::ReadResult rr = r->read_at_most(at, 65536, big.data());
            std::string e = r->last_error();
            if (!rr.ok && !rr.eof && e.find("transfer cut at") != std::string::npos)
                ok("a cut transfer on an end-of-object range is a failure, not EOF");
            else if (rr.ok && rr.eof)
                bad("a cut footer read was reported as a clean end of object",
                    "bytes=" + std::to_string(rr.bytes) + " eof=1");
            else
                bad("cut end-of-object range",
                    "ok=" + std::to_string(rr.ok) + " eof=" + std::to_string(rr.eof) +
                    " [" + e + "]");
            delete r;
        }
    }

    /* ---------- and a GENUINE end-of-object short read still is EOF ------- */
    /* The control: without it the fix above could be "never trust a short
     * read", which would make the reader unable to read a Parquet footer. */
    mode("ok");
    {
        xpb::ObjectReader *r = open_reader(1, &err);
        if (!r) { bad("open", err); }
        else
        {
            std::vector<char> big(200);
            xpb::ReadResult rr = r->read_at_most(180143345 - 4, 100, big.data());
            if (rr.ok && rr.bytes == 4 && rr.eof)
                ok("an honest short range at the object's end is still EOF");
            else
                bad("honest end-of-object read",
                    "ok=" + std::to_string(rr.ok) + " bytes=" + std::to_string(rr.bytes) +
                    " eof=" + std::to_string(rr.eof));
            delete r;
        }
    }

    /* ---------- retry turns a transient failure into a success ------------ */
    mode("fail_first:1");
    {
        xpb::ObjectReader *r = open_reader(3, &err);
        if (!r) { bad("open with retry", err); }
        else
        {
            /* size() is the first exchange and absorbs the injected 503. */
            int64_t sz = r->size();
            int64_t att = xpb::s3_http_attempts(r), ret = xpb::s3_retries(r);
            if (sz == 180143345 && ret >= 1 && att > xpb::s3_head_calls(r))
                ok("a transient 503 is retried and the read succeeds");
            else
                bad("transient retry", "size=" + std::to_string(sz) +
                    " attempts=" + std::to_string(att) + " retries=" + std::to_string(ret));
            delete r;
        }
    }

    /* ---------- the same failure with retry DISABLED must fail ------------ */
    /* Otherwise "retry made it work" is not shown to be the reason. */
    mode("fail_first:1");
    {
        xpb::ObjectReader *r = open_reader(1, &err);
        if (!r) { bad("open", err); }
        else
        {
            int64_t sz = r->size();
            if (sz < 0 && std::string(r->last_error()).find("503") != std::string::npos)
                ok("the same 503 with retry disabled fails, so retry is what fixed it");
            else bad("503 without retry", "size=" + std::to_string(sz) + " " + r->last_error());
            delete r;
        }
    }

    /* ---------- a truncated response is retried and recovered ------------ */
    /*
     * The mode is set AFTER size(), not before: the proxy triggers on an
     * exchange ordinal and resets that counter whenever the mode file changes,
     * so arming it here makes the next GET exchange number 1. Armed before
     * the open, the injected truncation landed on the HEAD instead and the
     * GET came through clean -- the assertion then reported retries=0 and the
     * case tested nothing.
     */
    mode("ok");
    {
        xpb::ObjectReader *r = open_reader(3, &err);
        if (!r) { bad("open", err); }
        else
        {
            (void) r->size();
            mode("truncate_first:1");
            int64_t before = xpb::s3_bytes_transferred(r);
            if (r->read_exact(0, 4096, buf.data()) && memcmp(buf.data(), "PAR1", 4) == 0)
            {
                int64_t after = xpb::s3_bytes_transferred(r);
                if (xpb::s3_retries(r) >= 1 && (after - before) > 4096)
                    ok("a cut transfer is retried, recovered, and costs more bytes than it returns");
                else
                    bad("truncated retry accounting",
                        "retries=" + std::to_string(xpb::s3_retries(r)) +
                        " transferred_delta=" + std::to_string(after - before));
            }
            else bad("truncated retry did not recover", r->last_error());
            delete r;
        }
    }

    /* ---------- a permanent failure stays a failure ---------------------- */
    mode("permanent");
    {
        xpb::ObjectReader *r = open_reader(3, &err);
        if (!r) { bad("open", err); }
        else
        {
            int64_t sz = r->size();
            int64_t ret = xpb::s3_retries(r);
            if (sz < 0 && ret == 2)
                ok("a permanent 500 is retried exactly to the bound and then fails");
            else bad("permanent failure", "size=" + std::to_string(sz) +
                     " retries=" + std::to_string(ret) + " " + r->last_error());
            delete r;
        }
    }

    /* ---------- an object shorter than expected --------------------------- */
    /* HEAD says 1000 bytes; the object is really 180 MB. A reader that trusted
     * only the byte count would mis-handle this; what must not happen is a
     * wrong answer presented as a correct one. */
    mode("short_object:1000");
    {
        xpb::ObjectReader *r = open_reader(1, &err);
        if (!r) { bad("open", err); }
        else
        {
            int64_t sz = r->size();
            if (sz == 1000)
            {
                ok("size() reports what the server states, even when it is wrong");
                /* Reading inside the claimed size still works; reading past it
                 * is the server's call, not an inference from the count. */
                if (r->read_exact(0, 16, buf.data()) && memcmp(buf.data(), "PAR1", 4) == 0)
                    ok("a read inside the claimed size succeeds");
                else bad("read inside the claimed size", r->last_error());
            }
            else bad("short_object", "size=" + std::to_string(sz));
            delete r;
        }
    }

    /* ---------- a 200 to a ranged request is refused, not sliced ---------- */
    mode("ignore_range");
    {
        xpb::ObjectReader *r = open_reader(1, &err);
        if (!r) { bad("open", err); }
        else
        {
            xpb::ReadResult rr = r->read_at_most(0, 16, buf.data());
            std::string e = r->last_error();
            if (!rr.ok && e.find("ignored Range") != std::string::npos)
                ok("a 200 to a ranged request is refused rather than sliced");
            else bad("ignore_range", "ok=" + std::to_string(rr.ok) + " [" + e + "]");
            delete r;
        }
    }

    /* ---------- a response with no Content-Length is refused -------------- */
    mode("no_length");
    {
        xpb::ObjectReader *r = open_reader(1, &err);
        if (!r) { bad("open", err); }
        else
        {
            (void) r->size();
            xpb::ReadResult rr = r->read_at_most(0, 16, buf.data());
            std::string e = r->last_error();
            if (!rr.ok && e.find("no Content-Length") != std::string::npos)
                ok("a response with no Content-Length is refused, not guessed at");
            else bad("no_length", "ok=" + std::to_string(rr.ok) + " [" + e + "]");
            delete r;
        }
    }

    mode("ok");
    printf("\n############ %d ok, %d wrong ############\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
