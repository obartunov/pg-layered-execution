# test/zlfs_persistence.sh does not run anywhere

Status: **open, test-harness portability. Not a product defect.**

`extension/xp_batch/test/zlfs_persistence.sh` exits 1 before reaching its first
check. Two reasons, both environmental:

```
zlfs_persistence.sh:11   su - pguser -c "export PATH=/root/pginstall/bin:$PATH; ..."
zlfs_persistence.sh:24   su - pguser -c "... pg_ctl -D $PGDATA restart ..."
```

* the install prefix is hard-coded to `/root/pginstall`, while this branch is
  built against PG20devel at whatever prefix `PG_CONFIG` points to;
* it calls `su - pguser` from inside the script, which prompts for a password
  when the caller is already `pguser`, so the run dies at
  `su: Authentication failure`.

Every other suite takes `(PGPORT, PGHOST)` and lets the caller decide who runs
it. This one decides for itself, which is why it is the only one that cannot run.

It was found while verifying the ZLFS empty-zone fix, because that fix changes
what is persisted and this is the suite that would normally cover a restart.
The check was done by hand instead, and it passes: a non-empty int8 zone and an
empty one both reload from disk as `VALID` after a restart, with int8 values
round-tripping exactly. So the coverage gap is in the harness, not in the
behaviour.

Deliberately not fixed alongside the two correctness fixes it was found next to:
a hard-coded `/root/pginstall` has nothing to do with either of them, and folding
it in would put an unrelated infrastructure change inside the semantic boundary
of that series.

## The task

1. Take `(PGPORT, PGHOST [, DB])` like every other suite, and drop the internal
   `su`: whoever runs the suite has already decided the user.
2. Take the install prefix from `PG_CONFIG` or `PATH` rather than a constant.
3. Restart through the same `pg_ctl` the caller already has on `PATH`, and skip
   with a clear message rather than failing when the data directory is not
   writable by the running user.
4. Then wire it into the gate set, which is the point -- ZLFS is the only part
   of this tree with on-disk state that outlives the backend, and nothing in the
   regular gates restarts the server.

One thing to preserve while rewriting: the suite must still assert that a zone
survives a real postmaster restart, not merely a new connection. `xp_batch` is
in `shared_preload_libraries`, so a new backend inherits the postmaster's copy of
the library and proves nothing about reload from disk.
