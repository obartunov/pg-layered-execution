# ObjectReader v2 — transport-general object identity

Base commit `22fa8e8`. Branch `objectreader-v2`.

## Goal

Make "one reader sees one incarnation of the object" a property of the
`ObjectReader` contract rather than of one transport's implementation, without
letting filesystem or S3 detail reach the layers above.

## Invariant

```
ObjectReader instance
    -> captures an opaque identity at open
    -> every later read must still belong to that same identity
    -> identity moved => ERROR
```

No reopen, no re-pin, no adopting a new incarnation. Recovering from a changed
object is a decision for a layer that knows what the query meant, and no such
layer has asked for one.

## Why transport-general

Because the wrong-answer class is not S3's. v0.5 proved both halves:

* over S3, replacing the object two seconds into a full scan served row groups
  0–52 from one incarnation and row group 53 from another, failing with
  `Deserializing page header failed` — indistinguishable from a corrupt file;
* locally, one `FileReader` returned `PAR1` and then `XXXX` from the same
  offset across an in-place overwrite, with its cached size unchanged.

Same defect, two transports. A guard built once per transport would be built
twice and forgotten the third time.

---

## Local mutation matrix

Measured before choosing anything
(`extension/xpb_parquet/test/local_identity_matrix.cpp`). Everything is
observed through **`fstat` on the reader's own descriptor**, never `stat` on a
pathname, because those are different questions.

| scenario | ino | size | mtime | ctime | cookie | fd sees new bytes | path same object |
|---|---|---|---|---|---|---|---|
| 1 same-size overwrite in place | no | no | **yes** | yes | n/a | **YES** | yes |
| 2 different-size rewrite | no | yes | **yes** | yes | n/a | **YES** | yes |
| 3 append | no | yes | yes | yes | n/a | no | yes |
| 4 truncate + rewrite same size | no | no | **yes** | yes | n/a | **YES** | yes |
| 5 rename away, new file at path | no | no | no | **yes** | n/a | no | no |
| 6 atomic replace via rename | no | no | no | **yes** | n/a | no | no |
| 7 unlink while reader open | no | no | no | **yes** | n/a | no | no |
| 8 overwrite immediately after open | no | no | **yes** | yes | n/a | **YES** | yes |

What the matrix rules out, and none of it was obvious:

* **`st_ctim` is unusable.** It changes in all eight scenarios, including the
  three where the reader's bytes are untouched — rename and unlink both touch
  it. The instinct to "catch more by also checking ctime" would condemn an
  atomic rename replacement, which is the *correct* way to publish a new file.
  Only the measurement says so.
* **`st_size` alone is unsound.** It misses 1, 4 and 8, all same-size
  mutations, and 4 is the nastiest: truncate and rewrite the same number of
  bytes and the size is identical at both ends.
* **`st_ino` cannot be a change detector.** `fstat` follows the descriptor to
  its inode, so it never moves. It is still worth carrying, because it states
  *which* inode is pinned.
* **statx's change cookie is not available.** It exists for exactly this
  question and this kernel and filesystem do not supply it. Probed by raw
  constant rather than assumed absent; an earlier version of the probe
  declared its own `struct statx` to reach the field and the kernel wrote past
  it, killing the program with *stack smashing detected*. The probe now reads
  only the mask.

Timestamp resolution, which decides whether the chosen token is sound at all:
**0 of 200 back-to-back writes left `st_mtim` unchanged**, at nanosecond
granularity.

## Chosen FileReader identity

```
(st_dev, st_ino, st_size, st_mtim)   captured by fstat at open
```

`st_ctim` deliberately excluded, for the reason above. Size and identity come
from the **same** `fstat`, so there is no window where the reader holds a
length from one incarnation and pins another.

## What the token does and does not mean

**It means:** is this still the thing I opened.

**It is not a content hash**, and nothing may treat it as one. Over S3 this is
already proven: the same 180 143 345 bytes carried
`"c7333b8e11884cda65957f5f6e7e62c4-22"` after a 22-part multipart upload and
`"0d8da64d7ccb93afad60f721b6594e9d"` after a server-side copy of identical
content. The first is not an MD5 of anything. Nothing compares a token with a
checksum, or across objects.

**The token is opaque and transport-specific internally; the invariant is
transport-general.** `ParquetReader`, Arrow and the XPBatch operators have no
notion of ETags, inodes or timestamps and must not acquire one. They learn only
that a read failed.

## Validation granularity

**Every read**, checked immediately before the physical read, alongside the
interrupt hook. Not every Nth: a sampled check leaves a window in which a
mixed-incarnation read is *served*, which is the whole thing being prevented.

The check sits before the read rather than after, so a read is never issued
against an object that has already moved.

## S3 mapping

No semantic change from v0.5. The ETag guard is now expressed as an
implementation of the same invariant: capture the opaque token at open,
condition every GET on it with `If-Match`, and report a 412 through the base
class's one way of saying it. No re-HEAD, no reopen, no accepting a new ETag,
and 412 remains final.

The only change is where the refusal is reported and counted. `identity_conflicts()`
moved to `ObjectReader`, so both transports count the same thing in the same
place; the ETag now appears only in the error *detail*, as this transport's
private token.

**No `ObjectReader` API was extended with a transport-specific field.** That
was the stop condition for this phase and it was not reached.

## Retry interaction

**S3:** unchanged and re-verified. A transient failure, the object replaced
before the retry arrives, and the retry still carries the identity captured at
open — 412, query error. It cannot do otherwise by construction: the header is
rebuilt each attempt from the pinned token, and `size()` is called once, so no
path re-HEADs mid-read.

**FileReader:** has no retry, and one was not invented for symmetry. What
matters is that a subsequent read detects the change, which the guard test
asserts directly, together with the fact that a refused reader stays refused
and does not re-pin itself on a later read.

## Rename versus in-place overwrite

This is the distinction the guard exists to get right, and it falls out of
*where* validation happens rather than from a special case:

* **`fstat` on our own descriptor** asks "has the thing I am holding changed".
  An in-place overwrite changes it; a rename does not.
* **`stat` on the pathname** would ask "what does this name point at now",
  which is none of an open reader's business. A path-based check reports
  corruption for the one replacement idiom that is correct.

So an atomic rename replacement, a new file appearing at the pathname, and an
unlink all leave the reader alone — it still refers to the inode it opened, and
those bytes cannot change. Verified as controls, not argued.

## Failure semantics

One message shape for both transports, from
`ObjectReader::fail_identity_moved()`:

```
the object changed since this read began (<transport detail>). One reader reads
one object incarnation; not retrying and not reopening
```

Local detail: `local file was dev … ino … size … mtime …, is now …`.
S3 detail: `GET /bucket/key bytes=a-b: If-Match "…" -> HTTP 412`.

No partial result, no retry, no reopen. The value of this over the alternative
is attributability: without it the same situation surfaces as
`Deserializing page header failed`, and a reader of that message goes looking
for data corruption.

## Overhead

One extra `fstat` per read. Counted rather than inferred — `strace` of 402
reads shows **404 `pread64` and 409 `fstat`**, so the guard roughly doubles the
syscall count on the local path.

The cost of that, timed in isolation: **156–198 ns per `fstat`**, so 402 of them
is **0.06–0.08 ms**, or **0.04–0.05 %** of a 163 ms full scan.

A wall-clock before/after comparison cannot resolve it and is reported as such:

| shape | with guard | without |
|---|---|---|
| small range, 1 col | 18 ms | 22 ms |
| projected, 1 col | 19 ms | 21 ms |
| pruned, 4 cols | 33 ms | 32 ms |
| full scan, 4 cols | 163 ms | 171 ms |

The guarded build measured *faster* in three of four shapes. That is run-to-run
noise, not an improvement, and the effect being looked for is ~50 µs against
several ms of variance. The syscall count is the honest number.

No optimisation was attempted and the guarantee was not weakened.

## Known limits

* **The local token is only as fine as the filesystem's timestamp.** `mtime` is
  the sole detector for a same-size overwrite. Measured here: 200 of 200 writes
  moved it, at nanosecond granularity. **On a filesystem with coarse
  timestamps, a same-size overwrite inside one tick would go unnoticed and this
  token would be unsound there.** That is a property of the chosen token, not
  of the invariant, and it is the main reason statx's change cookie would be
  the better basis if a kernel supplied it.
* **An append is refused although the bytes already read are intact.**
  Conservative on purpose: the object is no longer the one opened, and a footer
  read from a file of size N is not authoritative for size N + M. It is a
  deliberate false positive, not an oversight.
* **No off switch for the local guard.** S3 has `XPB_S3_IDENTITY_GUARD=off`
  because v0.5's negative control needed it; the local negative control is the
  matrix plus a build with the check removed, which is how the overhead was
  measured. Adding a way to disable a correctness guard that nothing needs
  would be worse than the asymmetry.
* **Other transports are unexamined.** The invariant is written down, but only
  two implementations exist to honour it.
* **A refused reader is a failed query.** There is deliberately no
  reopen-and-restart, which for a long scan over a frequently rewritten object
  means repeated failure rather than progress. Right for correctness, wrong for
  availability; choosing otherwise belongs above this layer.

## Architecture verdict

**A — one-incarnation-per-reader is a clean transport-general `ObjectReader`
invariant.**

* `ObjectReader` gained the obligation, one way to report it broken, and a
  transport-general conflict count. It gained **no** transport-specific field,
  and no public `identity()`.
* Each implementation owns its own token and validation, as it already owns
  retry.
* `ParquetReader`, Arrow and the XPBatch operators are unchanged and
  identity-blind.
* **`FileReader` needed no escape hatch.** The cases that must not be flagged —
  atomic rename, a new file at the pathname, unlink — are handled by validating
  against the descriptor rather than the pathname, which is the correct reading
  of the invariant and not an exception to it. That is what makes this A rather
  than B.
* S3's behaviour is unchanged and re-verified against a server that enforces
  SigV4.

Not B: the one place a transport-specific escape hatch looked necessary — the
rename controls — turned out to need a better choice of *where* to validate,
not an exception. Not C: the generalisation cost one documented obligation and
one shared error path.

## Regression

| suite | result |
|---|---|
| `object_reader.sh` (incl. the new local identity section) | **57 correct, 0 wrong** |
| `s3_reader.sh` against RustFS with both guards | **69 correct, 0 wrong**, three consecutive runs |
| benchmark 08 | 14 agree / 0 differ, and 41 / 0 for the provider |
| benchmark 02 | `gate PASS`, `third-party gate PASS` |

Unchanged by this phase: logical results, request counts (402 / 44 / 23 / 2),
projection and pruning behaviour, and retry accounting.

One test defect was found and fixed in the process, and it matters more than
the code change. The mid-scan replacement test had used *sleep two seconds,
then replace*. The replacement is a server-side copy taking ~4.4 s against a
~9.5 s scan, so it became visible anywhere between row group 50 and past the
last read: the test passed in v0.5 and failed here, by luck both times. It now
triggers on a **read ordinal** through the fault proxy, and passed three runs in
a row. A wall-clock trigger was the defect, not the guard.
