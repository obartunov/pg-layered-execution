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
#include "xpb_object_reader.h"
#include "xpb_s3_reader.h"

#include <atomic>
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
 * ObjectReader presented as the input Arrow's Parquet reader wants.
 *
 * This replaces the former CountingFile, which wrapped an
 * arrow::io::ReadableFile and counted what Arrow asked it for. The counting
 * moved down into ObjectReader, which is the only layer that sees a physical
 * read; what is left here is the adapter, and it is the ONLY Arrow-shaped code
 * in the byte path.
 *
 * The direction matters. ObjectReader does not inherit from
 * arrow::io::RandomAccessFile -- Arrow's interface carries Tell/Seek, a stream
 * position and a Status vocabulary that a byte-range object store has no
 * opinion about, and inheriting would mean a future S3Reader is written
 * against Arrow instead of against our contract. So the Arrow shape is
 * satisfied HERE, by translation, and the position-bearing half of it is
 * emulated: Arrow's sequential Read() is implemented as read_at(pos_) and a
 * cursor bump, because the Parquet reader uses it for the footer probe.
 */
class ArrowObjectInput : public arrow::io::RandomAccessFile
{
public:
    explicit ArrowObjectInput(xpb::ObjectReader *obj) : obj_(obj) {}

    /*
     * What ARROW asked for, counted here because this is the layer Arrow's
     * request arrives at. The reader's bytes_requested/bytes_returned are the
     * PHYSICAL level underneath, and the two differ as soon as anything below
     * retries or splits a read.
     *
     * Three levels are worth distinguishing for a remote source: what Arrow
     * wanted, what was asked of the store, and what crossed the wire. For a
     * local reader the last two coincide, so only two counters exist; the
     * third needs a transport before it means anything.
     */
    int64_t logical_calls() const { return logical_calls_; }
    int64_t logical_bytes() const { return logical_bytes_; }

    /* Everything read before the first column chunk is metadata. */
    void start_data_phase() { obj_->set_phase(xpb::ReadPhase::kData); }

    arrow::Status Close() override { obj_->close(); closed_ = true; return arrow::Status::OK(); }
    bool closed() const override { return closed_; }

    arrow::Result<int64_t> Tell() const override { return pos_; }

    arrow::Status Seek(int64_t position) override
    {
        if (position < 0)
            return arrow::Status::Invalid("negative seek");
        pos_ = position;
        return arrow::Status::OK();
    }

    arrow::Result<int64_t> GetSize() override
    {
        int64_t n = obj_->size();
        if (n < 0)
            return arrow::Status::IOError(obj_->last_error());
        return n;
    }

    /*
     * POSITIONAL reads use read_exact. Arrow computes these ranges from the
     * footer, so they are known to lie inside the object: anything short is a
     * fault, and accepting it would hand Parquet a column chunk with a hole in
     * it. Measured on the benchmark file, every range Arrow asked for was
     * inside the object and no read was ever short.
     *
     * Retry, if a reader has any, happens below this line. There is
     * deliberately no retry policy here -- that is what keeps transport
     * concerns out of the Parquet shim.
     */
    arrow::Result<int64_t> ReadAt(int64_t position, int64_t nbytes, void *out) override
    {
        note_logical(nbytes);
        if (!obj_->read_exact(position, nbytes, out))
            return arrow::Status::IOError(obj_->last_error());
        return nbytes;
    }

    arrow::Result<std::shared_ptr<arrow::Buffer>> ReadAt(int64_t position,
                                                        int64_t nbytes) override
    {
        note_logical(nbytes);
        ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateResizableBuffer(nbytes));
        if (!obj_->read_exact(position, nbytes, buf->mutable_data()))
            return arrow::Status::IOError(obj_->last_error());
        return std::shared_ptr<arrow::Buffer>(std::move(buf));
    }

    /*
     * The SEQUENTIAL half, which is Arrow's stream shape and is where a short
     * read is a legitimate answer -- a stream reader asks for a buffer-full and
     * takes what is there. So this one uses read_at_most, and the end of the
     * object comes from the reader's eof flag rather than from comparing counts.
     */
    arrow::Result<int64_t> Read(int64_t nbytes, void *out) override
    {
        note_logical(nbytes);
        xpb::ReadResult r = obj_->read_at_most(pos_, nbytes, out);
        if (!r.ok)
            return arrow::Status::IOError(obj_->last_error());
        pos_ += r.bytes;
        return r.bytes;
    }

    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t nbytes) override
    {
        note_logical(nbytes);
        ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateResizableBuffer(nbytes));
        xpb::ReadResult r = obj_->read_at_most(pos_, nbytes, buf->mutable_data());
        if (!r.ok)
            return arrow::Status::IOError(obj_->last_error());
        if (r.bytes < nbytes)
            ARROW_RETURN_NOT_OK(buf->Resize(r.bytes));
        pos_ += r.bytes;
        return std::shared_ptr<arrow::Buffer>(std::move(buf));
    }

private:
    /* Atomic for the same reason the reader's counters are: Arrow calls this
     * from its thread pool as well as from the backend thread. */
    void note_logical(int64_t nbytes)
    {
        if (nbytes <= 0)
            return;
        logical_calls_.fetch_add(1, std::memory_order_relaxed);
        logical_bytes_.fetch_add(nbytes, std::memory_order_relaxed);
    }

    xpb::ObjectReader  *obj_;       /* borrowed; XpqReader owns it */
    int64_t             pos_ = 0;
    bool                closed_ = false;
    std::atomic<int64_t> logical_calls_{0};
    std::atomic<int64_t> logical_bytes_{0};
};

/*
 * The interrupt hook, injected by the C side (xpq_set_interrupt_hook) so that
 * this file still includes no PostgreSQL header. Called before each physical
 * read; it may longjmp, which is why ObjectReader touches nothing before
 * calling it.
 */
xpb::InterruptHook g_interrupt_hook = nullptr;

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

    /*
     * The byte source, and the Arrow-shaped view of it. Order matters for
     * destruction: the adapter borrows the reader, and Arrow may hold the
     * adapter, so the reader is released last -- it is declared first and
     * members are destroyed in reverse.
     */
    std::unique_ptr<xpb::ObjectReader>              object;
    std::shared_ptr<ArrowObjectInput>               input;

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

/*
 * The one place a byte source is chosen. open_file_reader() is the only thing
 * in the module that touches a local file; the faulty wrapper exists so that
 * failure semantics can be tested without a network, and takes ownership of
 * the real reader it wraps.
 *
 * A second real implementation (S3) would be a third branch here and nothing
 * else -- that is the claim the boundary is for.
 */
static xpb::ObjectReader *
make_object_reader(const char *path, const xpb::FaultSpec *fault, std::string *err)
{
    /*
     * The scheme branch, and the whole of what adding a transport costs above
     * the reader. Nothing else in this file, in the Parquet reader or in any
     * XPBatch operator distinguishes the two.
     */
    xpb::ObjectReader *base = xpb::is_s3_uri(path)
                            ? xpb::open_s3_reader(path, err)
                            : xpb::open_file_reader(path, err);
    if (base == nullptr)
        return nullptr;
    if (fault == nullptr || fault->mode == xpb::FaultMode::kNone)
        return base;
    return xpb::open_faulty_reader(base, *fault, err);
}

static XpqReader *
xpq_open_internal(const char *path, const xpb::FaultSpec *fault,
                  char *errbuf, size_t errbuflen)
{
    try
    {
        double t0 = now_ms();

        /*
         * unique_ptr, not a raw new: ParquetFileReader::Open() throws
         * ParquetException on a truncated or corrupt footer, and that throw
         * lands in the catch handlers below, which cannot see a pointer
         * declared in here. A raw pointer therefore leaked the reader -- and
         * with it the open file descriptor -- on every bad file. Ownership is
         * handed to the caller by release() on the success path only.
         */
        std::unique_ptr<XpqReader> r(new XpqReader());

        /*
         * No pathname below this line. open_file_reader() is the only thing in
         * the module that touches a local file, and swapping it is the whole
         * point of the boundary.
         */
        std::string oerr;
        r->object.reset(make_object_reader(path, fault, &oerr));
        if (!r->object)
        {
            set_err(errbuf, errbuflen, oerr);
            return nullptr;
        }
        r->object->set_interrupt_hook(g_interrupt_hook);
        r->input = std::make_shared<ArrowObjectInput>(r->object.get());

        auto preader = parquet::ParquetFileReader::Open(r->input);
        r->md = preader->metadata();

        auto st = parquet::arrow::FileReader::Make(
            arrow::default_memory_pool(), std::move(preader), &r->arrow_reader);
        if (!st.ok())
        {
            set_err(errbuf, errbuflen, st.ToString());
            return nullptr;
        }

        /* attributed_bytes counts column-chunk bytes the footer accounts for,
         * and starts at zero by construction. */
        r->attributed_bytes = 0;

        r->open_ms = now_ms() - t0;
        return r.release();
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

XpqReader *
xpq_open(const char *path, char *errbuf, size_t errbuflen)
{
    return xpq_open_internal(path, nullptr, errbuf, errbuflen);
}

XpqReader *
xpq_open_faulty(const char *path, int mode, int64_t at_read, int64_t nbytes,
                int retries, char *errbuf, size_t errbuflen)
{
    xpb::FaultSpec spec;

    switch (mode)
    {
        case 1: spec.mode = xpb::FaultMode::kShortNoEof;      break;
        case 2: spec.mode = xpb::FaultMode::kTransientFirst;  break;
        case 3: spec.mode = xpb::FaultMode::kFailAfterNBytes; break;
        case 4: spec.mode = xpb::FaultMode::kGenuineEof;      break;
        case 5: spec.mode = xpb::FaultMode::kChunked;         break;
        default: spec.mode = xpb::FaultMode::kNone;           break;
    }
    spec.at_read = at_read > 0 ? at_read : 1;
    spec.nbytes  = nbytes;
    spec.retries = retries;

    return xpq_open_internal(path, &spec, errbuf, errbuflen);
}

int64_t xpq_faulty_attempts(const XpqReader *r)
{ return (r && r->object) ? xpb::faulty_physical_attempts(r->object.get()) : -1; }
int64_t xpq_faulty_injected(const XpqReader *r)
{ return (r && r->object) ? xpb::faulty_injected(r->object.get()) : -1; }

void
xpq_set_interrupt_hook(void (*hook)(void))
{
    g_interrupt_hook = hook;
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
        r->input->start_data_phase();

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

/*
 * Straight from the ObjectReader: it is the only layer that sees a physical
 * read, so nothing here is derived. meta_calls/data_calls in particular are
 * now COUNTED -- the previous note that metadata_calls was "derived, not
 * measured" no longer applies.
 */
int64_t xpq_meta_bytes(const XpqReader *r)
{ return (r && r->object) ? r->object->meta_bytes() : 0; }
int64_t xpq_data_bytes(const XpqReader *r)
{ return (r && r->object) ? r->object->data_bytes() : 0; }
int64_t xpq_read_calls(const XpqReader *r)
{ return (r && r->object) ? r->object->read_calls() : 0; }
int64_t xpq_bytes_requested(const XpqReader *r)
{ return (r && r->object) ? r->object->bytes_requested() : 0; }
int64_t xpq_bytes_returned(const XpqReader *r)
{ return (r && r->object) ? r->object->bytes_returned() : 0; }
int64_t xpq_meta_calls(const XpqReader *r)
{ return (r && r->object) ? r->object->meta_calls() : 0; }
int64_t xpq_data_calls(const XpqReader *r)
{ return (r && r->object) ? r->object->data_calls() : 0; }
int64_t xpq_interrupt_skipped_offthread(const XpqReader *r)
{ return (r && r->object) ? r->object->interrupt_skipped_offthread() : 0; }
int64_t xpq_short_reads_without_eof(const XpqReader *r)
{ return (r && r->object) ? r->object->short_reads_without_eof() : 0; }
int64_t xpq_logical_calls(const XpqReader *r)
{ return (r && r->input) ? r->input->logical_calls() : 0; }
int64_t xpq_logical_bytes(const XpqReader *r)
{ return (r && r->input) ? r->input->logical_bytes() : 0; }
int64_t xpq_s3_head_calls(const XpqReader *r)
{ return (r && r->object) ? xpb::s3_head_calls(r->object.get()) : -1; }
int64_t xpq_s3_get_calls(const XpqReader *r)
{ return (r && r->object) ? xpb::s3_get_calls(r->object.get()) : -1; }
int64_t xpq_s3_http_errors(const XpqReader *r)
{ return (r && r->object) ? xpb::s3_http_errors(r->object.get()) : -1; }
int64_t xpq_s3_bytes_transferred(const XpqReader *r)
{ return (r && r->object) ? xpb::s3_bytes_transferred(r->object.get()) : -1; }
int64_t xpq_s3_http_attempts(const XpqReader *r)
{ return (r && r->object) ? xpb::s3_http_attempts(r->object.get()) : -1; }
int64_t xpq_s3_retries(const XpqReader *r)
{ return (r && r->object) ? xpb::s3_retries(r->object.get()) : -1; }
const char *xpq_s3_identity(const XpqReader *r)
{ return (r && r->object) ? xpb::s3_identity(r->object.get()) : ""; }
int64_t xpq_s3_identity_conflicts(const XpqReader *r)
{ return (r && r->object) ? xpb::s3_identity_conflicts(r->object.get()) : -1; }

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
