/*
 * Stress test suite for mempool v0.5.1 — 1030 parameterised tests
 *
 * Tests all major allocation patterns over 103 (block_size × num_blocks ×
 * alignment) configurations, with ALL optional features compiled in.
 *
 * Each configuration is exercised by 10 independent test cases:
 *   1.  AllocAll_FreeAll_LIFO          — fill pool, free in stack (natural) order
 *   2.  AllocAll_FreeAll_FIFO          — fill pool, free in queue (reverse) order
 *   3.  AllocAll_FreeAll_Random        — fill pool, free in shuffled order
 *   4.  Interleaved_AllocFree          — alloc-one / free-one, repeated
 *   5.  FillDrain_5Cycles              — five complete fill-then-drain cycles
 *   6.  HalfAllocFree_ThenRest         — alloc half, free half, alloc remaining
 *   7.  Reset_RestoresCapacity         — partial alloc → reset → alloc all
 *   8.  Stats_ConsistentAfterEveryOp   — used+free == total after every alloc/free
 *   9.  PoisonPattern_AfterAlloc       — bytes == ALLOC_POISON_BYTE after alloc
 *  10.  HasFreeBlock_Transitions       — has_free_block tracks free-list state
 *
 * All features enabled (matches mempool_gtest_hardening configuration).
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>
#include <random>

static std::mutex g_stress_mtx;
static std::mutex g_stress_isr_mtx;
extern "C" {
void mempool_test_lock(void)       { g_stress_mtx.lock();       }
void mempool_test_unlock(void)     { g_stress_mtx.unlock();     }
void mempool_test_isr_lock(void)   { g_stress_isr_mtx.lock();   }
void mempool_test_isr_unlock(void) { g_stress_isr_mtx.unlock(); }
#include "mempool.h"
}

namespace {

/* -----------------------------------------------------------------------
 * Configuration descriptor
 * --------------------------------------------------------------------- */
struct SC {
    uint32_t bs; /* block_size  */
    uint32_t nb; /* num_blocks (minimum requested; actual may be more) */
    uint32_t al; /* alignment   */
};

/* -----------------------------------------------------------------------
 * Parameterised fixture
 * --------------------------------------------------------------------- */
class StressTest : public ::testing::TestWithParam<SC> {
protected:
    mempool_t *pool_ = nullptr;
    std::vector<uint8_t> pool_buf_;
    alignas(32) uint8_t state_[MEMPOOL_STATE_SIZE]{};

    void SetUp() override {
        SC p = GetParam();
        /* Size the buffer with the conservative macro (all features ON). */
        size_t bufsz = MEMPOOL_POOL_BUFFER_SIZE(p.bs, p.nb, p.al);
        pool_buf_.assign(bufsz + p.al, 0U);

        /* Align the raw buffer to the requested alignment. */
        void *ptr   = pool_buf_.data();
        size_t space = pool_buf_.size();
        std::align(p.al, bufsz, ptr, space);

        mempool_error_t err = mempool_init(
            state_, sizeof state_,
            ptr, bufsz,
            p.bs, p.al,
            &pool_
        );
        ASSERT_EQ(MEMPOOL_OK, err) << "mempool_init failed for bs=" << p.bs
                                    << " nb=" << p.nb << " al=" << p.al;
        ASSERT_NE(nullptr, pool_);
    }

    /* Allocate as many blocks as the pool will give, up to cap. */
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

    uint32_t total_blocks() {
#if MEMPOOL_ENABLE_STATS
        mempool_stats_t s{};
        EXPECT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
        return s.total_blocks;
#else
        return GetParam().nb; /* approximate */
#endif
    }

    void assert_stats_consistent() {
#if MEMPOOL_ENABLE_STATS
        mempool_stats_t s{};
        ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
        EXPECT_EQ(s.total_blocks, s.used_blocks + s.free_blocks);
        EXPECT_LE(s.used_blocks, s.total_blocks);
        EXPECT_GE(s.peak_usage, s.used_blocks);
#endif
    }
};

/* -----------------------------------------------------------------------
 * Test 1 — Allocate all, free in LIFO (stack) order
 * --------------------------------------------------------------------- */
TEST_P(StressTest, AllocAll_FreeAll_LIFO) {
    auto blocks = alloc_all();
    ASSERT_GT(blocks.size(), 0U);
    /* Free in reverse order (LIFO — natural free-list order). */
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool_, *it));
    }
    assert_stats_consistent();
#if MEMPOOL_ENABLE_STATS
    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
    EXPECT_EQ(0U, s.used_blocks);
    EXPECT_EQ(s.total_blocks, s.free_blocks);
#endif
    /* Pool should be fully available again. */
    EXPECT_EQ(1, mempool_has_free_block(pool_));
}

/* -----------------------------------------------------------------------
 * Test 2 — Allocate all, free in FIFO (queue) order
 * --------------------------------------------------------------------- */
TEST_P(StressTest, AllocAll_FreeAll_FIFO) {
    auto blocks = alloc_all();
    ASSERT_GT(blocks.size(), 0U);
    for (void *b : blocks) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
    }
    assert_stats_consistent();
#if MEMPOOL_ENABLE_STATS
    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
    EXPECT_EQ(0U, s.used_blocks);
#endif
}

/* -----------------------------------------------------------------------
 * Test 3 — Allocate all, free in random order
 * --------------------------------------------------------------------- */
TEST_P(StressTest, AllocAll_FreeAll_Random) {
    auto blocks = alloc_all();
    ASSERT_GT(blocks.size(), 0U);
    std::mt19937 rng(42U + GetParam().bs + GetParam().nb);
    std::shuffle(blocks.begin(), blocks.end(), rng);
    for (void *b : blocks) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
    }
    assert_stats_consistent();
#if MEMPOOL_ENABLE_STATS
    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
    EXPECT_EQ(0U, s.used_blocks);
#endif
}

/* -----------------------------------------------------------------------
 * Test 4 — Interleaved alloc / free
 * --------------------------------------------------------------------- */
TEST_P(StressTest, Interleaved_AllocFree) {
    uint32_t n = total_blocks();
    if (n == 0U) { GTEST_SKIP(); }
    for (uint32_t i = 0U; i < n * 4U; i++) {
        void *b = nullptr;
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b));
        ASSERT_NE(nullptr, b);
        EXPECT_EQ(1, mempool_contains(pool_, b));
        ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
    }
    assert_stats_consistent();
#if MEMPOOL_ENABLE_STATS
    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
    EXPECT_EQ(0U, s.used_blocks);
#endif
}

/* -----------------------------------------------------------------------
 * Test 5 — Five complete fill-then-drain cycles
 * --------------------------------------------------------------------- */
TEST_P(StressTest, FillDrain_5Cycles) {
    for (int cycle = 0; cycle < 5; cycle++) {
        auto blocks = alloc_all();
        ASSERT_GT(blocks.size(), 0U) << "cycle " << cycle;
        /* Next alloc must fail (pool exhausted). */
        void *extra = nullptr;
        EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, mempool_alloc(pool_, &extra));
        EXPECT_EQ(nullptr, extra);
        EXPECT_EQ(0, mempool_has_free_block(pool_));
        /* Free all. */
        for (void *b : blocks) {
            EXPECT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
        }
        EXPECT_EQ(1, mempool_has_free_block(pool_));
        assert_stats_consistent();
    }
}

/* -----------------------------------------------------------------------
 * Test 6 — Alloc half, free half, alloc the rest
 * --------------------------------------------------------------------- */
TEST_P(StressTest, HalfAllocFree_ThenRest) {
    uint32_t n = total_blocks();
    if (n < 2U) { GTEST_SKIP(); }

    /* Alloc first half. */
    uint32_t half = n / 2U;
    std::vector<void *> first_half;
    first_half.reserve(half);
    for (uint32_t i = 0U; i < half; i++) {
        void *b = nullptr;
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b)) << "alloc " << i;
        first_half.push_back(b);
    }
    assert_stats_consistent();

    /* Free first half (in random order). */
    std::mt19937 rng(7U + n);
    std::shuffle(first_half.begin(), first_half.end(), rng);
    for (void *b : first_half) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
    }

    /* Alloc everything remaining. */
    auto rest = alloc_all();
    EXPECT_GE(rest.size(), (size_t)1U);

    /* Pool must now be exhausted. */
    void *overflow = nullptr;
    EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, mempool_alloc(pool_, &overflow));
    EXPECT_EQ(0, mempool_has_free_block(pool_));

    for (void *b : rest) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
    }
    assert_stats_consistent();
}

/* -----------------------------------------------------------------------
 * Test 7 — Reset restores full capacity
 * --------------------------------------------------------------------- */
TEST_P(StressTest, Reset_RestoresCapacity) {
    uint32_t n = total_blocks();
    if (n == 0U) { GTEST_SKIP(); }

    /* Alloc some blocks. */
    uint32_t to_alloc = (n > 1U) ? n / 2U : 1U;
    for (uint32_t i = 0U; i < to_alloc; i++) {
        void *b = nullptr;
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b));
    }
#if MEMPOOL_ENABLE_STATS
    {
        mempool_stats_t s{};
        ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
        EXPECT_EQ(to_alloc, s.used_blocks);
    }
#endif

    /* Reset. */
    ASSERT_EQ(MEMPOOL_OK, mempool_reset(pool_));

#if MEMPOOL_ENABLE_STATS
    {
        mempool_stats_t s{};
        ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
        EXPECT_EQ(0U, s.used_blocks);
        EXPECT_EQ(s.total_blocks, s.free_blocks);
    }
#endif

    /* Allocate all blocks again. */
    auto blocks = alloc_all();
    EXPECT_EQ((size_t)n, blocks.size());
    for (void *b : blocks) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
    }
}

/* -----------------------------------------------------------------------
 * Test 8 — Stats remain consistent after every individual operation
 * --------------------------------------------------------------------- */
TEST_P(StressTest, Stats_ConsistentAfterEveryOp) {
#if MEMPOOL_ENABLE_STATS
    uint32_t n = total_blocks();
    if (n == 0U) { GTEST_SKIP(); }

    std::vector<void *> allocated;
    allocated.reserve(n);

    /* Alloc one at a time and verify. */
    for (uint32_t i = 0U; i < n; i++) {
        void *b = nullptr;
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b)) << "alloc " << i;
        allocated.push_back(b);

        mempool_stats_t s{};
        ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
        EXPECT_EQ(s.total_blocks, s.used_blocks + s.free_blocks) << "after alloc " << i;
        EXPECT_EQ(i + 1U, s.used_blocks)     << "after alloc " << i;
        EXPECT_GE(s.peak_usage, s.used_blocks) << "after alloc " << i;
    }

    /* Free one at a time and verify. */
    for (uint32_t i = 0U; i < n; i++) {
        ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, allocated[i])) << "free " << i;

        mempool_stats_t s{};
        ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool_, &s));
        EXPECT_EQ(s.total_blocks, s.used_blocks + s.free_blocks) << "after free " << i;
        EXPECT_EQ(n - i - 1U, s.used_blocks) << "after free " << i;
    }
#else
    GTEST_SKIP();
#endif
}

/* -----------------------------------------------------------------------
 * Test 9 — Alloc-poison pattern visible immediately after allocation
 * --------------------------------------------------------------------- */
TEST_P(StressTest, PoisonPattern_AfterAlloc) {
#if MEMPOOL_ENABLE_POISON
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool_, &b));
    ASSERT_NE(nullptr, b);

    SC p = GetParam();
    const uint8_t *bytes = static_cast<const uint8_t *>(b);
    bool all_poison = true;
    for (uint32_t i = 0U; i < p.bs; i++) {
        if (bytes[i] != (uint8_t)MEMPOOL_ALLOC_POISON_BYTE) {
            all_poison = false;
            break;
        }
    }
    EXPECT_TRUE(all_poison)
        << "alloc-poison not uniform at byte position; bs=" << p.bs;

    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
#else
    GTEST_SKIP();
#endif
}

/* -----------------------------------------------------------------------
 * Test 10 — mempool_has_free_block tracks free-list state correctly
 * --------------------------------------------------------------------- */
TEST_P(StressTest, HasFreeBlock_Transitions) {
    /* Fresh pool must have a free block. */
    EXPECT_EQ(1, mempool_has_free_block(pool_));

    auto blocks = alloc_all();
    ASSERT_GT(blocks.size(), 0U);

    /* Exhausted pool has no free block. */
    EXPECT_EQ(0, mempool_has_free_block(pool_));

    /* After one free, has a free block again. */
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool_, blocks.back()));
    blocks.pop_back();
    EXPECT_EQ(1, mempool_has_free_block(pool_));

    /* Free the rest. */
    for (void *b : blocks) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool_, b));
    }
    EXPECT_EQ(1, mempool_has_free_block(pool_));
}

/* -----------------------------------------------------------------------
 * Configuration list — 103 (block_size × num_blocks × alignment) entries
 * --------------------------------------------------------------------- */
/* 8-byte blocks (minimum for 64-bit free-list node) */
static const SC k8[] = {
    {8, 1, 4},  {8, 2, 4},  {8, 4, 4},  {8, 8, 4},  {8, 16, 4}, {8, 32, 4},
    {8, 1, 8},  {8, 2, 8},  {8, 4, 8},  {8, 8, 8},  {8, 16, 8}, {8, 32, 8},
};

/* 16-byte blocks */
static const SC k16[] = {
    {16, 1, 4},  {16, 2, 4},  {16, 4, 4},  {16, 8, 4},  {16, 16, 4}, {16, 32, 4},
    {16, 1, 8},  {16, 2, 8},  {16, 4, 8},  {16, 8, 8},  {16, 16, 8}, {16, 32, 8},
    {16, 1, 16}, {16, 2, 16}, {16, 4, 16}, {16, 8, 16}, {16, 16, 16},{16, 32, 16},
};

/* 32-byte blocks */
static const SC k32[] = {
    {32, 1, 4},  {32, 2, 4},  {32, 4, 4},  {32, 8, 4},  {32, 16, 4}, {32, 32, 4},
    {32, 1, 8},  {32, 2, 8},  {32, 4, 8},  {32, 8, 8},  {32, 16, 8}, {32, 32, 8},
    {32, 1, 16}, {32, 2, 16}, {32, 4, 16}, {32, 8, 16}, {32, 16, 16},{32, 32, 16},
    {32, 1, 32}, {32, 2, 32}, {32, 4, 32}, {32, 8, 32}, {32, 16, 32},{32, 32, 32},
};

/* 64-byte blocks */
static const SC k64[] = {
    {64, 1, 4},  {64, 2, 4},  {64, 4, 4},  {64, 8, 4},  {64, 16, 4},
    {64, 1, 8},  {64, 2, 8},  {64, 4, 8},  {64, 8, 8},  {64, 16, 8}, {64, 32, 8},  {64, 64, 8},
    {64, 1, 16}, {64, 4, 16}, {64, 8, 16}, {64, 16, 16},{64, 32, 16},{64, 64, 16},
    {64, 1, 32}, {64, 4, 32}, {64, 8, 32}, {64, 16, 32},
};

/* 128-byte blocks */
static const SC k128[] = {
    {128, 1, 8},  {128, 2, 8},  {128, 4, 8},  {128, 8, 8},  {128, 16, 8},  {128, 32, 8},
    {128, 1, 16}, {128, 4, 16}, {128, 8, 16}, {128, 16, 16},
};

/* 256-byte blocks */
static const SC k256[] = {
    {256, 1, 8}, {256, 2, 8}, {256, 4, 8}, {256, 8, 8},
    {256, 1, 16},{256, 4, 16},{256, 8, 16},
};

/* Non-power-of-2 block sizes — stress canary alignment (user_block_size%4!=0) */
static const SC knpow[] = {
    /* block_size % 4 == 0 but non-power-of-2 */
    {12, 4, 4},  {12, 8, 4},  {12, 16, 4},
    {20, 4, 4},  {20, 8, 4},  {20, 4, 8},  {20, 8, 8},
    {48, 4, 8},  {48, 8, 8},  {48, 16, 8},
    /* block_size % 4 != 0 — exercises memcpy canary fix */
    {9,  2, 4},  {9,  4, 4},
    {10, 2, 4},  {10, 4, 4},  {10, 4, 8},
    {11, 2, 4},  {11, 4, 4},
    {18, 4, 8},  {18, 8, 8},
    {21, 4, 4},  {21, 4, 8},
};

/* Flatten all config arrays into one Values() list */
INSTANTIATE_TEST_SUITE_P(
    Configs8B,   StressTest,
    ::testing::ValuesIn(k8));

INSTANTIATE_TEST_SUITE_P(
    Configs16B,  StressTest,
    ::testing::ValuesIn(k16));

INSTANTIATE_TEST_SUITE_P(
    Configs32B,  StressTest,
    ::testing::ValuesIn(k32));

INSTANTIATE_TEST_SUITE_P(
    Configs64B,  StressTest,
    ::testing::ValuesIn(k64));

INSTANTIATE_TEST_SUITE_P(
    Configs128B, StressTest,
    ::testing::ValuesIn(k128));

INSTANTIATE_TEST_SUITE_P(
    Configs256B, StressTest,
    ::testing::ValuesIn(k256));

INSTANTIATE_TEST_SUITE_P(
    ConfigsNPow, StressTest,
    ::testing::ValuesIn(knpow));

} /* anonymous namespace */
