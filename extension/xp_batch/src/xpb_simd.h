/*-------------------------------------------------------------------------
 * xpb_simd.h
 *
 * SIMD helpers for LiveBitmap mask construction.
 * Uses AVX-512 (16 × int32) when available, falls back to AVX2 (8 × int32).
 *
 * Interface:
 *   lbm_build_mask_simd(values, n, op, rhs) → uint64 mask
 *
 * For each page, caller:
 *   1. Extracts int32 column values into values[] array
 *   2. Calls lbm_build_mask_simd for each predicate
 *   3. ANDs masks: live_mask &= pred_mask
 *   4. Iterates set bits: aggregate only survivors
 *-------------------------------------------------------------------------
 */
#ifndef XPB_SIMD_H
#define XPB_SIMD_H

#include <stdint.h>
#include <immintrin.h>

#include "xpb_groupagg2_types.h"  /* PQOp */

/*
 * Build a bitmask of up to 64 positions where pred(values[i], rhs) is true.
 * Uses AVX-512 vpcmpd: processes 16 int32 per cycle.
 */
static inline uint64_t
lbm_build_mask_avx512(const int32_t *values, int n, PQOp op, int32_t rhs)
{
    __m512i vrhs = _mm512_set1_epi32(rhs);
    uint64_t mask = 0;
    int i = 0;

    /* Process 16 elements at a time */
    for (; i + 16 <= n; i += 16)
    {
        __m512i vv  = _mm512_loadu_si512((const __m512i *)(values + i));
        __mmask16 m;
        switch (op)
        {
            case PQ_LT: m = _mm512_cmplt_epi32_mask(vv, vrhs); break;
            case PQ_LE: m = _mm512_cmple_epi32_mask(vv, vrhs); break;
            case PQ_EQ: m = _mm512_cmpeq_epi32_mask(vv, vrhs); break;
            case PQ_GE: m = _mm512_cmpge_epi32_mask(vv, vrhs); break;
            case PQ_GT: m = _mm512_cmpgt_epi32_mask(vv, vrhs); break;
            default:    m = 0xFFFF; break;
        }
        mask |= ((uint64_t)(uint16_t)m) << i;
    }

    /* Scalar tail (< 16 remaining) */
    for (; i < n; i++)
    {
        bool ok;
        switch (op)
        {
            case PQ_LT: ok = values[i] <  rhs; break;
            case PQ_LE: ok = values[i] <= rhs; break;
            case PQ_EQ: ok = values[i] == rhs; break;
            case PQ_GE: ok = values[i] >= rhs; break;
            case PQ_GT: ok = values[i] >  rhs; break;
            default:    ok = true;              break;
        }
        if (ok) mask |= (uint64_t)1 << i;
    }
    return mask;
}

/* AVX2 fallback: processes 8 int32 per cycle */
static inline uint64_t
lbm_build_mask_avx2(const int32_t *values, int n, PQOp op, int32_t rhs)
{
    __m256i vrhs = _mm256_set1_epi32(rhs);
    uint64_t mask = 0;
    int i = 0;

    for (; i + 8 <= n; i += 8)
    {
        __m256i vv = _mm256_loadu_si256((const __m256i *)(values + i));
        __m256i cmp;
        switch (op)
        {
            case PQ_LT: cmp = _mm256_cmpgt_epi32(vrhs, vv);      break; /* rhs > val ≡ val < rhs */
            case PQ_LE: cmp = _mm256_or_si256(
                                _mm256_cmpgt_epi32(vrhs, vv),
                                _mm256_cmpeq_epi32(vv,   vrhs));  break;
            case PQ_EQ: cmp = _mm256_cmpeq_epi32(vv, vrhs);      break;
            case PQ_GE: cmp = _mm256_or_si256(
                                _mm256_cmpgt_epi32(vv, vrhs),
                                _mm256_cmpeq_epi32(vv, vrhs));    break;
            case PQ_GT: cmp = _mm256_cmpgt_epi32(vv, vrhs);      break;
            default:    cmp = _mm256_set1_epi32(-1);              break;
        }
        uint32_t bits = (uint32_t)_mm256_movemask_ps(
                            _mm256_castsi256_ps(cmp));
        /* movemask gives one bit per float (4 bytes = one int32) */
        mask |= ((uint64_t)(uint8_t)bits) << i;
    }

    /* Scalar tail */
    for (; i < n; i++)
    {
        bool ok;
        switch (op)
        {
            case PQ_LT: ok = values[i] <  rhs; break;
            case PQ_LE: ok = values[i] <= rhs; break;
            case PQ_EQ: ok = values[i] == rhs; break;
            case PQ_GE: ok = values[i] >= rhs; break;
            case PQ_GT: ok = values[i] >  rhs; break;
            default:    ok = true;              break;
        }
        if (ok) mask |= (uint64_t)1 << i;
    }
    return mask;
}

/* Forward: LiveBitmap struct (defined in xpb_groupagg2.c, repeated here) */
#ifndef XPB_PAGE_TUPLES_MAX
#define XPB_PAGE_TUPLES_MAX 256
#define XPB_BM_WORDS  ((XPB_PAGE_TUPLES_MAX + 63) / 64)
typedef struct LiveBitmap {
    uint64_t words[XPB_BM_WORDS];
    int ntuples;
    int nsurvivors;
} LiveBitmap;
#endif

/*
 * Build full LiveBitmap for up to XPB_PAGE_TUPLES_MAX tuples.
 * Fills bm->words[] and bm->nsurvivors.
 * Called once per predicate per page; caller ANDs results.
 */
static inline void
lbm_build_full_mask_avx512(LiveBitmap *bm, const int32_t *values,
                            int n, PQOp op, int32_t rhs)
{
    int survivors = 0;
    for (int w = 0; w < XPB_BM_WORDS; w++)
    {
        int base  = w * 64;
        if (base >= n) { bm->words[w] = 0; continue; }
        int chunk = (n - base < 64) ? n - base : 64;
        uint64_t mask = lbm_build_mask_avx512(values + base, chunk, op, rhs);
        bm->words[w]  = mask;
        survivors    += __builtin_popcountll(mask);
    }
    bm->nsurvivors = survivors;
}

/* AND another predicate's mask into bm */
static inline void
lbm_and_mask_avx512(LiveBitmap *bm, const int32_t *values,
                    int n, PQOp op, int32_t rhs)
{
    int survivors = 0;
    for (int w = 0; w < XPB_BM_WORDS; w++)
    {
        if (!bm->words[w]) continue;   /* already all-zero, skip */
        int base  = w * 64;
        if (base >= n) { bm->words[w] = 0; continue; }
        int chunk = (n - base < 64) ? n - base : 64;
        uint64_t pred = lbm_build_mask_avx512(values + base, chunk, op, rhs);
        bm->words[w] &= pred;
        survivors    += __builtin_popcountll(bm->words[w]);
    }
    bm->nsurvivors = survivors;
}

#endif /* XPB_SIMD_H */
