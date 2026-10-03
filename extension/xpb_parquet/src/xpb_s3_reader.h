/*
 * xpb_s3_reader.h -- an ObjectReader over HTTP ranged GET.
 *
 * Phase 1 of the S3 series: prove that a remote reader sits under the existing
 * ObjectReader contract without anything above it changing. Nothing here is
 * reachable from the Parquet reader except through ObjectReader, and this
 * header is not included by the shim's Arrow-facing code.
 *
 * ---------------------------------------------------------------- WHAT IT IS
 *
 * A minimal, blocking HTTP/1.1 client plus AWS SigV4, and nothing else:
 *
 *   - no SDK, therefore no SDK thread pool, therefore no question about which
 *     thread a callback runs on. Every read happens on the caller's thread,
 *     synchronously. That is a deliberate answer to the defect v0 shipped --
 *     see docs/OBJECT_READER_V0.md section 7 -- rather than a limitation to be
 *     fixed later;
 *   - no async, no cache, no prefetch. One request per read_at_most(), and a
 *     connection kept between requests (HTTP/1.1 keep-alive) -- a single
 *     socket, reused, never a pool and never pipelined. See CONNECTIONS;
 *   - no TLS. http:// only. This cannot talk to real AWS S3 and is not
 *     intended to yet; see LIMITS.
 *
 * ----------------------------------------------------------- CONNECTIONS
 *
 * One socket, held across requests and serialised by a mutex. Not a pool, not
 * pipelined, not HTTP/2: a reader issues one request at a time, measured rather
 * than assumed -- the most requests ever in flight at once is 1 in every scan
 * shape, because Arrow dispatches column-chunk reads to its thread pool and
 * then waits for each. The mutex is therefore uncontended; it is held anyway,
 * because the socket and its HTTP state are shared mutable state that an Arrow
 * worker and the backend thread both touch, and because correctness here must
 * not depend on Arrow's scheduling staying what it is today.
 *
 * The socket is dropped, not reused, whenever it is not known to be at a clean
 * message boundary: the response said Connection: close, the body did not
 * match Content-Length, or the exchange failed at all.
 *
 * A reused socket has exactly one failure the fresh one did not: the server
 * closed it while it sat idle, and the client finds out by sending into a dead
 * connection. One request is replayed on a new socket for that, and only when
 * NO response byte had arrived, so a replay can never sit on top of a partly
 * delivered answer. It is counted as a RECONNECT, not a retry, and spends no
 * retry budget -- there was no server answer to retry. GET and HEAD are all
 * this reader issues and both are idempotent, so the replay has no side effect.
 *
 * What reuse does NOT touch: the logical request, the bytes, the accounting,
 * the identity pinned at open (a reconnect re-signs but never re-HEADs, so
 * If-Match still names the incarnation opened), the deadlines, or any of the
 * failure classifications below.
 *
 * ------------------------------------------------------------------- EOF
 *
 * This is the reader the v1 contract was written for, so the mapping is the
 * whole point:
 *
 *   206 + Content-Range: bytes a-b/total
 *       bytes delivered = b - a + 1, and eof is TRUE only when b + 1 >= total.
 *       The object's own stated length decides it.
 *
 *   body shorter than Content-Length
 *       A TRUNCATED TRANSFER. ok = false, and eof stays false. This is exactly
 *       the case the v0 contract would have reported as a clean end of object,
 *       and therefore the case that would have produced a silently short
 *       column chunk.
 *
 *   416 Range Not Satisfiable
 *       The range is entirely past the end. bytes = 0, eof = TRUE, ok = true:
 *       the server has stated the object ends before the request, which is
 *       authoritative in a way a short body never is.
 *
 *   200 to a ranged request
 *       The server ignored Range. An ERROR, not a silent whole-object read: a
 *       180 MB body in answer to a 218-byte request is a configuration fault,
 *       and taking a slice of it would hide that.
 *
 *   4xx / 5xx
 *       Error. Phase 1 does not retry anything; retry policy is phase 2 and
 *       belongs inside this class, under read_exact, never in the adapter.
 *
 * ------------------------------------------------------------------- LIMITS
 *
 * Stated here rather than discovered later. None of these is a correctness
 * boundary for what phase 1 claims, and all of them block real S3 use:
 *
 *   - no TLS, so credentials and data cross the wire in clear. Only usable
 *     against a local endpoint;
 *   - SigV4 is implemented but UNVERIFIED against a real signer in this
 *     environment, because the test endpoint accepts unsigned requests. It is
 *     checked against botocore's SigV4Auth on the same canonical request
 *     instead, which proves the algorithm and not the integration;
 *   - no redirects, no 100-continue, no chunked transfer-encoding on
 *     responses, no IPv6 literal hosts, no virtual-host-style addressing
 *     (path style only);
 *   - credentials come from the environment of the BACKEND process, which
 *     means the postmaster's environment. Configuration has no proper home
 *     yet, and this is not it;
 *   - SigV4 is now verified against a server that enforces it (RustFS), but
 *     still not against AWS itself;
 *   - keep-alive is not negotiated beyond Connection: close. A server that
 *     keeps the socket but stops answering is caught by the I/O deadline, not
 *     by any probe of connection health, so one dead-but-open connection costs
 *     one deadline before the reader gives up on it.
 */
#ifndef XPB_S3_READER_H
#define XPB_S3_READER_H

#include <string>

namespace xpb {

class ObjectReader;

/*
 * Open s3://bucket/key.
 *
 * Endpoint and credentials are taken from the environment:
 *
 *   XPB_S3_ENDPOINT        http://host:port   (required; no default, because a
 *                                              default pointing at AWS over
 *                                              plain HTTP would be worse than
 *                                              a refusal)
 *   XPB_S3_ACCESS_KEY      access key id
 *   XPB_S3_SECRET_KEY      secret access key
 *   XPB_S3_REGION          region, default us-east-1
 *   XPB_S3_IDENTITY_GUARD  "off" disables the one-version-per-reader guard.
 *                          It exists so a test can demonstrate what the guard
 *                          prevents; it is not a tuning knob and must not be
 *                          off in use.
 *   XPB_S3_MAX_ATTEMPTS    total attempts per request, clamped to 1..5,
 *                          default 3. 1 disables retry, which is what the
 *                          failure tests use to see the raw error.
 *
 * Deliberately NOT the AWS_* names: these are read from the postmaster's
 * environment, and silently picking up a developer's real AWS credentials to
 * send over plain HTTP to whatever XPB_S3_ENDPOINT says is not a thing this
 * should do by accident.
 *
 * Returns NULL with *error set. The caller owns the result.
 */
ObjectReader *open_s3_reader(const char *uri, std::string *error);

/* True for a URI this reader handles, so the one place that chooses a byte
 * source can ask rather than parse. */
bool is_s3_uri(const char *uri);

/*
 * SigV4 over an already-canonical request, exposed only so it can be checked
 * against an independent signer. Returns the lowercase hex signature.
 *
 * It is here because a signing implementation that is never compared with
 * another one is a guess.
 */
std::string sigv4_signature(const std::string &canonical_request,
                            const std::string &amz_date,       /* 20130524T000000Z */
                            const std::string &datestamp,      /* 20130524 */
                            const std::string &region,
                            const std::string &service,
                            const std::string &secret_key);

/*
 * The full Authorization header this reader would send, with the timestamp
 * injected so the result is reproducible.
 *
 * Exists to be compared with an independent signer: it covers canonicalisation
 * as well as the HMAC chain, because a correct signing algorithm over a wrong
 * canonical request still fails against AWS. The local test endpoint accepts
 * unsigned requests, so without this the signing code would ship unchecked.
 */
std::string s3_test_canonical_request(const char *method, const char *uri,
                                      const char *host, const char *range,
                                      const char *amz_date);

std::string s3_test_authorization(const char *method, const char *uri,
                                  const char *host, const char *range,
                                  const char *amz_date, const char *datestamp,
                                  const char *region, const char *access,
                                  const char *secret);

/* Counters a test needs that are specific to a remote transport. -1 when the
 * reader is not an S3 one. */
int64_t s3_head_calls(const ObjectReader *r);
int64_t s3_get_calls(const ObjectReader *r);
int64_t s3_http_errors(const ObjectReader *r);
int64_t s3_bytes_transferred(const ObjectReader *r);
/* Physical HTTP exchanges and how many of them were retries. The pair is how
 * "a retry changes the physical traffic and not the logical request" is shown
 * for a remote source. */
int64_t s3_http_attempts(const ObjectReader *r);
int64_t s3_retries(const ObjectReader *r);
/* Actual connect() calls. With one connection per request this equals
 * s3_http_attempts; the point of keep-alive is to make it much smaller. */
int64_t s3_connections(const ObjectReader *r);
/*
 * Requests replayed because a reused connection turned out to be already closed
 * by the peer. Separate from s3_retries: a reconnect happens before the server
 * has seen anything, and consumes no retry budget.
 */
int64_t s3_reconnects(const ObjectReader *r);
/* The most reads ever in flight at once, which decides whether one persistent
 * connection can be serialised without losing parallelism there is. */
int64_t s3_max_in_flight(const ObjectReader *r);

/*
 * Range reads refused because the object changed since the reader opened it
 * (HTTP 412). Nonzero means a mixed-version read was prevented, not that one
 * happened.
 */
int64_t s3_identity_conflicts(const ObjectReader *r);

/* The opaque identity token captured at open. Empty when unguarded. */
const char *s3_identity(const ObjectReader *r);

}   /* namespace xpb */

#endif  /* XPB_S3_READER_H */
