# ZLFS registry: zone lifetime, and a silent refusal to register

Status: **open, pre-existing. Not a regression of the hash-growth series --
present at `3eb21db`, the last commit on `origin/main`.**

Raised by architectural review alongside two correctness blockers in
`xpb_src_pgcolumnar.c` and `xpb_zlfs.c`. Deliberately kept out of those two
fixes: this is a lifetime and API-honesty question, not a wrong answer, and
mixing it into a correctness commit would make both harder to review.

## 1. Zones are never freed

The registry and every zone buffer are allocated directly under
`TopMemoryContext`. Eviction and drop both remove only the pointer:

```
xpb_zlfs.c     zlfs_reg->zones[i] = zlfs_reg->zones[--zlfs_reg->nzones];
```

The zone's own `cols[]`, `col_validity[]` and the `ZlfsZone` itself stay
allocated. A long-lived backend that rebuilds or drops zones repeatedly
accumulates every previous generation. Nothing frees them before backend exit.

Rebuild is the common case, not an exotic one: a zone must be rebuilt after any
DML to its range, which is already documented as a limitation.

The fix is a context per zone rather than free-by-hand:

```
ZLFS registry context
    |
    +-- zone context A
    +-- zone context B
    +-- zone context C
```

and `MemoryContextDelete(zone->mcxt)` on eviction and on drop. That also removes
the need for the `MemoryContextSwitchTo(zlfs_reg->mcxt)` dance at every
allocation site inside `zlfs_build_zone()`, which is currently repeated five
times and is the kind of thing that eventually gets missed once.

## 2. A full registry is a silent no-op that still reports VALID

`zlfs_build_zone()` writes the zone file, then:

```
xpb_zlfs.c     if (zlfs_reg->nzones < ZLFS_MAX_ZONES)
                   zlfs_reg->zones[zlfs_reg->nzones++] = zone;
               ...
               PG_RETURN_TEXT_P(cstring_to_text("VALID"));
```

`ZLFS_MAX_ZONES` is 256. At the limit the file is written and the zone is *not*
registered, and the caller is told `VALID`. The next query does not find the
zone in the registry and falls back to the heap, so the answer stays correct --
this is not a wrong-result defect -- but the caller was told a zone exists when
it does not, and the only symptom is a performance cliff with no diagnostic.

It should refuse explicitly, and the refusal should come *before* the file is
written rather than after, so the directory does not collect files the registry
will not serve.

## Why it is not being fixed now

Both are pre-existing and neither produces a wrong answer. The two defects found
in the same review that *can* produce a wrong answer -- stale validity after a
predicate rejection, and an empty zone's column types -- are fixed separately
and first. Folding a memory-lifetime refactor into those commits would mean a
reviewer checking a correctness fix has to read an allocator change at the same
time.

## What must not happen

Do not "fix" the accumulation by raising `ZLFS_MAX_ZONES`, and do not fix the
silent non-registration by evicting an arbitrary existing zone to make room. The
first moves the boundary without closing it; the second changes which zones are
served without the caller asking, which is a correctness-adjacent surprise in a
cache that queries silently depend on.
