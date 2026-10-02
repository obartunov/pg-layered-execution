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
#include <stdexcept>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/statistics.h>

namespace {

/*
 * Counting wrapper over the file.
 *
 * Deliberately small: it forwards every call and records what was asked for.
 * The point is to separate the bytes read to parse the footer and schema from
 * the bytes read for column chunks -- otherwise open overhead and data volume
 * mix and neither can be explained. read_calls matters on its own: for an
 * object store, a hundred small ranges and one large one are different
 * economics even at equal bytes.
 *
 * Not an investigation of Arrow internals. It measures what Arrow ASKS the file
 * for, which is one layer above the page cache and one below the OS.
 */
class CountingFile : public arrow::io::RandomAccessFile
{
public:
    explicit CountingFile(std::shared_ptr<arrow::io::RandomAccessFile> inner)
        : inner_(std::move(inner)) {}

    /* Everything read before the first column chunk is open/metadata. */
    void start_data_phase() { data_phase_ = true; }

    int64_t meta_bytes()  const { return meta_bytes_; }
    int64_t data_bytes()  const { return data_bytes_; }
    int64_t read_calls()  const { return read_calls_; }

    arrow::Status Close() override { return inner_->Close(); }
    bool closed() const override { return inner_->closed(); }
    arrow::Result<int64_t> Tell() const override { return inner_->Tell(); }
    arrow::Status Seek(int64_t position) override { return inner_->Seek(position); }
    arrow::Result<int64_t> GetSize() override { return inner_->GetSize(); }

    arrow::Result<int64_t> Read(int64_t nbytes, void *out) override
    {
        auto res = inner_->Read(nbytes, out);
        if (res.ok()) account(*res);
        return res;
    }

    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t nbytes) override
    {
        auto res = inner_->Read(nbytes);
        if (res.ok()) account((*res)->size());
        return res;
    }

    arrow::Result<int64_t> ReadAt(int64_t position, int64_t nbytes, void *out) override
    {
        auto res = inner_->ReadAt(position, nbytes, out);
        if (res.ok()) account(*res);
        return res;
    }

    arrow::Result<std::shared_ptr<arrow::Buffer>> ReadAt(int64_t position,
                                                        int64_t nbytes) override
    {
        auto res = inner_->ReadAt(position, nbytes);
        if (res.ok()) account((*res)->size());
        return res;
    }

private:
    void account(int64_t n)
    {
        read_calls_++;
        if (data_phase_) data_bytes_ += n;
        else             meta_bytes_ += n;
    }

    std::shared_ptr<arrow::io::RandomAccessFile> inner_;
    bool    data_phase_ = false;
    int64_t meta_bytes_ = 0;
    int64_t data_bytes_ = 0;
    int64_t read_calls_ = 0;
};

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
    std::shared_ptr<CountingFile>                  counted;

    /*
     * Holds the decoded row group alive for exactly as long as the provider is
     * allowed to borrow it. Replaced wholesale on the next read, which is what
     * ends the borrow window -- the same window the batch contract defines.
     */
    std::shared_ptr<arrow::Table>                   held;
    /* Contiguous copies, only for columns Arrow handed over in several chunks.
     * Parallel to the columns of `held`; empty entries mean "borrowed". */
    std::vector<std::shared_ptr<arrow::Buffer>>     copies;

    int64_t attributed_bytes = 0;
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
        r->counted = std::make_shared<CountingFile>(r->file);

        auto preader = parquet::ParquetFileReader::Open(r->counted);
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
            r->attributed_bytes = 0;   /* column-chunk bytes only */

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

/*
 * Guarded although it looks like the trivial accessors above it:
 * FileMetaData::RowGroup() returns a unique_ptr and therefore allocates, so a
 * std::bad_alloc could escape where the others cannot throw at all -- they
 * bounds-check and then read parsed footer structures, with
 * SchemaDescriptor::Column() returning a borrowed pointer and num_*() returning
 * a member. An exception crossing this ABI is undefined behaviour, so the rule
 * is that no entry point may throw; the ones without a try block are the ones
 * where that holds by construction.
 */
int64_t
xpq_row_group_rows(const XpqReader *r, int rg)
{
    try
    {
        if (!r || !r->md || rg < 0 || rg >= r->md->num_row_groups())
            return 0;
        return r->md->RowGroup(rg)->num_rows();
    }
    catch (...) { return 0; }
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

        /* Everything read from here on is column-chunk data, not metadata. */
        r->counted->start_data_phase();

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
         * Projected compressed bytes attributable from the footer, for this row
         * group's SELECTED columns only. Not an I/O measurement -- see the
         * header. 0005 adds real byte-range accounting.
         */
        for (int i = 0; i < ncols; i++)
            r->attributed_bytes +=
                r->md->RowGroup(rg)->ColumnChunk(cols[i])->total_compressed_size();

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

int64_t xpq_attributed_bytes(const XpqReader *r) { return r ? r->attributed_bytes : 0; }

int64_t
xpq_column_compressed_bytes(const XpqReader *r, int col)
{
    try
    {
        if (!r || !r->md || col < 0 || col >= r->md->num_columns())
            return 0;
        int64_t total = 0;
        for (int rg = 0; rg < r->md->num_row_groups(); rg++)
            total += r->md->RowGroup(rg)->ColumnChunk(col)->total_compressed_size();
        return total;
    }
    catch (...) { return 0; }
}
int64_t xpq_decoded_values(const XpqReader *r)  { return r ? r->decoded_values : 0; }
int64_t xpq_copy_bytes(const XpqReader *r)      { return r ? r->copy_bytes : 0; }
int     xpq_row_groups_read(const XpqReader *r) { return r ? r->row_groups_read : 0; }
int64_t xpq_chunks_seen(const XpqReader *r)     { return r ? r->chunks_seen : 0; }
double  xpq_open_ms(const XpqReader *r)         { return r ? r->open_ms : 0.0; }
double  xpq_decode_ms(const XpqReader *r)       { return r ? r->decode_ms : 0.0; }

int64_t xpq_meta_bytes(const XpqReader *r)
{ return (r && r->counted) ? r->counted->meta_bytes() : 0; }
int64_t xpq_data_bytes(const XpqReader *r)
{ return (r && r->counted) ? r->counted->data_bytes() : 0; }
int64_t xpq_read_calls(const XpqReader *r)
{ return (r && r->counted) ? r->counted->read_calls() : 0; }

/*
 * Controlled failure for the exception boundary.
 *
 * Every entry point here is wrapped so that no C++ exception reaches the C ABI,
 * where it would be undefined behaviour rather than an error. That property is
 * otherwise only arguable from reading the code: Arrow reports most problems as
 * a Status, so a malformed file exercises the status path and leaves the catch
 * arms untested. This throws on purpose, through the same shape of wrapper, so
 * both arms are reached by a test:
 *
 *   1  std::exception     -- caught by the typed arm, message preserved
 *   2  parquet::ParquetException (also a std::exception, but Arrow's own)
 *   3  not a std::exception at all -- only catch (...) can take it
 *
 * Reachable only through xpq_selftest_throw() in the module's SQL surface,
 * which exists for test/parquet_cxx_boundary.sh. It touches no reader, no file
 * and no global state.
 */
int xpq_selftest_throw(int kind, char *errbuf, size_t errbuflen)
{
    try
    {
        switch (kind)
        {
            case 1: throw std::runtime_error("selftest: std::runtime_error");
            case 2: throw parquet::ParquetException("selftest: ParquetException");
            case 3: throw 42;            /* not derived from std::exception */
            default: return 0;           /* nothing thrown */
        }
    }
    catch (const std::exception &e)
    {
        set_err(errbuf, errbuflen, std::string("caught std::exception: ") + e.what());
        return -1;
    }
    catch (...)
    {
        set_err(errbuf, errbuflen, "caught non-std exception");
        return -1;
    }
}

}   /* extern "C" */
