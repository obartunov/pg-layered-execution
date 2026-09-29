# pgcolumnar patches

`xpb_src_pgcolumnar.c` reads pgcolumnar's decoded data in-process through its
fold reader API. pgcolumnar builds with `-fvisibility=hidden`, so those six
functions are declared in its headers but **not exported from the shared
library**. Without the patch below, `xp_batch.so` fails to resolve them at load;
and because `xp_batch` is normally in `shared_preload_libraries`, that means the
server does not start.

```
0001-export-fold-reader-api.patch
    PGDLLEXPORT on the six symbols xp_batch links against:
        PgColumnarBeginRead          PgColumnarEndRead
        PgColumnarReadFoldNextGroup  PgColumnarReadFoldGroupInfo
        PgColumnarReadFoldColumn     PgColumnarStorageId
```

Apply it in a pgcolumnar checkout before building:

```
git -C <pgcolumnar> apply .../0001-export-fold-reader-api.patch
make -C <pgcolumnar> PG_CONFIG=<pg20devel>/bin/pg_config install
```

Verified 2026-09-29 to apply cleanly and to export all six symbols at both:

```
5b20ae8   1.0-alpha5, the revision the published benchmarks were built against
be31874   commandprompt/pgcolumnar main, 112 commits later
```

Both revisions also build clean against PostgreSQL 20devel, which is outside
pgcolumnar's own supported matrix -- `src/columnar_compat.h` tops out at
`PG_VERSION_NUM >= 190000` and `test/run_all_versions.sh` lists pg15..pg19. It
compiles, but upstream does not test it, so a PG20devel-specific breakage would
reach us before it reaches them.

## Why this file exists

It did not, until 2026-09-29. `xpb_src_pgcolumnar.c` referenced
`patches/pgcolumnar-alpha5/0001-export-fold-reader-api.patch` in its header
comment, but no such file was ever committed. The working environment only
functioned because a patched `pgcolumnar.so` built in September happened to
survive on disk. A clean rebuild from the recorded state would have hidden the
symbols and broken the 05-A pgcolumnar arm -- and the server with it.

The patch here was reconstructed from the exported symbol set of that binary and
verified by building unpatched (six symbols hidden) and patched (six exported).
