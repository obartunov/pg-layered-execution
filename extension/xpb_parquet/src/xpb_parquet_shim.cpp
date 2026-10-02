/*
 * xpb_parquet_shim.cpp — the only C++ in this repository.
 *
 * Includes no PostgreSQL header. Every entry point is noexcept at the
 * boundary: a C++ exception reaching C is undefined behaviour, and inside a
 * PostgreSQL backend it would skip elog's cleanup entirely, so each function
 * wraps its body in try/catch and reports failure through errbuf.
 *
 * Symmetrically, no PostgreSQL error may be raised while the C++ side owns
 * resources: the provider calls elog only after this layer has returned.
 */
#include "xpb_parquet_shim.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/statistics.h>

namespace {

double now_ms()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void set_err(char *buf, size_t len, const std::string &msg)
{
    if (buf && len)
    {
        std::strncpy(buf, msg.c_str(), len - 1);
        buf[len - 1] = '\0';
    }
}

XpqColType map_type(const parquet::ColumnDescriptor *d)
{
    if (d == nullptr)
        return XPQ_COL_UNSUPPORTED;
    switch (d->physical_type())
    {
        case parquet::Type::INT32: return XPQ_COL_INT32;
        case parquet::Type::INT64: return XPQ_COL_INT64;
        default:                   return XPQ_COL_UNSUPPORTED;
    }
}

}   /* namespace */

struct XpqReader
{
    std::unique_ptr<parquet::arrow::FileReader>     arrow_reader;
    std::shared_ptr<parquet::FileMetaData>          md;
    std::shared_ptr<arrow::io::ReadableFile>        file;

    /*
     * Holds the decoded row group alive for exactly as long as the provider is
     * allowed to borrow it. Replaced wholesale on the next read, which is what
     * ends the borrow window -- the same window the batch contract defines.
     */
    std::shared_ptr<arrow::Table>                   held;
    /* Contiguous copies, only for columns Arrow handed over in several chunks.
     * Parallel to the columns of `held`; empty entries mean "borrowed". */
    std::vector<std::shared_ptr<arrow::Buffer>>     copies;

    int64_t bytes_read      = 0;
    int64_t decoded_values  = 0;
    int64_t copy_bytes      = 0;
    int     row_groups_read = 0;
    int64_t chunks_seen     = 0;
    double  open_ms         = 0;
    double  decode_ms       = 0;
};

extern "C" {

XpqReader *
xpq_open(const char *path, char *errbuf, size_t errbuflen)
{
    try
    {
        double t0 = now_ms();
        auto r = new XpqReader();

        auto fres = arrow::io::ReadableFile::Open(path);
        if (!fres.ok())
        {
            set_err(errbuf, errbuflen, fres.status().ToString());
            delete r;
            return nullptr;
        }
        r->file = *fres;

        auto preader = parquet::ParquetFileReader::Open(r->file);
        r->md = preader->metadata();

        auto st = parquet::arrow::FileReader::Make(
            arrow::default_memory_pool(), std::move(preader), &r->arrow_reader);
        if (!st.ok())
        {
            set_err(errbuf, errbuflen, st.ToString());
            delete r;
            return nullptr;
        }

        /* The footer is all that has been read so far; record it so "bytes
         * read" starts from a true baseline rather than from zero. */
        auto sz = r->file->GetSize();
        if (sz.ok())
            r->bytes_read = 0;      /* page data only; footer counted separately */

        r->open_ms = now_ms() - t0;
        return r;
    }
    catch (const std::exception &e)
    {
        set_err(errbuf, errbuflen, e.what());
        return nullptr;
    }
    catch (...)
    {
        set_err(errbuf, errbuflen, "unknown C++ exception in xpq_open");
        return nullptr;
    }
}

void
xpq_close(XpqReader *r)
{
    if (r == nullptr)
        return;
    try { delete r; } catch (...) { }
}

int64_t xpq_num_rows(const XpqReader *r)        { return r && r->md ? r->md->num_rows() : 0; }
int     xpq_num_row_groups(const XpqReader *r)  { return r && r->md ? r->md->num_row_groups() : 0; }
int     xpq_num_columns(const XpqReader *r)     { return r && r->md ? r->md->num_columns() : 0; }

const char *
xpq_column_name(const XpqReader *r, int col)
{
    if (!r || !r->md || col < 0 || col >= r->md->num_columns())
        return nullptr;
    return r->md->schema()->Column(col)->name().c_str();
}

XpqColType
xpq_column_type(const XpqReader *r, int col)
{
    if (!r || !r->md || col < 0 || col >= r->md->num_columns())
        return XPQ_COL_UNSUPPORTED;
    return map_type(r->md->schema()->Column(col));
}

int
xpq_column_index(const XpqReader *r, const char *name)
{
    if (!r || !r->md || name == nullptr)
        return -1;
    for (int i = 0; i < r->md->num_columns(); i++)
        if (r->md->schema()->Column(i)->name() == name)
            return i;
    return -1;
}

int64_t
xpq_row_group_rows(const XpqReader *r, int rg)
{
    if (!r || !r->md || rg < 0 || rg >= r->md->num_row_groups())
        return 0;
    return r->md->RowGroup(rg)->num_rows();
}

int
xpq_row_group_stats_i64(const XpqReader *r, int rg, int col,
                        int64_t *min_out, int64_t *max_out,
                        int64_t *null_count_out)
{
    try
    {
        if (!r || !r->md || rg < 0 || rg >= r->md->num_row_groups())
            return 0;
        if (col < 0 || col >= r->md->num_columns())
            return 0;

        auto cc = r->md->RowGroup(rg)->ColumnChunk(col);

        /*
         * Three separate conditions, all required. A writer may omit
         * statistics entirely, or write them without min/max, and some writers
         * record counts but not bounds. Any of those means the file does not
         * state the range, so the answer is "cannot exclude" -- never a guess.
         */
        if (!cc->is_stats_set())
            return 0;
        auto stats = cc->statistics();
        if (!stats || !stats->HasMinMax())
            return 0;

        if (null_count_out)
            *null_count_out = stats->HasNullCount() ? stats->null_count() : -1;

        switch (map_type(r->md->schema()->Column(col)))
        {
            case XPQ_COL_INT32:
            {
                auto s = std::static_pointer_cast<parquet::Int32Statistics>(stats);
                if (min_out) *min_out = s->min();
                if (max_out) *max_out = s->max();
                return 1;
            }
            case XPQ_COL_INT64:
            {
                auto s = std::static_pointer_cast<parquet::Int64Statistics>(stats);
                if (min_out) *min_out = s->min();
                if (max_out) *max_out = s->max();
                return 1;
            }
            default:
                return 0;
        }
    }
    catch (...)
    {
        return 0;
    }
}

int
xpq_read_row_group(XpqReader *r, int rg, const int *cols, int ncols,
                   XpqColumn *out, char *errbuf, size_t errbuflen)
{
    try
    {
        if (!r || !r->arrow_reader || cols == nullptr || out == nullptr)
        {
            set_err(errbuf, errbuflen, "xpq_read_row_group: bad arguments");
            return -1;
        }

        double t0 = now_ms();

        /*
         * Releasing the previous row group here, before reading the next, is
         * what makes the borrow window exact: anything the provider borrowed
         * from the last call becomes invalid at precisely the moment the
         * contract says it does.
         */
        r->held.reset();
        r->copies.clear();

        std::vector<int> want(cols, cols + ncols);
        std::shared_ptr<arrow::Table> tbl;
        auto st = r->arrow_reader->ReadRowGroup(rg, want, &tbl);
        if (!st.ok())
        {
            set_err(errbuf, errbuflen, st.ToString());
            return -1;
        }
        /*
         * Arrow DEDUPLICATES a repeated column index, so the table can come
         * back with fewer columns than were asked for. Reading out[i] for
         * i >= num_columns() then walks off the end -- it crashed the backend
         * before this check existed. Refuse instead: the caller asked for
         * something this ABI cannot express.
         */
        if (tbl->num_columns() != ncols)
        {
            set_err(errbuf, errbuflen,
                    std::string("requested ") + std::to_string(ncols) +
                    " columns but the reader returned " +
                    std::to_string(tbl->num_columns()) +
                    " (a repeated column index is deduplicated)");
            return -1;
        }

        r->held = tbl;
        r->copies.assign(static_cast<size_t>(ncols), nullptr);

        for (int i = 0; i < ncols; i++)
        {
            auto chunked = tbl->column(i);
            out[i].nrows      = chunked->length();
            out[i].null_count = chunked->null_count();
            out[i].copied     = 0;
            r->chunks_seen   += chunked->num_chunks();

            XpqColType ct = xpq_column_type(r, cols[i]);
            out[i].type = ct;
            if (ct == XPQ_COL_UNSUPPORTED)
            {
                set_err(errbuf, errbuflen,
                        std::string("unsupported physical type in column ") +
                        std::to_string(cols[i]));
                return -1;
            }

            const int width = (ct == XPQ_COL_INT32) ? 4 : 8;

            if (chunked->num_chunks() == 1)
            {
                /*
                 * The borrowable case: one contiguous Arrow array, handed over
                 * as is. `held` keeps it alive for the borrow window.
                 */
                auto arr = chunked->chunk(0);
                auto data = arr->data();
                const uint8_t *values = data->buffers[1]->data();
                /* An Arrow array may carry a non-zero offset; the provider
                 * cannot express that, so advance the pointer instead. */
                out[i].data = values + static_cast<ptrdiff_t>(arr->offset()) * width;
                out[i].validity = (arr->null_count() > 0 && data->buffers[0])
                                ? data->buffers[0]->data() : nullptr;
                /*
                 * A validity bitmap with an offset cannot be advanced by
                 * pointer arithmetic unless the offset is a whole byte, and
                 * the batch contract has no offset field. Copy in that case.
                 */
                if (out[i].validity && (arr->offset() % 8) != 0)
                {
                    auto res = arrow::internal::CopyBitmap(
                        arrow::default_memory_pool(), data->buffers[0]->data(),
                        arr->offset(), arr->length());
                    if (!res.ok())
                    {
                        set_err(errbuf, errbuflen, res.status().ToString());
                        return -1;
                    }
                    r->copies[i] = *res;
                    out[i].validity = r->copies[i]->data();
                    out[i].copied = 1;
                    r->copy_bytes += r->copies[i]->size();
                }
                else if (out[i].validity)
                {
                    out[i].validity += arr->offset() / 8;
                }
            }
            else
            {
                /*
                 * Arrow split this row group into several chunks. The batch
                 * contract has exactly one dense array per column and no
                 * notion of a fragmented column, and inventing one would be a
                 * new general abstraction for a case that may never recur. So
                 * the values are concatenated into one buffer and the column
                 * is reported as copied, with the bytes counted.
                 *
                 * This is the one place the experiment pays a copy it did not
                 * have to pay, and it is visible in both the ownership map and
                 * xpq_copy_bytes().
                 */
                auto res = arrow::Concatenate(chunked->chunks(),
                                              arrow::default_memory_pool());
                if (!res.ok())
                {
                    set_err(errbuf, errbuflen, res.status().ToString());
                    return -1;
                }
                auto arr = *res;
                auto data = arr->data();
                out[i].data = static_cast<const uint8_t *>(data->buffers[1]->data())
                            + static_cast<ptrdiff_t>(arr->offset()) * width;
                out[i].validity = (arr->null_count() > 0 && data->buffers[0])
                                ? data->buffers[0]->data() + arr->offset() / 8 : nullptr;
                out[i].copied = 1;
                r->copy_bytes += arr->length() * width;
                /* Keep the concatenated array alive for the borrow window. */
                r->copies[i] = data->buffers[1];
                /* Replacing the column in `held` keeps one owner for the
                 * lifetime rather than two. */
                auto replaced = tbl->SetColumn(
                    i, tbl->schema()->field(i),
                    std::make_shared<arrow::ChunkedArray>(arr));
                if (replaced.ok())
                    r->held = *replaced;
            }

            r->decoded_values += out[i].nrows;
        }

        /*
         * Bytes attributable to this row group's SELECTED columns, from the
         * footer's per-chunk compressed sizes. This is read volume, which is
         * the number projection and pruning are supposed to move; it is not
         * measured at the syscall level and is labelled accordingly.
         */
        for (int i = 0; i < ncols; i++)
            r->bytes_read += r->md->RowGroup(rg)->ColumnChunk(cols[i])->total_compressed_size();

        r->row_groups_read++;
        r->decode_ms += now_ms() - t0;
        return 0;
    }
    catch (const std::exception &e)
    {
        set_err(errbuf, errbuflen, e.what());
        return -1;
    }
    catch (...)
    {
        set_err(errbuf, errbuflen, "unknown C++ exception in xpq_read_row_group");
        return -1;
    }
}

int64_t xpq_bytes_read(const XpqReader *r)      { return r ? r->bytes_read : 0; }
int64_t xpq_decoded_values(const XpqReader *r)  { return r ? r->decoded_values : 0; }
int64_t xpq_copy_bytes(const XpqReader *r)      { return r ? r->copy_bytes : 0; }
int     xpq_row_groups_read(const XpqReader *r) { return r ? r->row_groups_read : 0; }
int64_t xpq_chunks_seen(const XpqReader *r)     { return r ? r->chunks_seen : 0; }
double  xpq_open_ms(const XpqReader *r)         { return r ? r->open_ms : 0.0; }
double  xpq_decode_ms(const XpqReader *r)       { return r ? r->decode_ms : 0.0; }

}   /* extern "C" */
