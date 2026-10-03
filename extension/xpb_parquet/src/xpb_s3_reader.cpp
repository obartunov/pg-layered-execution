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
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
                     const std::string &amz_date)
{
    std::string canon_headers = "host:" + host + "\n";
    std::string signed_headers = "host;";

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
                                 const std::string &amz_date);

std::string
build_signed_request(const char *method, const std::string &uri,
                     const std::string &host, const std::string &range,
                     const std::string &amz_date, const std::string &datestamp,
                     const std::string &region, const std::string &access,
                     const std::string &secret)
{
    std::string signed_headers = "host;";
    if (!range.empty())
        signed_headers += "range;";
    signed_headers += "x-amz-content-sha256;x-amz-date";

    std::string canonical_request =
        s3_canonical_request(method, uri, host, range, amz_date);

    std::string sig = sigv4_signature(canonical_request, amz_date, datestamp,
                      region, "s3", secret);
    std::string scope = std::string(datestamp) + "/" + region + "/s3/aws4_request";

    std::string req;
    req += std::string(method) + " " + uri + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    if (!range.empty())
        req += "Range: " + range + "\r\n";
    req += std::string("x-amz-content-sha256: ") + kEmptyPayloadSha256 + "\r\n";
    req += std::string("x-amz-date: ") + amz_date + "\r\n";
    req += "Authorization: AWS4-HMAC-SHA256 Credential=" + access + "/" + scope +
           ", SignedHeaders=" + signed_headers + ", Signature=" + sig + "\r\n";
    req += "Connection: close\r\n";
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

    ~S3Reader() override { close(); }

    int64_t head_calls()        const { return head_calls_; }
    int64_t get_calls()         const { return get_calls_; }
    int64_t http_errors()       const { return http_errors_; }
    int64_t bytes_transferred() const { return bytes_transferred_; }

    int64_t size() override
    {
        if (closed_)
            return fail("size() on a closed reader");
        if (size_ >= 0)
            return size_;            /* fixed for the reader's life, as the
                                      * ObjectReader contract requires */

        before_read();
        head_calls_++;

        std::string headers, body;
        int status = request("HEAD", "", &headers, &body, 0);
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
        size_ = strtoll(cl.c_str(), nullptr, 10);
        if (size_ < 0)
            return fail("HEAD " + path() + ": bad Content-Length");
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

        before_read();
        get_calls_++;

        char range[128];
        snprintf(range, sizeof(range), "bytes=%lld-%lld",
                 (long long) offset, (long long) (offset + nbytes - 1));

        std::string headers, body;
        int status = request("GET", range, &headers, &body, nbytes);
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
         * THE CASE THIS WHOLE CONTRACT EXISTS FOR.
         *
         * Fewer bytes than asked for, and the object does not end here. For a
         * local file that cannot happen; over a transport it is a truncated
         * response, and v0's "short means EOF" would have turned it into a
         * silently short column chunk. It is an error, and eof stays false.
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
                     ") -- truncated response, not EOF");
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
        /* No persistent connection to release in phase 1; each request closes
         * its own socket. Idempotent, as the contract requires. */
        closed_ = true;
    }

private:
    std::string path() const { return "/" + bucket_ + "/" + key_; }

    /*
     * One request, one socket, closed on return. `expect_body` is how many
     * bytes the caller wants, used only to size the reserve.
     *
     * Returns the HTTP status, or -1 with last_error() set when the exchange
     * itself failed.
     */
    int request(const char *method, const std::string &range,
                std::string *headers_out, std::string *body_out,
                int64_t expect_body)
    {
        int fd = connect_endpoint();
        if (fd < 0)
            return -1;

        std::string req = build_request(method, range);
        if (!send_all(fd, req))
        {
            ::close(fd);
            return -1;
        }

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
                fail(std::string("recv: ") + std::strerror(errno));
                ::close(fd);
                return -1;
            }
            if (n == 0)
            {
                fail("connection closed before the response headers were complete");
                ::close(fd);
                return -1;
            }
            buf.append(chunk, static_cast<size_t>(n));
            sep = buf.find("\r\n\r\n");
            if (sep != std::string::npos)
                break;
            if (buf.size() > (1u << 20))
            {
                fail("response headers exceed 1 MB");
                ::close(fd);
                return -1;
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
            ::close(fd);
            return -1;
        }

        if (strcmp(method, "HEAD") == 0)
        {
            ::close(fd);
            *body_out = std::string();
            return status;
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
            ::close(fd);
            return -1;
        }

        while (static_cast<int64_t>(body.size()) < want)
        {
            char chunk[65536];
            ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                fail(std::string("recv body: ") + std::strerror(errno));
                ::close(fd);
                return -1;
            }
            if (n == 0)
                break;              /* short body; the caller sees it as such */
            body.append(chunk, static_cast<size_t>(n));
        }

        ::close(fd);
        bytes_transferred_ += static_cast<int64_t>(body.size());
        *body_out = std::move(body);
        return status;
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
        if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0)
        {
            fail("connect " + ep_.host + ":" + port + ": " + std::strerror(errno));
            ::close(fd);
            ::freeaddrinfo(res);
            return -1;
        }
        ::freeaddrinfo(res);

        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        return fd;
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
        return build_signed_request(method, path(), host, range, amz_date,
                                    datestamp, region_, access_, secret_);
    }


    Endpoint    ep_;
    std::string bucket_, key_, access_, secret_, region_;
    int64_t     size_ = -1;
    bool        closed_ = false;
    int64_t     head_calls_ = 0;
    int64_t     get_calls_ = 0;
    int64_t     http_errors_ = 0;
    int64_t     bytes_transferred_ = 0;
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
                                           access, secret);
    /* Return just the Authorization line's value, which is what the oracle
     * produces too. */
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
    return s3_canonical_request(method, uri, host, range ? range : "", amz_date);
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
        return new S3Reader(ep, bucket, key, ak, sk, rg ? rg : "us-east-1");
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

}   /* namespace xpb */
