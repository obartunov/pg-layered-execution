/*
 * Does connection reuse change anything except the number of connections?
 *
 *   s3_keepalive_unit <upstream-host:port> [relay-port]
 *
 * Every case reads the SAME ranges and compares the bytes against the local
 * copy of the object, so "the connection was reused" is never the only thing
 * asserted. A test that counted connections and not bytes would pass over a
 * reader that reused a socket and returned the wrong half of the object.
 *
 * The cases that matter are the last two. A reused connection has one failure
 * mode a fresh one does not -- the server closed it while it was idle -- and the
 * reader replays through that. Case D proves the replay happens and costs no
 * retry budget; case E proves it does NOT happen when response bytes have
 * already arrived, because replaying there would hide a cut transfer, which is
 * the one thing the v1 contract exists to surface.
 */
#include "xpb_s3_reader.h"
#include "xpb_object_reader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int pass = 0, fail = 0;
void ok(const std::string &w)  { printf("  ok      %s\n", w.c_str()); pass++; }
void bad(const std::string &w, const std::string &d = "")
{ printf("  FAIL    %s%s%s\n", w.c_str(), d.empty() ? "" : " -- ", d.c_str()); fail++; }
void note(const std::string &w) { printf("          %s\n", w.c_str()); }

const char *kUri  = "s3://xpb/reg_buh.parquet";
const char *kLocal =
    "/home/claude/pg-layered-execution/benchmarks/02-batch-joins/data/reg_buh.parquet";

/* The ranges every case reads. Spread over the object, and one of them lands on
 * the last bytes so the end-of-object path is exercised through a reused
 * socket too. */
struct Range { int64_t off, len; };
std::vector<Range> ranges(int64_t size)
{
    std::vector<Range> v;
    for (int i = 0; i < 16; i++)
        v.push_back({(size / 20) * i + 7, 4096});
    v.push_back({size - 8, 8});
    return v;
}

/* What the object really contains, read straight from the local file. */
bool local_bytes(int64_t off, int64_t len, std::vector<char> *out)
{
    int fd = ::open(kLocal, O_RDONLY);
    if (fd < 0)
        return false;
    out->assign(static_cast<size_t>(len), 0);
    ssize_t n = ::pread(fd, out->data(), static_cast<size_t>(len), off);
    ::close(fd);
    return n == len;
}

struct Relay
{
    pid_t pid = -1;
    int   port = 0;

    bool start(const std::string &upstream, int p, const char *extra1 = nullptr,
               const char *extra2 = nullptr, const char *extra3 = nullptr,
               const char *extra4 = nullptr)
    {
        port = p;
        std::string sport = std::to_string(p);
        /* The child inherits this process's stdout buffer; anything still in
         * it would be written twice. */
        ::fflush(nullptr);
        pid = ::fork();
        if (pid < 0)
            return false;
        if (pid == 0)
        {
            const char *args[16];
            int n = 0;
            args[n++] = "python3";
            args[n++] = "/home/claude/pg-layered-execution/extension/xpb_parquet/"
                        "test/s3-latency-relay.py";
            args[n++] = "--listen";       args[n++] = sport.c_str();
            args[n++] = "--upstream";     args[n++] = upstream.c_str();
            args[n++] = "--connect-delay-ms"; args[n++] = "0";
            if (extra1) args[n++] = extra1;
            if (extra2) args[n++] = extra2;
            if (extra3) args[n++] = extra3;
            if (extra4) args[n++] = extra4;
            args[n] = nullptr;
            if (::freopen("/dev/null", "w", stdout) == nullptr)
                _exit(126);
            ::execvp("python3", const_cast<char **>(args));
            _exit(127);
        }
        ::usleep(900 * 1000);           /* the relay binds and listens */
        return true;
    }
    void stop()
    {
        if (pid > 0)
        {
            ::kill(pid, SIGTERM);
            int st = 0;
            ::waitpid(pid, &st, 0);
            pid = -1;
        }
    }
    ~Relay() { stop(); }
};

/* Read every range and check each one against the local file. Returns false on
 * the first mismatch or read failure, with why in `why`. */
bool read_and_verify(xpb::ObjectReader *r, const std::vector<Range> &rs,
                     std::string *why)
{
    for (const Range &g : rs)
    {
        std::vector<char> got(static_cast<size_t>(g.len));
        if (!r->read_exact(g.off, g.len, got.data()))
        {
            *why = std::string("read at ") + std::to_string(g.off) + ": " +
                   r->last_error();
            return false;
        }
        std::vector<char> want;
        if (!local_bytes(g.off, g.len, &want))
        {
            *why = "cannot read the local copy for comparison";
            return false;
        }
        if (got != want)
        {
            *why = std::string("bytes differ at offset ") + std::to_string(g.off);
            return false;
        }
    }
    return true;
}

}   /* namespace */

int main(int argc, char **argv)
{
    std::string upstream = argc > 1 ? argv[1] : "127.0.0.1:9000";
    int relay_port = argc > 2 ? atoi(argv[2]) : 9177;

    const char *ep = getenv("XPB_S3_ENDPOINT");
    if (ep == nullptr)
    { printf("  skip    XPB_S3_ENDPOINT is not set\n"); return 0; }

    std::string direct = std::string("http://") + upstream;
    std::string via    = "http://127.0.0.1:" + std::to_string(relay_port);

    /* ---- A. reuse on, against the endpoint itself ---- */
    int64_t nreads = 0;
    {
        setenv("XPB_S3_ENDPOINT", direct.c_str(), 1);
        std::string err;
        xpb::ObjectReader *r = xpb::open_s3_reader(kUri, &err);
        if (r == nullptr) { bad("A open", err); return 1; }
        int64_t sz = r->size();
        std::vector<Range> rs = ranges(sz);
        nreads = static_cast<int64_t>(rs.size());

        std::string why;
        bool good = read_and_verify(r, rs, &why);
        int64_t conns = xpb::s3_connections(r);
        int64_t gets  = xpb::s3_get_calls(r);
        note("A reuse on : GET=" + std::to_string(gets) +
             " connections=" + std::to_string(conns) +
             " reconnects=" + std::to_string(xpb::s3_reconnects(r)) +
             " retries=" + std::to_string(xpb::s3_retries(r)));

        if (!good)
            bad("A the reads themselves", why);
        else
            ok("A " + std::to_string(gets) + " range reads all return the "
               "object's real bytes");

        /* One connection for the HEAD and every GET. Asserted as "fewer than
         * the requests", not "exactly 1": a server is entitled to close a
         * persistent connection at any time, and this must not become a test
         * that fails when it does. The strict case is B below, where the
         * endpoint is known to persist. */
        if (conns >= 1 && conns < gets + 1)
            ok("A connections (" + std::to_string(conns) + ") are fewer than "
               "requests (" + std::to_string(gets + 1) + ")");
        else
            bad("A no reuse happened", std::to_string(conns) + " connections for " +
                std::to_string(gets + 1) + " requests");
        if (xpb::s3_reconnects(r) == 0)
            ok("A a healthy endpoint needs no reconnect");
        else
            bad("A reconnected against a healthy endpoint",
                std::to_string(xpb::s3_reconnects(r)));
        delete r;
    }

    /* ---- B. reuse off: the baseline, from the same binary ---- */
    {
        setenv("XPB_S3_ENDPOINT", direct.c_str(), 1);
        setenv("XPB_S3_CONNECTION_REUSE", "off", 1);
        std::string err;
        xpb::ObjectReader *r = xpb::open_s3_reader(kUri, &err);
        if (r == nullptr) { bad("B open", err); unsetenv("XPB_S3_CONNECTION_REUSE"); return 1; }
        std::vector<Range> rs = ranges(r->size());
        std::string why;
        bool good = read_and_verify(r, rs, &why);
        int64_t conns = xpb::s3_connections(r);
        int64_t gets  = xpb::s3_get_calls(r);
        note("B reuse off: GET=" + std::to_string(gets) +
             " connections=" + std::to_string(conns));
        if (good && conns == gets + 1)
            ok("B with reuse off it is one connection per request (" +
               std::to_string(conns) + "), same bytes");
        else if (!good)
            bad("B the reads themselves", why);
        else
            bad("B baseline connection count", std::to_string(conns) + " for " +
                std::to_string(gets + 1) + " requests");
        delete r;
        unsetenv("XPB_S3_CONNECTION_REUSE");
    }

    /* ---- C. a relay in between changes nothing on its own ---- */
    {
        Relay relay;
        if (!relay.start(upstream, relay_port))
        { bad("C cannot start the relay"); return 1; }
        setenv("XPB_S3_ENDPOINT", via.c_str(), 1);
        std::string err;
        xpb::ObjectReader *r = xpb::open_s3_reader(kUri, &err);
        if (r == nullptr) { bad("C open through the relay", err); return 1; }
        std::vector<Range> rs = ranges(r->size());
        std::string why;
        bool good = read_and_verify(r, rs, &why);
        note("C via relay : connections=" + std::to_string(xpb::s3_connections(r)));
        if (good && xpb::s3_connections(r) < xpb::s3_get_calls(r) + 1)
            ok("C the relay is transparent: same bytes, still reused");
        else
            bad("C through the relay", good ? "no reuse" : why);
        delete r;
        relay.stop();
    }

    /* ---- D. the server closed the idle socket: replay, no retry spent ---- */
    {
        Relay relay;
        /* Every connection serves exactly one request and is killed when the
         * second arrives, so EVERY reused socket is dead. Deterministic: an
         * ordinal, not an idle timer. */
        if (!relay.start(upstream, relay_port, "--kill-at-request", "2"))
        { bad("D cannot start the relay"); return 1; }
        setenv("XPB_S3_ENDPOINT", via.c_str(), 1);
        std::string err;
        xpb::ObjectReader *r = xpb::open_s3_reader(kUri, &err);
        if (r == nullptr) { bad("D open", err); return 1; }
        std::vector<Range> rs = ranges(r->size());
        std::string why;
        bool good = read_and_verify(r, rs, &why);
        int64_t conns = xpb::s3_connections(r);
        int64_t rec   = xpb::s3_reconnects(r);
        int64_t ret   = xpb::s3_retries(r);
        int64_t gets  = xpb::s3_get_calls(r);
        note("D killed idle: GET=" + std::to_string(gets) +
             " connections=" + std::to_string(conns) +
             " reconnects=" + std::to_string(rec) +
             " retries=" + std::to_string(ret));

        if (good)
            ok("D every read still returns the object's real bytes");
        else
            bad("D a dead reused socket lost data", why);
        /* The replay must actually have happened: without this the case passes
         * on a reader that never reused anything. */
        if (rec == gets)
            ok("D one reconnect per read after the first (" + std::to_string(rec) + ")");
        else
            bad("D reconnect count", std::to_string(rec) + " for " +
                std::to_string(gets) + " GETs");
        if (ret == 0)
            ok("D and it spent no retry budget (retries = 0)");
        else
            bad("D the reconnect was charged as a retry", std::to_string(ret));
        delete r;
        relay.stop();
    }

    /* ---- E. a cut transfer must NOT be replayed away ---- */
    {
        Relay relay;
        /* The first request on each connection gets a response truncated after
         * 400 bytes: headers arrive, the body does not. The reader has received
         * bytes, so a replay would be wrong. */
        if (!relay.start(upstream, relay_port, "--cut-at-request", "1",
                         "--cut-bytes", "400"))
        { bad("E cannot start the relay"); return 1; }
        setenv("XPB_S3_ENDPOINT", via.c_str(), 1);
        setenv("XPB_S3_MAX_ATTEMPTS", "1", 1);   /* see the raw failure */
        std::string err;
        xpb::ObjectReader *r = xpb::open_s3_reader(kUri, &err);
        if (r == nullptr)
        {
            /* The HEAD is request 1 on its own connection, so the cut may land
             * there instead. Either way nothing was silently accepted, which
             * is what this case is about. */
            if (err.find("not EOF") != std::string::npos ||
                err.find("closed") != std::string::npos ||
                err.find("Content-Length") != std::string::npos)
                ok("E a cut response at open is an error, not an empty object");
            else
                bad("E open failed for another reason", err);
            unsetenv("XPB_S3_MAX_ATTEMPTS");
            relay.stop();
        }
        else
        {
            int64_t before = xpb::s3_reconnects(r);
            char buf[4096];
            bool got = r->read_exact(4096, sizeof(buf), buf);
            int64_t after = xpb::s3_reconnects(r);
            std::string e = r->last_error();
            note(std::string("E cut body  : ok=") + (got ? "1" : "0") +
                 " reconnects " + std::to_string(before) + " -> " +
                 std::to_string(after) + "  err=" + e);
            if (!got)
                ok("E a cut transfer is an error, not a short read");
            else
                bad("E a cut transfer was accepted as data");
            if (after == before)
                ok("E and it was NOT replayed away (reconnects unchanged)");
            else
                bad("E the reader replayed over a partially received response",
                    std::to_string(before) + " -> " + std::to_string(after));
            delete r;
            unsetenv("XPB_S3_MAX_ATTEMPTS");
            relay.stop();
        }
    }

    setenv("XPB_S3_ENDPOINT", ep, 1);
    printf("\n######## %d ok, %d wrong ########\n", pass, fail);
    (void) nreads;
    return fail == 0 ? 0 : 1;
}
