# pgcolumnar integration patch

Local integration patch.
Exposes the existing Fold API to an out-of-tree consumer.
Not submitted or proposed upstream.

Target: `commandprompt/pgcolumnar` at commit `5b20ae8` (`VERSION` = `1.0-alpha5`).

`0001-export-fold-reader-api.patch` marks six declarations `PGDLLEXPORT`:

    PgColumnarStorageId
    PgColumnarBeginRead
    PgColumnarEndRead
    PgColumnarReadFoldNextGroup
    PgColumnarReadFoldGroupInfo
    PgColumnarReadFoldColumn

It adds no code and changes no behaviour. PostgreSQL builds extension modules
with `-fvisibility=hidden` (`CFLAGS_SL_MODULE`), so everything in
`pgcolumnar.so` that is not `PGDLLEXPORT` is a local symbol and cannot be
resolved from another module. Without the patch `xp_batch.so` fails to load
with `undefined symbol: PgColumnarReadFoldColumn`.

Applying it:

    git -C <pgcolumnar> checkout 5b20ae8
    git -C <pgcolumnar> apply <this dir>/0001-export-fold-reader-api.patch
    make -C <pgcolumnar> install PG_CONFIG=...

and `shared_preload_libraries = 'pgcolumnar,xp_batch'`, in that order, because
symbols resolve at load time.

The shape this would need to take if it were ever more than an experiment is a
versioned entry point on pgcolumnar's side — one `pgcolumnar_get_batch_api()`
returning a struct of function pointers with a version stamp — so a consumer
fails to negotiate rather than fails to link, and the fold contract has
somewhere to be documented. That is not what this patch is.
