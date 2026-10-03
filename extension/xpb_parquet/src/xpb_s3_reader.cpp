/*
 * xpb_s3_reader.cpp -- HTTP ranged GET behind the ObjectReader contract.
 *
 * The only code in the module that knows a socket, an HTTP header or an AWS
 * signature. Includes no Arrow header and no PostgreSQL header: the interrupt
 * hook arrives as a function pointer like it does for FileReader, and the
 * build rule for this file has no Arrow include path.
 *
 * Blocking, one request per read, on the caller's thread. See the header for
 * why that is a choice and not an omission.
 */
#include "xpb_s3_reader.h"
#include "xpb_object_reader.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace xpb {

namespace {

const char *const kEmptyPayloadSha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

std::string
to_hex(const unsigned char *p, size_t n)
{
    static const char *d = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; i++)
    {
        out.push_back(d[p[i] >> 4]);
        out.push_back(d[p[i] & 0x0f]);
    }
    return out;
}

std::string
sha256_hex(const std::string &s)
{
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(s.data()), s.size(), md);
    return to_hex(md, sizeof(md));
}

std::string
hmac_raw(const std::string &key, const std::string &data)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;

    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char *>(data.data()), data.size(),
         md, &len);
    return std::string(reinterpret_cast<char *>(md), len);
}

/* Case-insensitive header lookup over a raw header block. */
std::string
header_value(const std::string &headers, const std::string &name)
{
    std::string lower;
    lower.reserve(headers.size());
    for (char c : headers)
        lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));

    std::string needle = "\r\n";
    for (char c : name)
        needle.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
    needle += ":";

    size_t at = lower.find(needle);
    if (at == std::string::npos)
        return std::string();
    size_t vs = at + needle.size();
    size_t ve = lower.find("\r\n", vs);
    if (ve == std::string::npos)
        return std::string();
    std::string v = headers.substr(vs, ve - vs);
    size_t b = v.find_first_not_of(" \t");
    if (b == std::string::npos)
        return std::string();
    size_t e = v.find_last_not_of(" \t\r");
    return v.substr(b, e - b + 1);
}

/*
 * Does a header carry `token`, case-insensitively, as one of its comma-separated
 * values? Written for `Connection: close`, which a proxy may legitimately send
 * as "Connection: close, foo" -- a plain equality test would miss it and the
 * reader would then try to reuse a socket the peer is closing.
 */
bool
header_is(const std::string &headers, const std::string &name,
          const std::string &token)
{
    std::string v = header_value(headers, name);
    for (char &c : v)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);

    size_t at = 0;
    while (at < v.size())
    {
        size_t comma = v.find(',', at);
        std::string one = v.substr(at, comma == std::string::npos
                                       ? std::string::npos : comma - at);
        size_t b = one.find_first_not_of(" \t");
        size_t e = one.find_last_not_of(" \t");
        if (b != std::string::npos && one.substr(b, e - b + 1) == token)
            return true;
        if (comma == std::string::npos)
            break;
        at = comma + 1;
    }
    return false;
}

struct Endpoint
{
    std::string host;
    int         port = 80;
};

bool
parse_endpoint(const char *url, Endpoint *out, std::string *err)
{
    if (url == nullptr || *url == '\0')
    {
        *err = "XPB_S3_ENDPOINT is not set";
        return false;
    }
    std::string u(url);
    if (u.compare(0, 8, "https://") == 0)
    {
        /*
         * Refused rather than downgraded. A reader that silently spoke plain
         * HTTP to an https:// endpoint would send credentials in clear.
         */
        *err = "XPB_S3_ENDPOINT is https, and this reader has no TLS: refusing "
               "rather than sending credentials in clear";
        return false;
    }
    if (u.compare(0, 7, "http://") == 0)
        u = u.substr(7);

    size_t slash = u.find('/');
    if (slash != std::string::npos)
        u = u.substr(0, slash);

    size_t colon = u.rfind(':');
    if (colon == std::string::npos)
    {
        out->host = u;
        out->port = 80;
    }
    else
    {
        out->host = u.substr(0, colon);
        out->port = atoi(u.c_str() + colon + 1);
        if (out->port <= 0 || out->port > 65535)
        {
            *err = "XPB_S3_ENDPOINT has a bad port";
            return false;
        }
    }
    if (out->host.empty())
    {
        *err = "XPB_S3_ENDPOINT has no host";
        return false;
    }
    return true;
}

/*
 * Canonicalisation and signing, as a free function with the date injected.
 *
 * Split out of the reader for one reason: it is the only part of the transport
 * that can be checked against an independent implementation, and it cannot be
 * if the timestamp comes from the clock. The test compares the Authorization
 * header this produces with the one botocore produces for the same request --
 * which checks the canonical request too, not just the HMAC chain. A correct
 * signing algorithm over a wrong canonical request still fails against AWS.
 */
/*
 * The canonical request, exactly as AWS SigV4 defines it.
 *
 * Separate from signing so a test can compare THIS against an independent
 * implementation's canonical request -- a comparison that needs no frozen
 * clock, unlike comparing finished Authorization headers. Getting this wrong
 * while signing correctly still fails against AWS, so it is the part worth
 * checking directly.
 *
 * Headers are canonicalised sorted by name, which for this fixed set means
 * host, range (when present), x-amz-content-sha256, x-amz-date.
 */
std::string
s3_canonical_request(const char *method, const std::string &uri,
                     const std::string &host, const std::string &range,
                     const std::string &amz_date, const std::string &if_match)
{
    std::string canon_headers = "host:" + host + "\n";
    std::string signed_headers = "host;";

    /*
     * Canonical headers are sorted by name, so if-match goes between host and
     * range. Getting this order wrong produces a signature the server rejects,
     * which is at least loud -- unlike the identity bug it is here to prevent.
     */
    if (!if_match.empty())
    {
        canon_headers += "if-match:" + if_match + "\n";
        signed_headers += "if-match;";
    }
    if (!range.empty())
    {
        canon_headers += "range:" + range + "\n";
        signed_headers += "range;";
    }
    canon_headers += std::string("x-amz-content-sha256:") + kEmptyPayloadSha256 + "\n";
    canon_headers += std::string("x-amz-date:") + amz_date + "\n";
    signed_headers += "x-amz-content-sha256;x-amz-date";

    return std::string(method) + "\n" +
           uri + "\n" +
           "" + "\n" +                 /* no query string */
           canon_headers + "\n" +
           signed_headers + "\n" +
           kEmptyPayloadSha256;
}

/* Forward: the canonical request, built by the same code that signs it. */
std::string s3_canonical_request(const char *method, const std::string &uri,
                                 const std::string &host, const std::string &range,
                                 const std::string &amz_date,
                                 const std::string &if_match);

std::string
build_signed_request(const char *method, const std::string &uri,
                     const std::string &host, const std::string &range,
                     const std::string &amz_date, const std::string &datestamp,
                     const std::string &region, const std::string &access,
                     const std::string &secret, const std::string &if_match,
                     bool keep_alive)
{
    std::string signed_headers = "host;";
    if (!if_match.empty())
        signed_headers += "if-match;";
    if (!range.empty())
        signed_headers += "range;";
    signed_headers += "x-amz-content-sha256;x-amz-date";

    std::string canonical_request =
        s3_canonical_request(method, uri, host, range, amz_date, if_match);

    std::string sig = sigv4_signature(canonical_request, amz_date, datestamp,
                      region, "s3", secret);
    std::string scope = std::string(datestamp) + "/" + region + "/s3/aws4_request";

    std::string req;
    req += std::string(method) + " " + uri + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    if (!if_match.empty())
        req += "If-Match: " + if_match + "\r\n";
    if (!range.empty())
        req += "Range: " + range + "\r\n";
    req += std::string("x-amz-content-sha256: ") + kEmptyPayloadSha256 + "\r\n";
    req += std::string("x-amz-date: ") + amz_date + "\r\n";
    req += "Authorization: AWS4-HMAC-SHA256 Credential=" + access + "/" + scope +
           ", SignedHeaders=" + signed_headers + ", Signature=" + sig + "\r\n";
    /*
     * Connection is a hop-by-hop header and is NOT part of the signed set, so
     * switching it changes nothing about the signature. HTTP/1.1 defaults to
     * persistent anyway; both values are stated explicitly so the wire shape
     * of each mode is unambiguous in a capture.
     */
    req += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    req += "\r\n";
    return req;
}

/* ------------------------------------------------------------- the reader -- */

class S3Reader : public ObjectReader
{
public:
    S3Reader(const Endpoint &ep, std::string bucket, std::string key,
             std::string access, std::string secret, std::string region)
        : ep_(ep), bucket_(std::move(bucket)), key_(std::move(key)),
          access_(std::move(access)), secret_(std::move(secret)),
          region_(std::move(region)) {}

    void set_max_attempts(int n) { max_attempts_ = n; }
    void set_io_timeout_ms(int n) { io_timeout_ms_ = n; }
    void set_require_identity(bool b) { require_identity_ = b; }
    /*
     * Off exists so the one-connection-per-request baseline can be measured
     * from the SAME binary; a before/after across two builds would compare two
     * compilations as well as two transports.
     */
    void set_reuse(bool b) { reuse_ = b; }
    const std::string &identity() const { return identity_; }
    void set_connect_timeout_ms(int n) { connect_timeout_ms_ = n; }

    ~S3Reader() override { close(); }

    int64_t head_calls()        const { return head_calls_; }
    int64_t get_calls()         const { return get_calls_; }
    int64_t http_errors()       const { return http_errors_; }
    int64_t bytes_transferred() const { return bytes_transferred_; }
    int64_t http_attempts()     const { return http_attempts_; }
    int64_t connections()       const { return connections_; }
    /*
     * The most requests ever in flight at once. Kept rather than removed with
     * the rest of the phase's instrumentation, because it is the only thing
     * that shows the single connection loses no parallelism: it reads 1 in
     * every scan shape, so serialising on one socket serialises nothing that
     * was concurrent. Without it, "a mutex around the connection is free" would
     * be an assertion about Arrow's scheduling rather than a measurement of it.
     */
    int64_t max_in_flight()     const { return max_in_flight_; }
    /*
     * Replays after a reused socket turned out to be already closed. Counted
     * apart from retries() because they are different events: a retry answers a
     * server that failed, a reconnect answers a connection that was gone before
     * the server saw anything.
     */
    int64_t reconnects()        const { return reconnects_; }
    int64_t retries()           const { return retries_done_; }

    int64_t size() override
    {
        if (closed_)
            return fail("size() on a closed reader");
        if (size_ >= 0)
            return size_;            /* fixed for the reader's life, as the
                                      * ObjectReader contract requires */

        head_calls_++;

        std::string headers, body;
        int status = -1;
        for (int attempt = 1; attempt <= max_attempts_; attempt++)
        {
            before_read();
            headers.clear();
            status = request("HEAD", "", &headers, &body, 0);
            bool retryable = (status < 0) || status == 429 ||
                             (status >= 500 && status < 600);
            if (!retryable || attempt == max_attempts_)
                break;
            retries_done_++;
            struct timespec ts = {0, 10 * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
        if (status < 0)
            return -1;
        if (status != 200)
        {
            http_errors_++;
            return fail("HEAD " + path() + ": HTTP " + std::to_string(status));
        }
        std::string cl = header_value(headers, "Content-Length");
        if (cl.empty())
            return fail("HEAD " + path() + ": no Content-Length");
        int64_t sz = strtoll(cl.c_str(), nullptr, 10);
        if (sz < 0)
            return fail("HEAD " + path() + ": bad Content-Length");

        /*
         * SIZE AND IDENTITY ARE ONE SNAPSHOT.
         *
         * They come from the same HEAD response and are stored together, so
         * there is no state in which the reader holds a length from one
         * incarnation of the object and reads ranges from another. That pair
         * is the whole of what this reader knows about "which object".
         *
         * The ETag is an OPAQUE IDENTITY TOKEN, not a content hash, and this
         * code must never treat it as one. The same bytes get different ETag
         * shapes depending on how they were written: a 22-part multipart
         * upload of the benchmark file gives
         * "c7333b8e11884cda65957f5f6e7e62c4-22", while a server-side copy of
         * the same content gives a single-part "5492fdb1...". Comparing an
         * ETag with a checksum we computed, or across objects, would be wrong.
         * It is only ever echoed back in If-Match.
         */
        std::string etag = header_value(headers, "ETag");

        if (etag.empty() && require_identity_)
        {
            /*
             * Refused rather than read unguarded. Without an identity token
             * there is no way to notice the object changing underneath a scan,
             * and that is a wrong-answer class: a footer from one incarnation
             * and data from another. A reader that cannot be made safe should
             * not silently become unsafe.
             */
            return fail("HEAD " + path() + ": the server returned no ETag, so "
                        "object identity cannot be pinned for the life of this "
                        "read; refusing rather than risking a mixed-version "
                        "scan (set XPB_S3_IDENTITY_GUARD=off to override, which "
                        "is for testing the unguarded behaviour and not for use)");
        }

        size_ = sz;
        identity_ = require_identity_ ? etag : std::string();
        return size_;
    }

    ReadResult read_at_most(int64_t offset, int64_t nbytes, void *out) override
    {
        clear_error();

        if (closed_)
        {
            fail("read on a closed reader");
            return ReadResult::failure();
        }
        if (!range_ok(offset, nbytes))
            return ReadResult::failure();
        if (nbytes == 0)
            return ReadResult::got(0, false);
        if (out == nullptr)
        {
            fail("read with a null buffer");
            return ReadResult::failure();
        }

        get_calls_++;

        char range[128];
        snprintf(range, sizeof(range), "bytes=%lld-%lld",
                 (long long) offset, (long long) (offset + nbytes - 1));

        /*
         * BOUNDED RETRY, here and nowhere above.
         *
         * The v1 contract puts retry inside the implementation, under
         * read_exact, so the Arrow adapter never learns that a read was
         * attempted more than once. Minimal on purpose: a fixed small number
         * of attempts, a fixed small delay, no exponential backoff, no jitter,
         * no circuit breaker. Those are policy, and policy with no measured
         * need is a guess.
         *
         * What is retryable is a judgement about whether another attempt could
         * plausibly differ, not about whether the error looks bad:
         *
         *   connect/send/recv failure   yes, the exchange did not complete
         *   5xx, 429                    yes, the server said "later"
         *   truncated body              yes, the transfer was cut; THIS is the
         *                               case v0 would have called EOF
         *   416                         NO, it is a valid and stable answer
         *   other 4xx                   NO, another attempt gives the same
         *   200 to a ranged request     NO, the endpoint is misconfigured
         *
         * The interrupt hook runs before EACH attempt, so a cancel is seen
         * between retries and not only between logical reads.
         */
        std::string headers, body;
        int status = -1;

        for (int attempt = 1; attempt <= max_attempts_; attempt++)
        {
            before_read();

            headers.clear();
            body.clear();
            status = request("GET", range, &headers, &body, nbytes);

            bool retryable = false;
            if (status < 0)
                retryable = true;                       /* exchange failed    */
            else if (status == 429 || (status >= 500 && status < 600))
                retryable = true;
            else if (status == 206 && body_is_truncated(headers, body))
                retryable = true;

            if (!retryable || attempt == max_attempts_)
                break;

            retries_done_++;
            struct timespec ts = {0, 10 * 1000 * 1000};  /* 10 ms, fixed */
            nanosleep(&ts, nullptr);
        }

        if (status < 0)
            return ReadResult::failure();

        /*
         * 416: the server states the range is past the end. That is an
         * authoritative end of object -- unlike a short body, which is not.
         */
        if (status == 416)
        {
            account(nbytes, 0);
            return ReadResult::got(0, true);
        }

        if (status == 200)
        {
            /*
             * Range ignored. Refused rather than sliced: a whole-object body in
             * answer to a range request is a misconfigured endpoint, and taking
             * the bytes we wanted out of it would make the fault invisible.
             */
            http_errors_++;
            fail("GET " + path() + ": server ignored Range and returned the "
                 "whole object (HTTP 200); refusing rather than slicing it");
            return ReadResult::failure();
        }

        /*
         * 412: the object is no longer the one this reader opened.
         *
         * An explicit, final error. No retry -- another attempt asks the same
         * question and gets the same answer. No re-HEAD, no adopting the new
         * ETag, no reopening: one ObjectReader reads exactly one incarnation
         * of the object or it fails. Silently continuing is the mixed-version
         * read this guard exists to prevent, and it is reachable: without the
         * guard, replacing the object mid-scan made a scan whose footer came
         * from A fail at row group 53 on a page header from B.
         */
        if (status == 412)
        {
            http_errors_++;
            /*
             * Reported through the base class, which is where the invariant
             * lives and where the count is kept, so this transport and the
             * local one say the same thing in the same words. The ETag appears
             * only in the DETAIL -- it is this transport's private token and
             * nothing above ObjectReader is told what kind of token it is.
             */
            fail_identity_moved("GET " + path() + " " + range +
                                ": If-Match " + identity_ + " -> HTTP 412");
            return ReadResult::failure();
        }

        if (status != 206)
        {
            http_errors_++;
            fail("GET " + path() + " " + range + ": HTTP " + std::to_string(status));
            return ReadResult::failure();
        }

        /*
         * Content-Range is where eof comes from. "bytes a-b/total": the object
         * ends at total, so this read reached the end only if b + 1 >= total.
         * Nothing about the SIZE of the body is allowed to imply eof.
         */
        std::string cr = header_value(headers, "Content-Range");
        int64_t first = -1, last = -1, total = -1;
        if (!cr.empty())
        {
            const char *p = cr.c_str();
            while (*p && (*p < '0' || *p > '9')) p++;
            if (*p)
            {
                first = strtoll(p, nullptr, 10);
                const char *dash = strchr(p, '-');
                const char *slash = strchr(p, '/');
                if (dash) last = strtoll(dash + 1, nullptr, 10);
                if (slash) total = strtoll(slash + 1, nullptr, 10);
            }
        }

        const int64_t got = static_cast<int64_t>(body.size());

        /*
         * THE CASE THIS WHOLE CONTRACT EXISTS FOR, and the order of these two
         * tests is the whole of it.
         *
         * A CUT TRANSFER IS CHECKED FIRST, against the response's own
         * Content-Length, and it is never EOF whatever Content-Range says.
         *
         * This was wrong when first written: it decided eof from Content-Range
         * before looking at whether the body was complete. A response whose
         * range happens to reach the end of the object -- which is EVERY
         * Parquet footer probe, since the footer is read backwards from the
         * end -- was therefore classified as a clean end of object when its
         * transfer had been cut in half. read_exact refused it, so no wrong
         * answer escaped through that path; but read_at_most is what Arrow's
         * sequential footer read uses, and it would have been handed
         * eof = true with half a footer. "The file ends here" is exactly the
         * lie this contract exists to prevent, and it survived in the remote
         * implementation for precisely the ranges that matter most.
         *
         * Found by pointing the footer probe at an endpoint that truncates
         * every response.
         */
        if (body_is_truncated(headers, body))
        {
            account(nbytes, got);
            http_errors_++;
            fail("GET " + path() + " " + range + ": transfer cut at " +
                 std::to_string(got) + " of the " +
                 header_value(headers, "Content-Length") +
                 " bytes the response promised (" + cr +
                 ") -- truncated response, not EOF");
            return ReadResult::failure();
        }

        /*
         * Short, but the response delivered everything it promised. So the
         * server is telling us the object ends here, and Content-Range is the
         * authority on that.
         */
        if (got < nbytes)
        {
            bool ends_here = (total >= 0 && last >= 0 && last + 1 >= total);
            account(nbytes, got);
            if (!ends_here)
            {
                http_errors_++;
                fail("GET " + path() + " " + range + ": body is " +
                     std::to_string(got) + " of " + std::to_string(nbytes) +
                     " bytes and the object does not end here (" + cr +
                     ") -- short range with no end of object");
                return ReadResult::failure();
            }
            memcpy(out, body.data(), static_cast<size_t>(got));
            return ReadResult::got(got, true);
        }

        if (got > nbytes)
        {
            http_errors_++;
            account(nbytes, 0);
            fail("GET " + path() + " " + range + ": server returned " +
                 std::to_string(got) + " bytes for a " + std::to_string(nbytes) +
                 "-byte range");
            return ReadResult::failure();
        }

        memcpy(out, body.data(), static_cast<size_t>(nbytes));
        account(nbytes, nbytes);
        /* A full read can still be the last one: say so when the object ends. */
        bool ends_here = (total >= 0 && last >= 0 && last + 1 >= total);
        return ReadResult::got(nbytes, ends_here);
    }

    void close() override
    {
        /*
         * Releases the persistent socket if one is held. Idempotent, as the
         * contract requires, and taken under the same lock as every other use
         * of conn_fd_: close() runs on the backend thread while an Arrow worker
         * may still be inside an exchange, so an unlocked close here would be a
         * use-after-close on a live descriptor.
         */
        {
            std::lock_guard<std::mutex> lk(conn_mutex_);
            drop_connection();
        }
        closed_ = true;
    }

private:
    std::string path() const { return "/" + bucket_ + "/" + key_; }

    /*
     * Short against the response's OWN Content-Length -- the transfer was cut
     * mid-flight. Deliberately not "short against what the caller asked for":
     * a legitimately short range at the end of the object has a matching
     * Content-Length and must not be retried.
     */
    static bool body_is_truncated(const std::string &headers, const std::string &body)
    {
        std::string cl = header_value(headers, "Content-Length");
        if (cl.empty())
            return false;
        int64_t promised = strtoll(cl.c_str(), nullptr, 10);
        return promised >= 0 && static_cast<int64_t>(body.size()) < promised;
    }

    /*
     * One HTTP exchange on a connection the caller owns.
     *
     * This function never closes `fd` and never connects: whether the socket
     * survives is reported, not decided here, because the decision belongs with
     * the code that holds the connection. That split is the whole of keep-alive
     * support -- the response parsing below is byte-for-byte the one-socket
     * version, so no failure classification moved with it.
     */
    struct Exchange
    {
        int  status;        /* HTTP status, or -1 with last_error() set       */
        bool got_bytes;     /* any response byte arrived -- see request()     */
        bool reusable;      /* the socket is positioned at a clean boundary   */
    };

    static Exchange ex_fail(bool got_bytes)
    { return Exchange{-1, got_bytes, false}; }

    Exchange do_exchange(int fd, const char *method, const std::string &req,
                         std::string *headers_out, std::string *body_out,
                         int64_t expect_body)
    {
        if (!send_all(fd, req))
            return ex_fail(false);

        std::string buf;
        if (expect_body > 0)
            buf.reserve(static_cast<size_t>(expect_body) + 1024);

        /* Headers first: read until the blank line. */
        size_t sep = std::string::npos;
        while (true)
        {
            char chunk[8192];
            ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    fail("timed out after " + std::to_string(io_timeout_ms_) +
                         " ms waiting for response headers from " + ep_.host);
                else
                    fail(std::string("recv: ") + std::strerror(errno));
                return ex_fail(!buf.empty());
            }
            if (n == 0)
            {
                /*
                 * The peer closed before a complete response. On a REUSED
                 * socket with nothing received this is the ordinary race with
                 * the server's idle timeout, and request() replays it once; on
                 * a fresh socket, or after bytes arrived, it is a real failure.
                 * This function does not know which, so it only reports what
                 * happened.
                 */
                fail("connection closed before the response headers were complete");
                return ex_fail(!buf.empty());
            }
            buf.append(chunk, static_cast<size_t>(n));
            sep = buf.find("\r\n\r\n");
            if (sep != std::string::npos)
                break;
            if (buf.size() > (1u << 20))
            {
                fail("response headers exceed 1 MB");
                return ex_fail(true);
            }
        }

        *headers_out = buf.substr(0, sep + 2);   /* keep a trailing CRLF so the
                                                  * header lookup can anchor */
        std::string body = buf.substr(sep + 4);

        int status = 0;
        if (headers_out->compare(0, 5, "HTTP/") == 0)
        {
            size_t sp = headers_out->find(' ');
            if (sp != std::string::npos)
                status = atoi(headers_out->c_str() + sp + 1);
        }
        if (status == 0)
        {
            fail("malformed status line");
            return ex_fail(true);
        }

        /*
         * The server has the last word on whether the socket survives. Asking
         * to keep it is a request; "Connection: close" in the response is an
         * instruction, and reading a second response off a socket the server
         * is about to close would be a self-inflicted failure.
         */
        const bool peer_closes = header_is(*headers_out, "Connection", "close");

        if (strcmp(method, "HEAD") == 0)
        {
            /* No body by definition, so the socket is already at a boundary. */
            *body_out = std::string();
            return Exchange{status, true, reuse_ && !peer_closes};
        }

        /*
         * Read exactly Content-Length bytes. A body that ends early is NOT
         * treated as "that is all there was": it is reported, and the caller
         * decides, which for read_exact means an error.
         */
        std::string cl = header_value(*headers_out, "Content-Length");
        int64_t want = cl.empty() ? -1 : strtoll(cl.c_str(), nullptr, 10);
        if (want < 0)
        {
            /* No Content-Length: would need chunked decoding, which phase 1
             * does not implement. Refused rather than guessed. */
            fail("response has no Content-Length (chunked transfer-encoding is "
                 "not implemented)");
            return ex_fail(true);
        }

        while (static_cast<int64_t>(body.size()) < want)
        {
            char chunk[65536];
            ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    fail("timed out after " + std::to_string(io_timeout_ms_) +
                         " ms with " + std::to_string(body.size()) + " of " +
                         std::to_string(want) + " body bytes from " + ep_.host);
                else
                    fail(std::string("recv body: ") + std::strerror(errno));
                return ex_fail(true);
            }
            if (n == 0)
                break;              /* short body; the caller sees it as such */
            body.append(chunk, static_cast<size_t>(n));
        }

        bytes_transferred_ += static_cast<int64_t>(body.size());

        /*
         * A socket may only be reused when the stream is at a known boundary,
         * which means exactly Content-Length bytes consumed and no more. Short
         * is the cut-transfer case the caller turns into an error; longer means
         * the server sent something this code does not understand. Either way
         * the next response could not be found reliably, so the connection is
         * not reusable -- that is a transport decision and it does not change
         * what the caller is told about the data.
         */
        const bool at_boundary = (static_cast<int64_t>(body.size()) == want);

        *body_out = std::move(body);
        return Exchange{status, true, reuse_ && !peer_closes && at_boundary};
    }

    /*
     * One logical HTTP request, on a persistent connection when there is one.
     * `expect_body` is how many bytes the caller wants, used only to size the
     * reserve. Returns the HTTP status, or -1 with last_error() set.
     *
     * Serialised on conn_mutex_ for the whole exchange. Measured first: reads
     * never overlap in any scan shape (max_in_flight = 1 for footer, projected,
     * pruned and full scans), because Arrow dispatches column-chunk reads to its
     * pool but waits for each. So the lock is uncontended in practice; it is
     * held anyway, because correctness here must not rest on Arrow's scheduling,
     * and because the fd and its HTTP state are shared mutable state crossing
     * threads.
     *
     * The interrupt hook is NOT called here -- before_read() runs in the
     * caller's retry loop, outside this lock. That ordering is deliberate: the
     * hook may longjmp out of a PostgreSQL ERROR, and a longjmp through a held
     * std::mutex would leave the reader permanently locked.
     */
    int request(const char *method, const std::string &range,
                std::string *headers_out, std::string *body_out,
                int64_t expect_body)
    {
        /*
         * How many requests are inside this function at once, counted BEFORE
         * the lock. Counting it inside would measure the lock rather than the
         * callers, and would read 1 by construction.
         */
        int64_t cur = in_flight_.fetch_add(1, std::memory_order_relaxed) + 1;
        int64_t seen = max_in_flight_.load(std::memory_order_relaxed);
        while (cur > seen &&
               !max_in_flight_.compare_exchange_weak(seen, cur,
                                                     std::memory_order_relaxed))
            ;
        struct InFlightGuard
        {
            std::atomic<int64_t> *c;
            ~InFlightGuard() { c->fetch_sub(1, std::memory_order_relaxed); }
        } ifg{&in_flight_};

        std::lock_guard<std::mutex> lk(conn_mutex_);

        /*
         * At most two passes: the second exists only to replay a request that
         * died on a connection the SERVER had already closed while it sat idle.
         *
         * That replay is bounded by construction and deliberately narrow:
         *   - only on a socket that was reused, never on a fresh one;
         *   - only when NO response byte arrived, so nothing can be replayed
         *     over a partially delivered answer;
         *   - GET and HEAD only, which is all this reader issues, and both are
         *     idempotent, so the replay cannot have a side effect.
         * It is counted as a RECONNECT and not as a retry: it consumes none of
         * the caller's bounded retry budget, because no server answer was
         * received to retry. A failure that happens after bytes arrive is left
         * to the caller's retry loop exactly as before.
         */
        for (int pass = 0; pass < 2; pass++)
        {
            const bool reused = (conn_fd_ >= 0);

            if (!reused)
            {
                conn_fd_ = connect_endpoint();
                if (conn_fd_ < 0)
                {
                    conn_fd_ = -1;
                    return -1;
                }
            }

            http_attempts_++;

            /*
             * Signed afresh each pass. The replay therefore carries a new
             * x-amz-date and signature, and -- the point that matters -- the
             * SAME If-Match, because build_request() rebuilds it from the
             * identity pinned at open. A reconnect cannot re-HEAD, cannot
             * observe a new ETag and cannot widen the incarnation this reader
             * reads.
             */
            std::string req = build_request(method, range);

            headers_out->clear();
            body_out->clear();
            Exchange ex = do_exchange(conn_fd_, method, req,
                                      headers_out, body_out, expect_body);

            if (!ex.reusable)
                drop_connection();

            if (ex.status < 0 && reused && !ex.got_bytes && pass == 0)
            {
                reconnects_++;
                clear_error();
                continue;               /* conn_fd_ is -1: the next pass dials */
            }
            return ex.status;
        }
        return -1;                      /* not reachable: pass 1 always returns */
    }

    void drop_connection()
    {
        if (conn_fd_ >= 0)
            ::close(conn_fd_);
        conn_fd_ = -1;
    }

    int connect_endpoint()
    {
        struct addrinfo hints;
        struct addrinfo *res = nullptr;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;          /* IPv4 only in phase 1 */
        hints.ai_socktype = SOCK_STREAM;

        char port[16];
        snprintf(port, sizeof(port), "%d", ep_.port);

        int rc = ::getaddrinfo(ep_.host.c_str(), port, &hints, &res);
        if (rc != 0 || res == nullptr)
        {
            fail("getaddrinfo " + ep_.host + ": " + gai_strerror(rc));
            return -1;
        }

        int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0)
        {
            fail(std::string("socket: ") + std::strerror(errno));
            ::freeaddrinfo(res);
            return -1;
        }
        /*
         * Non-blocking connect plus poll. The OS default can be minutes, and
         * this thread cannot be interrupted -- see set_deadlines().
         */
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rcc = ::connect(fd, res->ai_addr, res->ai_addrlen);
        if (rcc != 0 && errno == EINPROGRESS)
        {
            struct pollfd pfd;

            pfd.fd = fd;
            pfd.events = POLLOUT;
            int pr = ::poll(&pfd, 1, connect_timeout_ms_);
            if (pr == 0)
            {
                fail("connect " + ep_.host + ":" + port + ": timed out after " +
                     std::to_string(connect_timeout_ms_) + " ms");
                ::close(fd);
                ::freeaddrinfo(res);
                return -1;
            }
            if (pr < 0)
            {
                fail(std::string("poll on connect: ") + std::strerror(errno));
                ::close(fd);
                ::freeaddrinfo(res);
                return -1;
            }
            int       soerr = 0;
            socklen_t slen = sizeof(soerr);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
            if (soerr != 0)
            {
                fail("connect " + ep_.host + ":" + port + ": " + std::strerror(soerr));
                ::close(fd);
                ::freeaddrinfo(res);
                return -1;
            }
        }
        else if (rcc != 0)
        {
            fail("connect " + ep_.host + ":" + port + ": " + std::strerror(errno));
            ::close(fd);
            ::freeaddrinfo(res);
            return -1;
        }
        ::fcntl(fd, F_SETFL, flags);
        ::freeaddrinfo(res);

        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        set_deadlines(fd);
        connections_.fetch_add(1, std::memory_order_relaxed);
        return fd;
    }

    /*
     * A DEADLINE, not an interrupt check.
     *
     * Every data read below Arrow's pre_buffer runs on an Arrow worker thread,
     * and the interrupt hook deliberately does not fire there -- see
     * docs/OBJECT_READER_V0.md section 7. So a blocking recv() on a worker is
     * unreachable by statement_timeout, pg_cancel_backend and
     * pg_terminate_backend alike.
     *
     * Measured before this existed: an endpoint that accepts a connection,
     * promises a body and then sends nothing left statement_timeout = 3 s
     * still blocked at 90 seconds, with the backend alive afterwards. That is
     * the v0 defect class reached by a different route -- not an interrupt
     * check on the wrong thread, but no interrupt check reachable at all.
     *
     * The transport therefore carries its own bound, because at this depth it
     * is the only thing that can: it is the only code holding the syscall.
     */
    void set_deadlines(int fd) const
    {
        struct timeval tv;

        tv.tv_sec = io_timeout_ms_ / 1000;
        tv.tv_usec = (io_timeout_ms_ % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    static bool send_all_fd(int fd, const char *p, size_t n)
    {
        while (n > 0)
        {
            ssize_t w = ::send(fd, p, n, 0);
            if (w < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            p += w;
            n -= static_cast<size_t>(w);
        }
        return true;
    }

    bool send_all(int fd, const std::string &s)
    {
        if (!send_all_fd(fd, s.data(), s.size()))
        {
            fail(std::string("send: ") + std::strerror(errno));
            return false;
        }
        return true;
    }

    std::string build_request(const char *method, const std::string &range)
    {
        char amz_date[32], datestamp[16];
        time_t now = time(nullptr);
        struct tm tmv;
        gmtime_r(&now, &tmv);
        strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", &tmv);
        strftime(datestamp, sizeof(datestamp), "%Y%m%d", &tmv);

        std::string host = ep_.host + ":" + std::to_string(ep_.port);

        /*
         * Every RANGE read carries the identity captured at open. The HEAD
         * that establishes it sends none -- it is what defines identity, so
         * conditioning it on itself would be circular.
         */
        const std::string cond = range.empty() ? std::string() : identity_;

        return build_signed_request(method, path(), host, range, amz_date,
                                    datestamp, region_, access_, secret_, cond,
                                    reuse_);
    }


    Endpoint    ep_;
    std::string bucket_, key_, access_, secret_, region_;
    int64_t     size_ = -1;
    bool        closed_ = false;
    int64_t     head_calls_ = 0;        /* logical size() calls that did I/O  */
    int64_t     get_calls_ = 0;         /* logical range reads                */
    int64_t     http_errors_ = 0;
    int64_t     bytes_transferred_ = 0; /* bytes off the wire, retries included */
    int64_t     http_attempts_ = 0;     /* physical exchanges, retries included */
    std::atomic<int64_t> connections_{0};   /* actual connect() calls          */
    std::atomic<int64_t> in_flight_{0};
    std::atomic<int64_t> max_in_flight_{0};
    int64_t     reconnects_ = 0;        /* replays after a server-closed idle  */
    int         conn_fd_ = -1;          /* the persistent socket, or -1        */
    std::mutex  conn_mutex_;            /* guards conn_fd_ and its HTTP state  */
    bool        reuse_ = true;
    std::string identity_;              /* opaque ETag captured with size_     */
    bool        require_identity_ = true;
    int64_t     retries_done_ = 0;
    int         max_attempts_ = 3;
    int         io_timeout_ms_ = 15000;
    int         connect_timeout_ms_ = 5000;
};

}   /* namespace */

/* ---------------------------------------------------------------- exports -- */

std::string
sigv4_signature(const std::string &canonical_request, const std::string &amz_date,
                const std::string &datestamp, const std::string &region,
                const std::string &service, const std::string &secret_key)
{
    std::string scope = datestamp + "/" + region + "/" + service + "/aws4_request";
    std::string to_sign = "AWS4-HMAC-SHA256\n" + amz_date + "\n" + scope + "\n" +
                          sha256_hex(canonical_request);

    std::string k = hmac_raw("AWS4" + secret_key, datestamp);
    k = hmac_raw(k, region);
    k = hmac_raw(k, service);
    k = hmac_raw(k, "aws4_request");

    std::string raw = hmac_raw(k, to_sign);
    return to_hex(reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
}

std::string
s3_test_authorization(const char *method, const char *uri, const char *host,
                      const char *range, const char *amz_date,
                      const char *datestamp, const char *region,
                      const char *access, const char *secret)
{
    std::string req = build_signed_request(method, uri, host,
                                           range ? range : "",
                                           amz_date, datestamp, region,
                                           access, secret, std::string(),
                                           /* keep_alive */ true);
    /* Return just the Authorization line's value, which is what the oracle
     * produces too. Connection is not a signed header, so this value cannot
     * affect what the oracle compares; it matches what the reader sends. */
    const std::string tag = "Authorization: ";
    size_t at = req.find(tag);
    if (at == std::string::npos)
        return std::string();
    size_t end = req.find("\r\n", at);
    return req.substr(at + tag.size(), end - at - tag.size());
}

std::string
s3_test_canonical_request(const char *method, const char *uri, const char *host,
                          const char *range, const char *amz_date)
{
    return s3_canonical_request(method, uri, host, range ? range : "", amz_date,
                                std::string());
}

bool
is_s3_uri(const char *uri)
{
    return uri != nullptr && strncmp(uri, "s3://", 5) == 0;
}

ObjectReader *
open_s3_reader(const char *uri, std::string *error)
{
    std::string err;

    if (!is_s3_uri(uri))
    {
        if (error) *error = "not an s3:// URI";
        return nullptr;
    }

    std::string rest(uri + 5);
    size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size())
    {
        if (error) *error = std::string("malformed S3 URI \"") + uri +
                            "\": expected s3://bucket/key";
        return nullptr;
    }
    std::string bucket = rest.substr(0, slash);
    std::string key = rest.substr(slash + 1);

    Endpoint ep;
    if (!parse_endpoint(getenv("XPB_S3_ENDPOINT"), &ep, &err))
    {
        if (error) *error = err;
        return nullptr;
    }

    const char *ak = getenv("XPB_S3_ACCESS_KEY");
    const char *sk = getenv("XPB_S3_SECRET_KEY");
    const char *rg = getenv("XPB_S3_REGION");

    if (ak == nullptr || sk == nullptr)
    {
        if (error) *error = "XPB_S3_ACCESS_KEY and XPB_S3_SECRET_KEY must be set "
                            "in the server's environment";
        return nullptr;
    }

    try
    {
        S3Reader *r = new S3Reader(ep, bucket, key, ak, sk, rg ? rg : "us-east-1");
        /*
         * Bounded, and bounded again here: an operator who sets this to 1000
         * turns a dead endpoint into a hung backend. 1 means no retry at all,
         * which is what the tests use to see the raw failure.
         */
        const char *ma = getenv("XPB_S3_MAX_ATTEMPTS");
        if (ma != nullptr)
        {
            int n = atoi(ma);
            if (n < 1) n = 1;
            if (n > 5) n = 5;
            r->set_max_attempts(n);
        }
        /*
         * Bounded at both ends. Too small and a healthy slow object fails; too
         * large and an unreachable endpoint is indistinguishable from a hang,
         * which is what these exist to prevent.
         */
        const char *io = getenv("XPB_S3_IO_TIMEOUT_MS");
        if (io != nullptr)
        {
            int n = atoi(io);
            if (n < 100) n = 100;
            if (n > 300000) n = 300000;
            r->set_io_timeout_ms(n);
        }
        /*
         * Off exists so the negative control is reproducible -- a test has to
         * be able to show what the guard prevents. It is not a tuning knob.
         */
        const char *ig = getenv("XPB_S3_IDENTITY_GUARD");
        if (ig != nullptr && (strcmp(ig, "off") == 0 || strcmp(ig, "0") == 0))
            r->set_require_identity(false);

        /*
         * The baseline control. Off means one connection per request, which is
         * what every earlier phase measured; it exists so the comparison is one
         * binary with one flag, not two builds.
         */
        const char *cr = getenv("XPB_S3_CONNECTION_REUSE");
        if (cr != nullptr && (strcmp(cr, "off") == 0 || strcmp(cr, "0") == 0))
            r->set_reuse(false);

        const char *ct = getenv("XPB_S3_CONNECT_TIMEOUT_MS");
        if (ct != nullptr)
        {
            int n = atoi(ct);
            if (n < 100) n = 100;
            if (n > 60000) n = 60000;
            r->set_connect_timeout_ms(n);
        }
        return r;
    }
    catch (...)
    {
        throw;
    }
}

int64_t s3_head_calls(const ObjectReader *r)
{ const S3Reader *s = dynamic_cast<const S3Reader *>(r); return s ? s->head_calls() : -1; }
int64_t s3_get_calls(const ObjectReader *r)
{ const S3Reader *s = dynamic_cast<const S3Reader *>(r); return s ? s->get_calls() : -1; }
int64_t s3_http_errors(const ObjectReader *r)
{ const S3Reader *s = dynamic_cast<const S3Reader *>(r); return s ? s->http_errors() : -1; }
int64_t s3_bytes_transferred(const ObjectReader *r)
{ const S3Reader *s = dynamic_cast<const S3Reader *>(r); return s ? s->bytes_transferred() : -1; }
int64_t s3_http_attempts(const ObjectReader *r)
{ const S3Reader *s = dynamic_cast<const S3Reader *>(r); return s ? s->http_attempts() : -1; }
int64_t s3_connections(const ObjectReader *r)
{ const S3Reader *s = dynamic_cast<const S3Reader *>(r); return s ? s->connections() : -1; }
int64_t s3_max_in_flight(const ObjectReader *r)
{ const S3Reader *s = dynamic_cast<const S3Reader *>(r); return s ? s->max_in_flight() : -1; }
int64_t s3_retries(const ObjectReader *r)
{ const S3Reader *s = dynamic_cast<const S3Reader *>(r); return s ? s->retries() : -1; }
int64_t s3_reconnects(const ObjectReader *r)
{ const S3Reader *s = dynamic_cast<const S3Reader *>(r); return s ? s->reconnects() : -1; }
int64_t s3_identity_conflicts(const ObjectReader *r)
{
    /*
     * Now the base class's transport-general count, kept here only so the
     * existing SQL column keeps working. A -1 still means "not an S3 reader".
     */
    const S3Reader *s = dynamic_cast<const S3Reader *>(r);
    return s ? s->identity_conflicts() : -1;
}
const char *s3_identity(const ObjectReader *r)
{ const S3Reader *s = dynamic_cast<const S3Reader *>(r); return s ? s->identity().c_str() : ""; }

}   /* namespace xpb */
