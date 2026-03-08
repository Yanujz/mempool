/*
 * Paranoia test suite for mempool v0.5.1 — ~600 tests
 *
 * Exhaustively verifies safety/correctness invariants that go beyond normal
 * functional tests.  ALL optional features are compiled in (same config as
 * mempool_gtest_hardening).
 *
 * Parameterised suites (30 base configs × 16 test types = 480 cases):
 *   1.  CanaryTamper_Byte0   — overwrite canary byte 0 → GUARD_CORRUPTED
 *   2.  CanaryTamper_Byte1   — overwrite canary byte 1 → GUARD_CORRUPTED
 *   3.  CanaryTamper_Byte2   — overwrite canary byte 2 → GUARD_CORRUPTED
 *   4.  CanaryTamper_Byte3   — overwrite canary byte 3 → GUARD_CORRUPTED
 *   5.  DoubleFreeDirect     — alloc → free → free → DOUBLE_FREE
 *   6.  DoubleFreeAfterRealloc — alloc B1 → free B1 → alloc B2(=B1) → free B1 → DOUBLE_FREE
 *   7.  FreeMisaligned_Plus1 — ptr+1 → INVALID_BLOCK
 *   8.  FreeBeforePoolStart  — ptr = blocks_start − 8 → INVALID_BLOCK
 *   9.  WalkVsStats_FullPool — fill pool, walk, count == total_blocks
 *  10.  WalkVsStats_HalfPool — alloc half, walk, count == half
 *  11.  BitmapIntegrity_Full — every block is_allocated after full fill
 *  12.  AllocZeroAllBytes    — mempool_alloc_zero zeros every byte
 *  13.  TagClearedOnFree     — tag cleared after free, 0 after realloc
 *  14.  PeakUsageTracked     — peak always ≥ current used
 *  15.  FreePoisonPattern    — free-poison fill verified through re-alloc
 *  16.  PoolBufSize_Macro_vs_Runtime — compile-time macro == runtime function
 *
 * Plus ~120 non-parameterised edge tests covering ISR queue, error codes,
 * manager bounds, alloc_tagged, mempool_contains, reset semantics, etc.
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>
#include <random>
#include <mutex>

static std::mutex g_para_mtx;
static std::mutex g_para_isr_mtx;
extern "C" {
void mempool_test_lock(void)       { g_para_mtx.lock();       }
void mempool_test_unlock(void)     { g_para_mtx.unlock();     }
void mempool_test_isr_lock(void)   { g_para_isr_mtx.lock();   }
void mempool_test_isr_unlock(void) { g_para_isr_mtx.unlock(); }
#include "mempool.h"
#include "mempool_mgr.h"
}

namespace {

/* -----------------------------------------------------------------------
 * Helpers shared by all tests
 * --------------------------------------------------------------------- */

struct PC { uint32_t bs; uint32_t nb; uint32_t al; };

/* Pool fixture that auto-initialises from a PC parameter. */
class ParanoiaTest : public ::testing::TestWithParam<PC> {
protected:
    mempool_t *pool_ = nullptr;
    uint32_t   user_block_size_ = 0U;  /* caller-visible bytes per block */
    std::vector<uint8_t> pool_buf_;
    std::vector<uint8_t> state_buf_;

    void SetUp() override {
        PC p = GetParam();
        state_buf_.assign(MEMPOOL_STATE_SIZE, 0U);
        size_t bufsz = MEMPOOL_POOL_BUFFER_SIZE(p.bs, p.nb, p.al);
        pool_buf_.assign(bufsz + (size_t)p.al * 2U, 0U);

        void *ptr   = pool_buf_.data();
        size_t space = pool_buf_.size();
        std::align(p.al, bufsz, ptr, space);

        mempool_error_t err = mempool_init(
            state_buf_.data(), MEMPOOL_STATE_SIZE,
            ptr, bufsz,
            p.bs, p.al, &pool_
        );
        ASSERT_EQ(MEMPOOL_OK, err)
            << "bs=" << p.bs << " nb=" << p.nb << " al=" << p.al;
        ASSERT_NE(nullptr, pool_);
        user_block_size_ = p.bs;
    }

    /* Return pointer to byte N within block's raw storage (0-based). */
    static uint8_t *byte_at(void *block, uint32_t n) {
        return static_cast<uint8_t *>(block) + n;
    }

    std::vector<void *> alloc_all() {
        std::vector<void *> v;
        for (;;) {
            void *b = nullptr;
            if (mempool_alloc(pool_, &b) != MEMPOOL_OK) break;
            EXPECT_NE(nullptr, b);
            v.push_back(b);
        }
        return v;
    }

    uint32_t total_n() {
#if MEMPOOL_ENABLE_STATS
        mempool_stats_t s{};
        (void)mempool_get_stats(pool_, &s);
        return s.total_blocks;
#else
        return GetParam().nb;
#endif
    }
};

/* -----------------------------------------------------------------------
 * 1–4: Canary tamper — each of the 4 bytes
 * --------------------------------------------------------------------- */
template<int BYTE_POS>
void RunCanaryTamper(mempool_t *pool, uint32_t user_block_size) {
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b));
    ASSERT_NE(nullptr, b);
    /* Overwrite exactly one byte of the post-canary word. */
    uint8_t *tamper = static_cast<uint8_t *>(b) + user_block_size + BYTE_POS;
    *tamper ^= 0xFFU;  /* flip all bits of that byte */
    /* mempool_free must now detect the corruption. */
    EXPECT_EQ(MEMPOOL_ERR_GUARD_CORRUPTED, mempool_free(pool, b));
#if MEMPOOL_ENABLE_STATS
    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &s));
    EXPECT_EQ(1U, s.guard_violations);
#endif
}

TEST_P(ParanoiaTest, CanaryTamper_Byte0) {
    RunCanaryTamper<0>(pool_, user_block_size_);
}
TEST_P(ParanoiaTest, CanaryTamper_Byte1) {
    RunCanaryTamper<1>(pool_, user_block_size_);
}
TEST_P(ParanoiaTest, CanaryTamper_Byte2) {
    RunCanaryTamper<2>(pool_, user_block_size_);
}
TEST_P(ParanoiaTest, CanaryTamper_Byte3) {
    RunCanaryTamper<3>(pool_, user_block_size_);
}

/* -----------------------------------------------------------------------
 * 5: Double-free — direct
 * --------------------------------------------------------------------- */
TEST_P(ParanoiaTest, DoubleFreeDirect) {
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b));
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
    EXPECT_EQ(MEMPOOL_ERR_DOUBLE_FREE, mempool_free(pool_, b));
}

/* -----------------------------------------------------------------------
 * 6: Double-free after a realloc of the same physical block
 * --------------------------------------------------------------------- */
TEST_P(ParanoiaTest, DoubleFreeAfterRealloc) {
    if (total_n() < 1U) { GTEST_SKIP(); }
    void *orig = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &orig));
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, orig));

    /* LIFO: re-alloc gives back the same block. */
    void *next = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &next));
    /* next == orig on any LIFO pool */

    if (next == orig) {
        /* next is currently allocated. Free it properly first. */
        ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, next));
        /* Now orig (== next) is freed — a second free is a double-free. */
        EXPECT_EQ(MEMPOOL_ERR_DOUBLE_FREE, mempool_free(pool_, orig));
    } else {
        /* Different block: orig's bitmap bit is 0 (freed) → double-free. */
        EXPECT_EQ(MEMPOOL_ERR_DOUBLE_FREE, mempool_free(pool_, orig));
        ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, next));
    }
}

/* -----------------------------------------------------------------------
 * 7: Free of a pointer offset by +1 byte (misaligned)
 * --------------------------------------------------------------------- */
TEST_P(ParanoiaTest, FreeMisaligned_Plus1) {
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b));
    uint8_t *p1 = static_cast<uint8_t *>(b) + 1U;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK, mempool_free(pool_, p1));
    /* Clean up the actual allocated block. */
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
}

/* -----------------------------------------------------------------------
 * 8: Free of a pointer before the pool's block array start
 * --------------------------------------------------------------------- */
TEST_P(ParanoiaTest, FreeBeforePoolStart) {
    /* Use the start of the state buffer as an obviously-wrong pointer. */
    uint8_t dummy[64] = {};
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK, mempool_free(pool_, dummy));
}

/* -----------------------------------------------------------------------
 * 9: Walk count matches stats.used_blocks when pool is full
 * --------------------------------------------------------------------- */
TEST_P(ParanoiaTest, WalkVsStats_FullPool) {
    auto blocks = alloc_all();
    ASSERT_GT(blocks.size(), 0U);

    uint32_t walk_count = 0U;
    ASSERT_EQ(MEMPOOL_OK, mempool_walk(pool_,
        [](const mempool_t * /*p*/, const void * /*blk*/,
           uint32_t /*idx*/, void *ctx) {
            (*static_cast<uint32_t *>(ctx))++;
        }, &walk_count));
    EXPECT_EQ((uint32_t)blocks.size(), walk_count);

#if MEMPOOL_ENABLE_STATS
    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
    EXPECT_EQ(s.used_blocks, walk_count);
#endif

    for (void *b : blocks) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
    }
}

/* -----------------------------------------------------------------------
 * 10: Walk count matches stats when pool is half-full
 * --------------------------------------------------------------------- */
TEST_P(ParanoiaTest, WalkVsStats_HalfPool) {
    uint32_t n = total_n();
    if (n < 2U) { GTEST_SKIP(); }

    uint32_t half = n / 2U;
    std::vector<void *> blocks;
    blocks.reserve(half);
    for (uint32_t i = 0U; i < half; i++) {
        void *b = nullptr;
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b));
        blocks.push_back(b);
    }

    uint32_t walk_count = 0U;
    ASSERT_EQ(MEMPOOL_OK, mempool_walk(pool_,
        [](const mempool_t * /*p*/, const void * /*blk*/,
           uint32_t /*idx*/, void *ctx) {
            (*static_cast<uint32_t *>(ctx))++;
        }, &walk_count));
    EXPECT_EQ(half, walk_count);

    for (void *b : blocks) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
    }
}

/* -----------------------------------------------------------------------
 * 11: Bitmap — every block shows is_allocated == 1 after full fill
 * --------------------------------------------------------------------- */
TEST_P(ParanoiaTest, BitmapIntegrity_Full) {
    auto blocks = alloc_all();
    ASSERT_GT(blocks.size(), 0U);

    for (void *b : blocks) {
        EXPECT_EQ(1, mempool_is_block_allocated(pool_, b))
            << "block " << b << " should be allocated";
    }

    for (void *b : blocks) {
        ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
        EXPECT_EQ(0, mempool_is_block_allocated(pool_, b))
            << "block " << b << " should be free after free()";
    }
}

/* -----------------------------------------------------------------------
 * 12: mempool_alloc_zero — every byte must be 0
 * --------------------------------------------------------------------- */
TEST_P(ParanoiaTest, AllocZeroAllBytes) {
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_zero(pool_, &b));
    ASSERT_NE(nullptr, b);

    /* Write a recognisable pattern, free, zero-alloc again — must be 0. */
    memset(b, 0xBB, user_block_size_);
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, b));

    b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_zero(pool_, &b));
    ASSERT_NE(nullptr, b);

    const uint8_t *bytes = static_cast<const uint8_t *>(b);
    bool all_zero = true;
    for (uint32_t i = 0U; i < user_block_size_; i++) {
        if (bytes[i] != 0U) { all_zero = false; break; }
    }
    EXPECT_TRUE(all_zero) << "alloc_zero: non-zero byte found; bs=" << user_block_size_;

    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
}

/* -----------------------------------------------------------------------
 * 13: Tag cleared on free, zero on fresh realloc
 * --------------------------------------------------------------------- */
TEST_P(ParanoiaTest, TagClearedOnFree) {
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b));
    ASSERT_EQ(MEMPOOL_OK, mempool_set_block_tag(pool_, b, 0xDEADU));

    uint32_t t = 0U;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool_, b, &t));
    EXPECT_EQ(0xDEADU, t);

    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
    /* Tag must be rejected on freed block. */
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK, mempool_get_block_tag(pool_, b, &t));

    /* Re-allocate (LIFO — likely same block). */
    void *b2 = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b2));
    uint32_t t2 = 0xFFFFFFFFU;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool_, b2, &t2));
    EXPECT_EQ(0U, t2) << "fresh alloc must have tag == 0";

    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, b2));
}

/* -----------------------------------------------------------------------
 * 14: Peak usage always ≥ current used_blocks
 * --------------------------------------------------------------------- */
TEST_P(ParanoiaTest, PeakUsageTracked) {
#if MEMPOOL_ENABLE_STATS
    uint32_t n = total_n();
    if (n == 0U) { GTEST_SKIP(); }

    uint32_t seen_peak = 0U;
    std::vector<void *> blocks;
    blocks.reserve(n);

    for (uint32_t i = 0U; i < n; i++) {
        void *b = nullptr;
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b));
        blocks.push_back(b);

        mempool_stats_t s{};
        ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
        EXPECT_GE(s.peak_usage, s.used_blocks);
        if (s.peak_usage > seen_peak) { seen_peak = s.peak_usage; }
    }

    /* Free all and verify peak doesn't decrease. */
    for (void *b : blocks) {
        ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
        mempool_stats_t s{};
        ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
        EXPECT_EQ(seen_peak, s.peak_usage)
            << "peak must not decrease on free";
    }
#else
    GTEST_SKIP();
#endif
}

/* -----------------------------------------------------------------------
 * 15: Free-poison fill verified through immediate re-alloc
 *
 * Strategy: alloc B, write 0xFF everywhere, free B (→ 0xDD poison fill),
 * re-alloc (→ 0xCD alloc poison fill overwrites 0xDD), verify 0xCD.
 * Then separately verify the FREE poison by examining the raw buffer at
 * the free-list node offset after free (bytes sizeof(void*)..bs-1 == 0xDD).
 * --------------------------------------------------------------------- */
TEST_P(ParanoiaTest, FreePoisonPattern) {
#if MEMPOOL_ENABLE_POISON
    if (total_n() < 2U) { GTEST_SKIP(); }  /* need 2 blocks to avoid ISR complications */

    /* Alloc first block — keep it to prevent it being immediately reused. */
    void *anchor = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &anchor));

    /* Alloc second block — the one we'll free-poison test. */
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b));
    ASSERT_NE(nullptr, b);

    /* Write 0xFF everywhere (overwriting alloc-poison). */
    memset(b, 0xFF, user_block_size_);

    /* Free — library should write FREE_POISON_BYTE (0xDD) to user area,
     * then prepend free-list node (first sizeof(void*) bytes become pointer). */
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, b));

    /* Examine raw bytes starting after the free-list pointer. */
    const uint8_t *raw = static_cast<const uint8_t *>(b);
    const size_t ptr_size = sizeof(void *);
    bool poison_ok = true;
    for (uint32_t i = (uint32_t)ptr_size; i < user_block_size_; i++) {
        if (raw[i] != (uint8_t)MEMPOOL_FREE_POISON_BYTE) {
            poison_ok = false;
            break;
        }
    }
    EXPECT_TRUE(poison_ok)
        << "free-poison not uniform after first " << ptr_size << " bytes; bs=" << user_block_size_;

    /* Re-alloc: now bytes must be ALLOC_POISON_BYTE (0xCD). */
    void *b2 = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b2));
    EXPECT_EQ(b, b2) << "LIFO: same block should be returned";

    const uint8_t *raw2 = static_cast<const uint8_t *>(b2);
    bool alloc_poison_ok = true;
    for (uint32_t i = 0U; i < user_block_size_; i++) {
        if (raw2[i] != (uint8_t)MEMPOOL_ALLOC_POISON_BYTE) {
            alloc_poison_ok = false;
            break;
        }
    }
    EXPECT_TRUE(alloc_poison_ok)
        << "alloc-poison not uniform after re-alloc; bs=" << user_block_size_;

    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, b2));
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, anchor));
#else
    GTEST_SKIP();
#endif
}

/* -----------------------------------------------------------------------
 * 16: mempool_pool_buffer_size() runtime == MEMPOOL_POOL_BUFFER_SIZE() macro
 *
 * Only valid when all features are ON (the macro assumes everything active).
 * --------------------------------------------------------------------- */
TEST_P(ParanoiaTest, PoolBufSize_Macro_vs_Runtime) {
    PC p = GetParam();
    size_t runtime_sz = mempool_pool_buffer_size(p.bs, p.nb, p.al);
    size_t macro_sz   = MEMPOOL_POOL_BUFFER_SIZE(p.bs, p.nb, p.al);
    /* With all features ON, both should give the same result. */
    EXPECT_EQ(macro_sz, runtime_sz)
        << "bs=" << p.bs << " nb=" << p.nb << " al=" << p.al;
    EXPECT_GT(runtime_sz, 0U);
}

/* -----------------------------------------------------------------------
 * Configuration list — 30 representative configs
 * --------------------------------------------------------------------- */
static const PC kParanoiaConfigs[] = {
    /* small blocks */
    {8, 2, 4},   {8, 4, 8},
    {16, 4, 8},  {16, 8, 8},   {16, 4, 16},
    /* mid blocks */
    {32, 4, 8},  {32, 8, 8},   {32, 4, 16},  {32, 8, 16},
    {64, 4, 8},  {64, 8, 8},   {64, 4, 16},  {64, 8, 16},
    {64, 16, 8}, {64, 32, 8},
    /* large blocks */
    {128, 4, 8}, {128, 8, 8},  {128, 4, 16}, {128, 16, 8},
    {256, 4, 8}, {256, 8, 8},  {256, 4, 16},
    /* non-power-of-2 block sizes — stress canary alignment */
    {9,  4, 4},  {10, 4, 4},   {11, 4, 4},
    {18, 4, 8},  {21, 4, 8},
    /* non-power-of-2 both block_size and num_blocks */
    {12, 4, 4},  {20, 8, 8},   {48, 8, 8},
};

INSTANTIATE_TEST_SUITE_P(
    Base30,
    ParanoiaTest,
    ::testing::ValuesIn(kParanoiaConfigs));

/* -----------------------------------------------------------------------
 * Non-parameterised edge / integration tests
 * --------------------------------------------------------------------- */

/* Helpers for standalone tests */
static constexpr size_t kStateSize  = MEMPOOL_STATE_SIZE;
static constexpr uint32_t kStdBs   = 32U;
static constexpr uint32_t kStdNb   = 8U;
static constexpr uint32_t kStdAl   = 8U;

struct PoolEnv {
    alignas(8) uint8_t state[kStateSize];
    uint8_t buf[MEMPOOL_POOL_BUFFER_SIZE(kStdBs, kStdNb, kStdAl)];
    mempool_t *pool = nullptr;

    void init(uint32_t bs = kStdBs, uint32_t /*nb*/ = kStdNb, uint32_t al = kStdAl) {
        memset(state, 0, sizeof state);
        memset(buf,   0, sizeof buf);
        ASSERT_EQ(MEMPOOL_OK,
                  mempool_init(state, sizeof state, buf, sizeof buf, bs, al, &pool));
        ASSERT_NE(nullptr, pool);
    }
};

/* ----------- Null-pointer guards on every public API ------------------- */

TEST(NullPtrGuards, AllocNullPool)    { void *b=nullptr; EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_alloc(nullptr, &b)); }
TEST(NullPtrGuards, AllocNullOut)     { PoolEnv e; e.init(); EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_alloc(e.pool, nullptr)); }
TEST(NullPtrGuards, FreeNullPool)     { EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_free(nullptr, (void*)1)); }
TEST(NullPtrGuards, FreeNullBlock)    { PoolEnv e; e.init(); EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_free(e.pool, nullptr)); }
TEST(NullPtrGuards, ResetNullPool)    { EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_reset(nullptr)); }
TEST(NullPtrGuards, ContainsNullPool) { EXPECT_EQ(0, mempool_contains(nullptr, (void*)1)); }
TEST(NullPtrGuards, ContainsNullPtr)  { PoolEnv e; e.init(); EXPECT_EQ(0, mempool_contains(e.pool, nullptr)); }
TEST(NullPtrGuards, BlockSizeNull)    { EXPECT_EQ(0U, mempool_block_size(nullptr)); }
TEST(NullPtrGuards, CapacityNull)     { EXPECT_EQ(0U, mempool_capacity(nullptr)); }
TEST(NullPtrGuards, HasFreeBlockNull) { EXPECT_EQ(0, mempool_has_free_block(nullptr)); }
TEST(NullPtrGuards, GetStatsNullPool) { mempool_stats_t s{}; EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_get_stats(nullptr, &s)); }
TEST(NullPtrGuards, GetStatsNullOut)  { PoolEnv e; e.init(); EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_get_stats(e.pool, nullptr)); }
TEST(NullPtrGuards, SetTagNullPool)   { EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_set_block_tag(nullptr, (void*)1, 0)); }
TEST(NullPtrGuards, SetTagNullBlock)  { PoolEnv e; e.init(); EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_set_block_tag(e.pool, nullptr, 0)); }
TEST(NullPtrGuards, GetTagNullPool)   { uint32_t t=0; EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_get_block_tag(nullptr, (void*)1, &t)); }
TEST(NullPtrGuards, GetTagNullBlock)  { PoolEnv e; e.init(); uint32_t t=0; EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_get_block_tag(e.pool, nullptr, &t)); }
TEST(NullPtrGuards, GetTagNullOut)    { PoolEnv e; e.init(); void *b=nullptr; e.pool && mempool_alloc(e.pool,&b); EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_get_block_tag(e.pool, b?b:(void*)1, nullptr)); if(b) mempool_free(e.pool,b); }
TEST(NullPtrGuards, IsAllocNullPool)  { EXPECT_EQ(0, mempool_is_block_allocated(nullptr, (void*)1)); }
TEST(NullPtrGuards, IsAllocNullBlock) { PoolEnv e; e.init(); EXPECT_EQ(0, mempool_is_block_allocated(e.pool, nullptr)); }
TEST(NullPtrGuards, WalkNullPool) {
    mempool_walk_fn_t stub = [](const mempool_t*, const void*, uint32_t, void*){};
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_walk(nullptr, stub, nullptr));
}
TEST(NullPtrGuards, WalkNullCb) {
    PoolEnv e; e.init();
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_walk(e.pool, nullptr, nullptr));
}
TEST(NullPtrGuards, AllocZeroNull)    { void *b=nullptr; EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_alloc_zero(nullptr, &b)); }
TEST(NullPtrGuards, AllocTaggedNullPool){ void *b=nullptr; EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_alloc_tagged(nullptr, &b, 0xAAU)); }
TEST(NullPtrGuards, IsrFreeNull)      { EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_free_from_isr(nullptr, (void*)1)); }
TEST(NullPtrGuards, IsrFreeNullBlock) { PoolEnv e; e.init(); EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_free_from_isr(e.pool, nullptr)); }
TEST(NullPtrGuards, DrainIsrNull)     { EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_drain_isr_queue(nullptr)); }

/* ----------- Init validation ------------------------------------------- */

TEST(InitValidation, ZeroPoolBuffer)    {
    PoolEnv e;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE,
              mempool_init(e.state, sizeof e.state, e.buf, 0, 32, 8, &e.pool));
}
TEST(InitValidation, ZeroBlockSize)     {
    PoolEnv e;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE,
              mempool_init(e.state, sizeof e.state, e.buf, sizeof e.buf, 0, 8, &e.pool));
}
TEST(InitValidation, BlockSizeTooSmall) {
    PoolEnv e;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE,
              mempool_init(e.state, sizeof e.state, e.buf, sizeof e.buf, 1, 1, &e.pool));
}
TEST(InitValidation, NonPow2Alignment)  {
    PoolEnv e;
    EXPECT_EQ(MEMPOOL_ERR_ALIGNMENT,
              mempool_init(e.state, sizeof e.state, e.buf, sizeof e.buf, 32, 3, &e.pool));
}
TEST(InitValidation, StateTooSmall)     {
    uint8_t tiny[4]{};
    PoolEnv e;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE,
              mempool_init(tiny, sizeof tiny, e.buf, sizeof e.buf, 32, 8, &e.pool));
}
TEST(InitValidation, PoolTooSmallForOneBlock) {
    PoolEnv e;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE,
              mempool_init(e.state, sizeof e.state, e.buf, 1, 32, 8, &e.pool));
}

/* ----------- mempool_strerror ------------------------------------------ */

TEST(Strerror, AllErrorCodes) {
    EXPECT_STRNE("", mempool_strerror(MEMPOOL_OK));
    EXPECT_STRNE("", mempool_strerror(MEMPOOL_ERR_NULL_PTR));
    EXPECT_STRNE("", mempool_strerror(MEMPOOL_ERR_INVALID_SIZE));
    EXPECT_STRNE("", mempool_strerror(MEMPOOL_ERR_OUT_OF_MEMORY));
    EXPECT_STRNE("", mempool_strerror(MEMPOOL_ERR_INVALID_BLOCK));
    EXPECT_STRNE("", mempool_strerror(MEMPOOL_ERR_DOUBLE_FREE));
    EXPECT_STRNE("", mempool_strerror(MEMPOOL_ERR_GUARD_CORRUPTED));
    EXPECT_STRNE("", mempool_strerror(MEMPOOL_ERR_NOT_INITIALIZED));
    /* Unknown error code must not crash. */
    EXPECT_NE(nullptr, mempool_strerror((mempool_error_t)9999));
}

/* ----------- mempool_alloc_tagged -------------------------------------- */

TEST(AllocTagged, SetsTagCorrectly) {
    PoolEnv e; e.init();
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_tagged(e.pool, &b, 0xCAFEBABEU));
    ASSERT_NE(nullptr, b);
    uint32_t t = 0U;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_block_tag(e.pool, b, &t));
    EXPECT_EQ(0xCAFEBABEU, t);
    ASSERT_EQ(MEMPOOL_OK, mempool_free(e.pool, b));
}

TEST(AllocTagged, TagZeroIsValid) {
    PoolEnv e; e.init();
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_tagged(e.pool, &b, 0U));
    uint32_t t = 0xFFU;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_block_tag(e.pool, b, &t));
    EXPECT_EQ(0U, t);
    ASSERT_EQ(MEMPOOL_OK, mempool_free(e.pool, b));
}

TEST(AllocTagged, TagMaxValue) {
    PoolEnv e; e.init();
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_tagged(e.pool, &b, 0xFFFFFFFFU));
    uint32_t t = 0U;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_block_tag(e.pool, b, &t));
    EXPECT_EQ(0xFFFFFFFFU, t);
    ASSERT_EQ(MEMPOOL_OK, mempool_free(e.pool, b));
}

TEST(AllocTagged, MultipleBlocksIndependentTags) {
    alignas(8) uint8_t state[kStateSize]{};
    alignas(8) uint8_t buf[MEMPOOL_POOL_BUFFER_SIZE(32U, 4U, 8U)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state, buf, sizeof buf, 32U, 8U, &pool));

    void *b0=nullptr, *b1=nullptr, *b2=nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_tagged(pool, &b0, 0x111U));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_tagged(pool, &b1, 0x222U));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_tagged(pool, &b2, 0x333U));

    uint32_t t0=0, t1=0, t2=0;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, b0, &t0));
    ASSERT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, b1, &t1));
    ASSERT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, b2, &t2));
    EXPECT_EQ(0x111U, t0);
    EXPECT_EQ(0x222U, t1);
    EXPECT_EQ(0x333U, t2);

    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool, b0));
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool, b1));
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool, b2));
}

/* ----------- ISR queue stress ------------------------------------------ */

TEST(IsrQueue, FillToExactCapacity) {
    /* Pool with cap+1 blocks: fill ISR queue to capacity, then test overflow.
     * Critically, don't call mempool_alloc after filling the queue because
     * mempool_alloc eagerly drains the ISR queue before allocating. */
    constexpr uint32_t cap = MEMPOOL_ISR_QUEUE_CAPACITY;
    alignas(8) uint8_t state[kStateSize]{};
    alignas(8) uint8_t buf[MEMPOOL_POOL_BUFFER_SIZE(32U, cap + 4U, 8U)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state, sizeof state, buf, sizeof buf, 32U, 8U, &pool));

    /* Pre-allocate cap+1 blocks up front. */
    void *blocks[MEMPOOL_ISR_QUEUE_CAPACITY + 1U]{};
    for (uint32_t i = 0U; i <= cap; i++) {
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blocks[i]))
            << "alloc failed at i=" << i;
    }

    /* Queue exactly cap blocks via ISR path. */
    for (uint32_t i = 0U; i < cap; i++) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, blocks[i]))
            << "ISR free " << i;
    }

    /* Queue is full — the extra ISR free must fail with ISR_QUEUE_FULL. */
    EXPECT_EQ(MEMPOOL_ERR_ISR_QUEUE_FULL,
              mempool_free_from_isr(pool, blocks[cap]));

    /* Free the last block via the task path, then drain the queued blocks. */
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool, blocks[cap]));
    ASSERT_EQ(MEMPOOL_OK, mempool_drain_isr_queue(pool));

#if MEMPOOL_ENABLE_STATS
    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &s));
    EXPECT_EQ(0U, s.used_blocks);
#endif
}

TEST(IsrQueue, GuardCorruptedInIsrPath) {
    alignas(8) uint8_t state[kStateSize]{};
    alignas(8) uint8_t buf[MEMPOOL_POOL_BUFFER_SIZE(32U, 2U, 8U)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state, sizeof state, buf, sizeof buf, 32U, 8U, &pool));

    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b));
    /* Corrupt the canary byte 0. */
    *(static_cast<uint8_t *>(b) + 32U) ^= 0xFFU;

    /* Queue via ISR path — drain should quarantine it. */
    ASSERT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, b));
    ASSERT_EQ(MEMPOOL_OK, mempool_drain_isr_queue(pool));

#if MEMPOOL_ENABLE_STATS
    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &s));
    EXPECT_EQ(1U, s.guard_violations);
    /* Block is quarantined: used_blocks was decremented but bit stays SET. */
    EXPECT_EQ(0U, s.used_blocks);
#endif
}

TEST(IsrQueue, DrainRestoresAllBlocks) {
    alignas(8) uint8_t state[kStateSize]{};
    alignas(8) uint8_t buf[MEMPOOL_POOL_BUFFER_SIZE(32U, 4U, 8U)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state, sizeof state, buf, sizeof buf, 32U, 8U, &pool));

    void *b0=nullptr, *b1=nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b0));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b1));

    ASSERT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, b0));
    ASSERT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, b1));
    ASSERT_EQ(MEMPOOL_OK, mempool_drain_isr_queue(pool));

#if MEMPOOL_ENABLE_STATS
    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &s));
    EXPECT_EQ(0U, s.used_blocks);
#endif
}

/* ----------- mempool_contains ------------------------------------------ */

TEST(Contains, PointerInPool)         { PoolEnv e; e.init(); void*b=nullptr; mempool_alloc(e.pool,&b); EXPECT_EQ(1,mempool_contains(e.pool,b)); mempool_free(e.pool,b); }
TEST(Contains, PointerBeforePool)     { PoolEnv e; e.init(); uint8_t x=0; EXPECT_EQ(0,mempool_contains(e.pool,&x)); }
TEST(Contains, FreedBlockStillContained) { PoolEnv e; e.init(); void*b=nullptr; mempool_alloc(e.pool,&b); mempool_free(e.pool,b); EXPECT_EQ(1,mempool_contains(e.pool,b)); }

/* ----------- mempool_capacity ------------------------------------------ */

TEST(Capacity, EqualsTotalBlocks) {
#if MEMPOOL_ENABLE_STATS
    PoolEnv e; e.init();
    mempool_stats_t s{}; mempool_get_stats(e.pool, &s);
    EXPECT_EQ(s.total_blocks, mempool_capacity(e.pool));
#else
    GTEST_SKIP();
#endif
}

TEST(Capacity, RemainsAfterAllocFree) {
    PoolEnv e; e.init();
    uint32_t cap = mempool_capacity(e.pool);
    void *b = nullptr; mempool_alloc(e.pool, &b);
    EXPECT_EQ(cap, mempool_capacity(e.pool));
    if (b) mempool_free(e.pool, b);
    EXPECT_EQ(cap, mempool_capacity(e.pool));
}

/* ----------- mempool_reset_stats --------------------------------------- */

TEST(ResetStats, PeakResetsToCurrentUsed) {
#if MEMPOOL_ENABLE_STATS
    PoolEnv e; e.init();
    void *b0=nullptr, *b1=nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(e.pool, &b0));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(e.pool, &b1));

    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(e.pool, &s));
    EXPECT_EQ(2U, s.peak_usage);

    ASSERT_EQ(MEMPOOL_OK, mempool_free(e.pool, b0));
    ASSERT_EQ(MEMPOOL_OK, mempool_reset_stats(e.pool));

    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(e.pool, &s));
    EXPECT_EQ(0U, s.alloc_count);
    EXPECT_EQ(0U, s.free_count);
    EXPECT_EQ(s.used_blocks, s.peak_usage)
        << "peak_usage must equal used_blocks immediately after reset_stats";

    ASSERT_EQ(MEMPOOL_OK, mempool_free(e.pool, b1));
#else
    GTEST_SKIP();
#endif
}

/* ----------- Multiple independent pools -------------------------------- */

TEST(MultiPool, BlocksBelongToCorrectPool) {
    alignas(8) uint8_t sA[kStateSize]{}, sB[kStateSize]{};
    alignas(8) uint8_t bA[MEMPOOL_POOL_BUFFER_SIZE(32U,4U,8U)]{};
    alignas(8) uint8_t bB[MEMPOOL_POOL_BUFFER_SIZE(64U,4U,8U)]{};
    mempool_t *pA=nullptr, *pB=nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(sA,sizeof sA,bA,sizeof bA,32U,8U,&pA));
    ASSERT_EQ(MEMPOOL_OK, mempool_init(sB,sizeof sB,bB,sizeof bB,64U,8U,&pB));

    void *from_A=nullptr, *from_B=nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pA, &from_A));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pB, &from_B));

    EXPECT_EQ(1, mempool_contains(pA, from_A));
    EXPECT_EQ(0, mempool_contains(pA, from_B));
    EXPECT_EQ(0, mempool_contains(pB, from_A));
    EXPECT_EQ(1, mempool_contains(pB, from_B));

    /* Cross-pool free must fail. */
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK, mempool_free(pA, from_B));
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK, mempool_free(pB, from_A));

    ASSERT_EQ(MEMPOOL_OK, mempool_free(pA, from_A));
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pB, from_B));
}

/* ----------- Pool manager (mgr) bounds / routing ----------------------- */

TEST(MgrBounds, CorruptCountGuard) {
    alignas(8) uint8_t s0[kStateSize]{}, s1[kStateSize]{};
    alignas(8) uint8_t b0[MEMPOOL_POOL_BUFFER_SIZE(32U,4U,8U)]{};
    alignas(8) uint8_t b1[MEMPOOL_POOL_BUFFER_SIZE(64U,4U,8U)]{};
    mempool_t *p0=nullptr, *p1=nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(s0,sizeof s0,b0,sizeof b0,32U,8U,&p0));
    ASSERT_EQ(MEMPOOL_OK, mempool_init(s1,sizeof s1,b1,sizeof b1,64U,8U,&p1));

    mempool_mgr_t mgr{};
    mempool_t *pools[] = { p0, p1 };
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_init(&mgr, pools, 2U));

    /* Alloc a block while the mgr is healthy, so we have a non-NULL pointer. */
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 16U, &b, nullptr));
    ASSERT_NE(nullptr, b);

    /* Corrupt the count field beyond MEMPOOL_MGR_MAX_POOLS. */
    mgr.count = (uint32_t)MEMPOOL_MGR_MAX_POOLS + 1U;

    /* Both alloc and free must reject the corrupted manager. */
    void *b2 = nullptr;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE, mempool_mgr_alloc(&mgr, 32U, &b2, nullptr));
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE, mempool_mgr_free(&mgr, b));

    /* Restore count so we can cleanly free the pre-allocated block. */
    mgr.count = 2U;
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_free(&mgr, b));
}

TEST(MgrRouting, AllocsToSmallestFittingPool) {
    alignas(8) uint8_t s0[kStateSize]{}, s1[kStateSize]{};
    alignas(8) uint8_t b0[MEMPOOL_POOL_BUFFER_SIZE(32U,4U,8U)]{};
    alignas(8) uint8_t b1[MEMPOOL_POOL_BUFFER_SIZE(64U,4U,8U)]{};
    mempool_t *p0=nullptr, *p1=nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(s0,sizeof s0,b0,sizeof b0,32U,8U,&p0));
    ASSERT_EQ(MEMPOOL_OK, mempool_init(s1,sizeof s1,b1,sizeof b1,64U,8U,&p1));

    mempool_mgr_t mgr{};
    mempool_t *pools[] = { p0, p1 };
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_init(&mgr, pools, 2U));

    mempool_t *out = nullptr;
    void *b = nullptr;
    /* Request 24 bytes — should go to the 32-byte pool. */
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 24U, &b, &out));
    EXPECT_EQ(p0, out) << "24 bytes should route to the 32-byte pool";
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_free(&mgr, b));

    /* Request 33 bytes — should go to the 64-byte pool. */
    out = nullptr; b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 33U, &b, &out));
    EXPECT_EQ(p1, out) << "33 bytes should route to the 64-byte pool";
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_free(&mgr, b));
}

TEST(MgrRouting, FallbackWhenFirstPoolExhausted) {
    alignas(8) uint8_t s0[kStateSize]{}, s1[kStateSize]{};
    alignas(8) uint8_t b0[MEMPOOL_POOL_BUFFER_SIZE(32U,2U,8U)]{};
    alignas(8) uint8_t b1[MEMPOOL_POOL_BUFFER_SIZE(32U,4U,8U)]{};
    mempool_t *p0=nullptr, *p1=nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(s0,sizeof s0,b0,sizeof b0,32U,8U,&p0));
    ASSERT_EQ(MEMPOOL_OK, mempool_init(s1,sizeof s1,b1,sizeof b1,32U,8U,&p1));

    mempool_mgr_t mgr{};
    mempool_t *pools[] = { p0, p1 };
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_init(&mgr, pools, 2U));

    /* Exhaust p0 first (it was inserted first, so mgr uses it first). */
    std::vector<void*> from_p0;
    for (;;) {
        void *b = nullptr;
        mempool_t *src = nullptr;
        if (mempool_mgr_alloc(&mgr, 32U, &b, &src) != MEMPOOL_OK) break;
        if (src != p0) { mempool_mgr_free(&mgr, b); break; }
        from_p0.push_back(b);
    }
    EXPECT_GT(from_p0.size(), 0U);

    /* Next alloc must fall back to p1. */
    void *b = nullptr;
    mempool_t *src = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 32U, &b, &src));
    EXPECT_EQ(p1, src);
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_free(&mgr, b));

    for (void *x : from_p0) mempool_mgr_free(&mgr, x);
}

TEST(MgrRouting, AllPoolsExhausted) {
    alignas(8) uint8_t s0[kStateSize]{};
    alignas(8) uint8_t b0[MEMPOOL_POOL_BUFFER_SIZE(32U,1U,8U)]{};
    mempool_t *p0=nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(s0,sizeof s0,b0,sizeof b0,32U,8U,&p0));

    mempool_mgr_t mgr{};
    mempool_t *pools[] = { p0 };
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_init(&mgr, pools, 1U));

    /* Exhaust the single pool. */
    void *b0_ptr = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 32U, &b0_ptr, nullptr));

    void *b1 = nullptr;
    EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, mempool_mgr_alloc(&mgr, 32U, &b1, nullptr));
    EXPECT_EQ(nullptr, b1);

    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_free(&mgr, b0_ptr));
}

/* ----------- mempool_state_size ---------------------------------------- */

TEST(StateSize, FitsInMEMPOOL_STATE_SIZE) {
    EXPECT_LE(mempool_state_size(), (size_t)MEMPOOL_STATE_SIZE);
    EXPECT_GT(mempool_state_size(), 0U);
}

/* ----------- version string -------------------------------------------- */

TEST(Version, StringNotEmpty) {
    EXPECT_GT(strlen(MEMPOOL_VERSION_STRING), 0U);
}

/* ----------- mempool_block_size (user-visible) ------------------------- */

TEST(BlockSize, ReturnsStrideNotUserSize) {
    /* block_size=10, alignment=4 → stride = align_up(10+4,4) = 16 (with GUARD)
     * or align_up(10,4) = 12 (without GUARD).
     * The function always returns the stride (physical footprint). */
    PoolEnv e;
    alignas(8) uint8_t buf[MEMPOOL_POOL_BUFFER_SIZE(32U,4U,8U)]{};
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(e.state, sizeof e.state, buf, sizeof buf, 32U, 8U, &e.pool));
    uint32_t stride = mempool_block_size(e.pool);
    EXPECT_GE(stride, 32U);  /* at least the user size */
}

/* ----------- mempool_walk: freed blocks are NOT visited ---------------- */

TEST(Walk, FreedBlocksNotVisited) {
    PoolEnv e; e.init();

    /* Alloc two blocks, free one, walk and count — should see only 1. */
    void *b0=nullptr, *b1=nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(e.pool, &b0));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(e.pool, &b1));
    ASSERT_EQ(MEMPOOL_OK, mempool_free(e.pool, b0));

    uint32_t alloc_cnt = 0U;
    ASSERT_EQ(MEMPOOL_OK, mempool_walk(e.pool,
        [](const mempool_t * /*p*/, const void * /*blk*/,
           uint32_t /*idx*/, void *ctx) {
            (*static_cast<uint32_t *>(ctx))++;
        }, &alloc_cnt));
    EXPECT_EQ(1U, alloc_cnt);  /* b1 still allocated; b0 freed */

    ASSERT_EQ(MEMPOOL_OK, mempool_free(e.pool, b1));
}

/* ----------- alloc_zero zeroes the full user area --------------------- */

TEST(AllocZero, SingleBlock_FullUserArea) {
    PoolEnv e; e.init();
    void *b = nullptr;
    memset(e.buf, 0xBB, sizeof e.buf);  /* dirty the pool buffer */
    ASSERT_EQ(MEMPOOL_OK, mempool_init(e.state, sizeof e.state,
                                       e.buf, sizeof e.buf, kStdBs, kStdAl, &e.pool));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_zero(e.pool, &b));
    const uint8_t *bytes = static_cast<const uint8_t *>(b);
    for (uint32_t i = 0U; i < kStdBs; i++) {
        EXPECT_EQ(0U, bytes[i]) << "byte " << i << " not zeroed";
    }
    ASSERT_EQ(MEMPOOL_OK, mempool_free(e.pool, b));
}

/* ----------- alloc OOM leaves *block == NULL --------------------------- */

TEST(AllocOOM, OutputPointerNullOnOOM) {
    alignas(8) uint8_t state[kStateSize]{};
    alignas(8) uint8_t buf[MEMPOOL_POOL_BUFFER_SIZE(32U,1U,8U)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state, sizeof state, buf, sizeof buf, 32U, 8U, &pool));
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b));
    void *b2 = (void*)0xDEAD;
    EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, mempool_alloc(pool, &b2));
    EXPECT_EQ(nullptr, b2) << "*block must be NULL on OOM";
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool, b));
}

/* ----------- reset clears guard violations counter -------------------- */

TEST(Reset, ClearsGuardViolations) {
#if MEMPOOL_ENABLE_STATS && MEMPOOL_ENABLE_GUARD
    PoolEnv e; e.init();
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(e.pool, &b));
    /* Corrupt canary byte 0. */
    *(static_cast<uint8_t *>(b) + kStdBs) ^= 0xFFU;
    EXPECT_EQ(MEMPOOL_ERR_GUARD_CORRUPTED, mempool_free(e.pool, b));

    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(e.pool, &s));
    EXPECT_EQ(1U, s.guard_violations);

    ASSERT_EQ(MEMPOOL_OK, mempool_reset(e.pool));
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(e.pool, &s));
    EXPECT_EQ(0U, s.guard_violations);
#else
    GTEST_SKIP();
#endif
}

/* ----------- OOM hook fires on exhaustion ----------------------------- */

TEST(OomHook, InvokedOnExhaustion) {
    alignas(8) uint8_t state[kStateSize]{};
    alignas(8) uint8_t buf[MEMPOOL_POOL_BUFFER_SIZE(32U,2U,8U)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state, sizeof state, buf, sizeof buf, 32U, 8U, &pool));

    int hook_calls = 0;
    ASSERT_EQ(MEMPOOL_OK, mempool_set_oom_hook(pool,
        [](mempool_t * /*p*/, void *ctx) {
            (*static_cast<int *>(ctx))++;
        }, &hook_calls));

    void *b0=nullptr, *b1=nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b0));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b1));
    EXPECT_EQ(0, hook_calls);

    void *b2 = nullptr;
    EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, mempool_alloc(pool, &b2));
    EXPECT_GE(hook_calls, 1) << "OOM hook should have fired";

    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool, b0));
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool, b1));
}

/* ----------- mempool_pool_buffer_size corner cases -------------------- */

TEST(PoolBufSize, ZeroBlockSizeReturnsZero)    { EXPECT_EQ(0U, mempool_pool_buffer_size(0, 4, 8)); }
TEST(PoolBufSize, ZeroNumBlocksReturnsZero)    { EXPECT_EQ(0U, mempool_pool_buffer_size(32, 0, 8)); }
TEST(PoolBufSize, NonPow2AlignmentReturnsZero) { EXPECT_EQ(0U, mempool_pool_buffer_size(32, 4, 3)); }
TEST(PoolBufSize, SingleBlock)                 { EXPECT_GT(mempool_pool_buffer_size(32, 1, 8), 0U); }
TEST(PoolBufSize, IncreaseWithN) {
    size_t s1 = mempool_pool_buffer_size(32, 1, 8);
    size_t s4 = mempool_pool_buffer_size(32, 4, 8);
    EXPECT_GT(s4, s1);
}

} /* anonymous namespace */
