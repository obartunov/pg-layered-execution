import pyarrow as pa, pyarrow.parquet as pq, os
A="/home/claude/pg-layered-execution/benchmarks/02-batch-joins/data/reg_buh.parquet"
B="/tmp/rustfs-run/reg_buh_B.parquet"
md=pq.ParquetFile(A).metadata
rg0=md.row_group(0)
comp=rg0.column(0).compression
rows_per_rg=rg0.num_rows
print(f"A: {md.num_rows} rows, {md.num_row_groups} row groups, {rows_per_rg}/rg, compression {comp}, {os.path.getsize(A)} bytes")
t=pq.read_table(A)
# Same schema, same layout, different VALUES: amount_dt + 1000000. A mixed read
# is then either a wrong sum or a decode error, and both prove B's bytes arrived.
names=t.column_names
i=names.index("amount_dt")
col=t.column(i).combine_chunks()
newcol=pa.compute.add(col, pa.scalar(1000000, type=col.type))
t2=t.set_column(i, names[i], newcol)
pq.write_table(t2, B, row_group_size=rows_per_rg, compression=str(comp).lower(),
               write_statistics=True, version="2.6")
mdb=pq.ParquetFile(B).metadata
print(f"B: {mdb.num_rows} rows, {mdb.num_row_groups} row groups, {os.path.getsize(B)} bytes")
print(f"length delta B-A: {os.path.getsize(B)-os.path.getsize(A)}")
import pyarrow.compute as pc
print(f"sum(amount_dt) A = {pc.sum(t.column(i)).as_py()}")
print(f"sum(amount_dt) B = {pc.sum(newcol).as_py()}")
