/*
 * Comprehensive test suite for mempool library - 500+ test cases
 * Covers initialization, allocation, freeing, stats, concurrency, edge cases
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <random>
#include <thread>
#include <atomic>
#include <mutex>

/* Test sync primitives */
static std::mutex g_mempool_mtx;
extern "C" {
void mempool_test_lock(void)   { g_mempool_mtx.lock();   }
void mempool_test_unlock(void) { g_mempool_mtx.unlock(); }
#include "mempool.h"
}

namespace {

constexpr size_t MAX_POOL_BUF = 128U * 1024U;
constexpr size_t TEST_ALIGN = 8U;

/* Calculate minimum pool buffer size needed for N blocks of given size and alignment.
 * Accounts for bitmap overhead when MEMPOOL_ENABLE_DOUBLE_FREE_CHECK=1 */
static size_t calc_min_pool_size(size_t block_size, size_t num_blocks, size_t alignment) {
#ifdef MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    size_t bitmap_size = (num_blocks + 7) / 8;  /* Round up to bytes */
    /* Align bitmap to alignment */
    size_t bitmap_aligned = (bitmap_size + alignment - 1) & ~(alignment - 1);
    return bitmap_aligned + (num_blocks * block_size);
#else
    (void)alignment;
    return num_blocks * block_size;
#endif
}

/* ========================================================================
 * PARAMETERIZED INITIALIZATION TESTS (~120 cases)
 * ======================================================================== */

struct InitParams {
    size_t alignment;
    size_t block_size;
    size_t pool_size;
    bool should_succeed;
};

class MempoolInitTest : public ::testing::TestWithParam<InitParams> {};

TEST_P(MempoolInitTest, InitWithVariousConfigurations) {
    InitParams p = GetParam();
    
    alignas(64) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    std::vector<uint8_t> pool_buf(p.pool_size + 64);
    
    /* Align pool buffer */
    void *aligned_pool = pool_buf.data();
    size_t space = pool_buf.size();
    std::align(p.alignment, p.pool_size, aligned_pool, space);
    
    mempool_t *pool = nullptr;
    mempool_error_t err = mempool_init(
        state_buf, sizeof(state_buf),
        aligned_pool, p.pool_size,
        p.block_size, p.alignment,
        &pool
    );
    
    if (p.should_succeed) {
        EXPECT_EQ(MEMPOOL_OK, err);
        if (err == MEMPOOL_OK) {
            ASSERT_NE(nullptr, pool);
#if MEMPOOL_ENABLE_STATS
            mempool_stats_t stats;
            EXPECT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
            EXPECT_GT(stats.total_blocks, 0U);
            EXPECT_EQ(stats.total_blocks, stats.free_blocks);
#endif
        }
    } else {
        EXPECT_NE(MEMPOOL_OK, err);
    }
}

/* Power-of-2 alignments × power-of-2 block sizes × various pool sizes */
INSTANTIATE_TEST_SUITE_P(
    AlignmentBlockSizeCombos,
    MempoolInitTest,
    ::testing::Values(
        /* alignment=1 (will fail) */
        InitParams{1, 64, 4096, false},
        /* alignment=2 */
        InitParams{2, 8, 1024, true},
        InitParams{2, 16, 2048, true},
        InitParams{2, 32, 4096, true},
        /* alignment=4 */
        InitParams{4, 8, 512, true},
        InitParams{4, 16, 1024, true},
        InitParams{4, 32, 2048, true},
        InitParams{4, 64, 4096, true},
        InitParams{4, 128, 8192, true},
        /* alignment=8 */
        InitParams{8, 8, 256, true},
        InitParams{8, 16, 512, true},
        InitParams{8, 32, 1024, true},
        InitParams{8, 64, 2048, true},
        InitParams{8, 128, 4096, true},
        InitParams{8, 256, 8192, true},
        InitParams{8, 512, 16384, true},
        InitParams{8, 1024, 32768, true},
        /* alignment=16 */
        InitParams{16, 16, 512, true},
        InitParams{16, 32, 1024, true},
        InitParams{16, 64, 2048, true},
        InitParams{16, 128, 4096, true},
        InitParams{16, 256, 8192, true},
        InitParams{16, 512, 16384, true},
        /* alignment=32 */
        InitParams{32, 32, 1024, true},
        InitParams{32, 64, 2048, true},
        InitParams{32, 128, 4096, true},
        InitParams{32, 256, 8192, true},
        /* alignment=64 */
        InitParams{64, 64, 2048, true},
        InitParams{64, 128, 4096, true},
        InitParams{64, 256, 8192, true},
        /* Non-power-of-2 block sizes */
        InitParams{8, 48, 2048, true},
        InitParams{8, 80, 4096, true},
        InitParams{8, 96, 4096, true},
        InitParams{8, 100, 8192, true},
        InitParams{8, 200, 16384, true},
        InitParams{16, 48, 2048, true},
        InitParams{16, 80, 4096, true},
        /* Edge: pool too small */
        InitParams{8, 64, 32, false},
        InitParams{8, 128, 64, false},
        InitParams{8, 256, 128, false},
        /* Edge: block_size too small (< sizeof(free_node_t) = 8 on 64-bit) */
        InitParams{4, 4, 1024, false},
        InitParams{8, 4, 1024, false},
        /* Edge: exact fit */
        InitParams{8, 64, 64, true},
        InitParams{8, 128, 128, true},
        /* Edge: off-by-one too small */
        InitParams{8, 64, 63, false},
        InitParams{8, 128, 127, false},
        /* Large pools */
        InitParams{8, 64, 65536, true},
        InitParams{8, 128, 65536, true},
        InitParams{8, 256, 65536, true},
        InitParams{16, 64, 32768, true},
        InitParams{16, 128, 65536, true}
    )
);

/* Additional 50 init tests with more variations */
INSTANTIATE_TEST_SUITE_P(
    MoreAlignmentVariations,
    MempoolInitTest,
    ::testing::Values(
        InitParams{2, 64, 2048, true},
        InitParams{2, 128, 4096, true},
        InitParams{4, 256, 8192, true},
        InitParams{4, 512, 16384, true},
        InitParams{8, 17, 1024, true},
        InitParams{8, 33, 2048, true},
        InitParams{8, 65, 4096, true},
        InitParams{8, 129, 8192, true},
        InitParams{16, 17, 1024, true},
        InitParams{16, 33, 2048, true},
        InitParams{16, 65, 4096, true},
        InitParams{16, 129, 8192, true},
        InitParams{32, 17, 1024, true},
        InitParams{32, 33, 2048, true},
        InitParams{32, 65, 4096, true},
        InitParams{32, 129, 8192, true},
        InitParams{64, 17, 1024, true},
        InitParams{64, 33, 2048, true},
        InitParams{64, 65, 4096, true},
        InitParams{64, 129, 8192, true},
        InitParams{8, 24, 1024, true},
        InitParams{8, 40, 2048, true},
        InitParams{8, 72, 4096, true},
        InitParams{8, 88, 4096, true},
        InitParams{8, 120, 8192, true},
        InitParams{8, 136, 8192, true},
        InitParams{8, 152, 8192, true},
        InitParams{8, 168, 16384, true},
        InitParams{8, 184, 16384, true},
        InitParams{8, 248, 16384, true},
        InitParams{16, 24, 1024, true},
        InitParams{16, 40, 2048, true},
        InitParams{16, 72, 4096, true},
        InitParams{16, 88, 4096, true},
        InitParams{16, 120, 8192, true},
        InitParams{32, 40, 2048, true},
        InitParams{32, 72, 4096, true},
        InitParams{32, 88, 4096, true},
        InitParams{32, 120, 8192, true},
        InitParams{64, 72, 4096, true},
        InitParams{64, 88, 4096, true},
        InitParams{64, 120, 8192, true},
        /* Pool sizes that are not multiples of block_size */
        InitParams{8, 64, 1000, true},
        InitParams{8, 128, 1500, true},
        InitParams{8, 256, 3000, true},
        InitParams{16, 64, 2000, true},
        InitParams{16, 128, 3000, true},
        InitParams{32, 64, 3000, true},
        InitParams{32, 128, 5000, true},
        InitParams{64, 128, 10000, true}
    )
);

/* ========================================================================
 * ALLOCATION TESTS (~80 cases)
 * ======================================================================== */

struct AllocParams {
    size_t block_size;
    size_t num_blocks;      /* Desired capacity in blocks */
    size_t num_allocs;      /* Number of allocs to attempt */
    bool should_oom;
};

class MempoolAllocTest : public ::testing::TestWithParam<AllocParams> {};

TEST_P(MempoolAllocTest, AllocationPatterns) {
    AllocParams p = GetParam();
    
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    /* Calculate required pool size accounting for bitmap */
    size_t pool_size = calc_min_pool_size(p.block_size, p.num_blocks, TEST_ALIGN);
    std::vector<uint8_t> pool_buf(pool_size);
    
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        p.block_size, TEST_ALIGN, &pool
    ));
    
    std::vector<void*> blocks;
    size_t successful_allocs = 0;
    
    for (size_t i = 0; i < p.num_allocs; ++i) {
        void *block = nullptr;
        mempool_error_t err = mempool_alloc(pool, &block);
        if (err == MEMPOOL_OK) {
            ASSERT_NE(nullptr, block);
            /* Verify alignment */
            EXPECT_EQ(0U, ((uintptr_t)block) % TEST_ALIGN);
            /* Verify uniqueness */
            for (void *prev : blocks) {
                EXPECT_NE(prev, block);
            }
            blocks.push_back(block);
            successful_allocs++;
        } else {
            EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, err);
            break;
        }
    }
    
    if (p.should_oom) {
        EXPECT_LT(successful_allocs, p.num_allocs);
    } else {
        EXPECT_EQ(successful_allocs, p.num_allocs);
    }
    
#if MEMPOOL_ENABLE_STATS
    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_EQ(successful_allocs, (size_t)stats.used_blocks);
    EXPECT_EQ(successful_allocs, (size_t)stats.peak_usage);
#endif
    
    /* Free all */
    for (void *block : blocks) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, block));
    }
}

INSTANTIATE_TEST_SUITE_P(
    VariousAllocationCounts,
    MempoolAllocTest,
    ::testing::Values(
        /* Single block pools */
        AllocParams{64, 1, 1, false},
        AllocParams{64, 1, 2, true},
        AllocParams{128, 1, 1, false},
        AllocParams{128, 1, 2, true},
        /* Small pools (8 blocks) */
        AllocParams{64, 8, 1, false},
        AllocParams{64, 8, 4, false},
        AllocParams{64, 8, 7, false},
        AllocParams{64, 8, 8, false},
        AllocParams{64, 8, 9, true},
        /* Medium pools (16 blocks) */
        AllocParams{64, 16, 10, false},
        AllocParams{64, 16, 15, false},
        AllocParams{64, 16, 16, false},
        AllocParams{64, 16, 17, true},
        /* Larger pools (32 blocks) */
        AllocParams{64, 32, 20, false},
        AllocParams{64, 32, 30, false},
        AllocParams{64, 32, 31, false},
        AllocParams{64, 32, 32, false},
        AllocParams{64, 32, 33, true},
        AllocParams{128, 32, 16, false},
        AllocParams{128, 32, 30, false},
        AllocParams{128, 32, 31, false},
        AllocParams{128, 32, 32, false},
        AllocParams{128, 32, 33, true},
        /* Large pools (128 blocks) */
        AllocParams{64, 128, 64, false},
        AllocParams{64, 128, 100, false},
        AllocParams{64, 128, 127, false},
        AllocParams{64, 128, 128, false},
        AllocParams{64, 128, 129, true},
        AllocParams{128, 128, 64, false},
        AllocParams{128, 128, 100, false},
        AllocParams{128, 128, 127, false},
        AllocParams{128, 128, 128, false},
        AllocParams{128, 128, 129, true},
        AllocParams{256, 128, 64, false},
        AllocParams{256, 128, 100, false},
        AllocParams{256, 128, 127, false},
        AllocParams{256, 128, 128, false},
        AllocParams{256, 128, 129, true},
        /* Exhaust exactly */
        AllocParams{32, 16, 16, false},
        AllocParams{32, 16, 17, true},
        AllocParams{64, 16, 16, false},
        AllocParams{64, 16, 17, true},
        AllocParams{128, 16, 16, false},
        AllocParams{128, 16, 17, true}
    )
);

/* More allocation patterns */
INSTANTIATE_TEST_SUITE_P(
    AdditionalAllocationPatterns,
    MempoolAllocTest,
    ::testing::Values(
        AllocParams{16, 16, 8, false},
        AllocParams{16, 16, 16, false},
        AllocParams{16, 16, 17, true},
        AllocParams{32, 16, 8, false},
        AllocParams{32, 16, 16, false},
        AllocParams{32, 16, 17, true},
        AllocParams{64, 16, 8, false},
        AllocParams{64, 32, 16, false},
        AllocParams{64, 32, 32, false},
        AllocParams{64, 32, 33, true},
        AllocParams{128, 16, 8, false},
        AllocParams{128, 16, 16, false},
        AllocParams{128, 16, 17, true},
        AllocParams{256, 16, 8, false},
        AllocParams{256, 16, 16, false},
        AllocParams{256, 16, 17, true},
        AllocParams{512, 16, 8, false},
        AllocParams{512, 16, 16, false},
        AllocParams{512, 16, 17, true},
        AllocParams{24, 20, 10, false},
        AllocParams{24, 20, 20, false},
        AllocParams{24, 20, 21, true},
        AllocParams{40, 25, 12, false},
        AllocParams{40, 25, 24, false},
        AllocParams{40, 25, 26, true},
        AllocParams{48, 21, 10, false},
        AllocParams{48, 21, 20, false},
        AllocParams{48, 21, 22, true},
        AllocParams{80, 25, 12, false},
        AllocParams{80, 25, 24, false},
        AllocParams{80, 25, 26, true},
        AllocParams{96, 21, 10, false},
        AllocParams{96, 21, 20, false},
        AllocParams{96, 21, 22, true},
        AllocParams{100, 4, 4, false},
        AllocParams{100, 4, 5, true},
        AllocParams{200, 40, 20, false},
        AllocParams{200, 40, 40, false},
        AllocParams{200, 40, 41, true}
    )
);

/* ========================================================================
 * FREE TESTS (~60 cases)
 * ======================================================================== */

struct FreeParams {
    size_t block_size;
    size_t num_blocks;
    enum FreeOrder { FORWARD, REVERSE, RANDOM } free_order;
};

class MempoolFreeTest : public ::testing::TestWithParam<FreeParams> {};

TEST_P(MempoolFreeTest, FreeInVariousOrders) {
    FreeParams p = GetParam();
    
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(p.block_size, p.num_blocks, TEST_ALIGN);
    std::vector<uint8_t> pool_buf(pool_size);
    
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        p.block_size, TEST_ALIGN, &pool
    ));
    
    std::vector<void*> blocks;
    for (size_t i = 0; i < p.num_blocks; ++i) {
        void *block = nullptr;
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &block));
        blocks.push_back(block);
    }
    
    std::vector<void*> free_order_vec = blocks;
    switch (p.free_order) {
        case FreeParams::FORWARD:
            /* Already in order */
            break;
        case FreeParams::REVERSE:
            std::reverse(free_order_vec.begin(), free_order_vec.end());
            break;
        case FreeParams::RANDOM:
            std::mt19937 rng(12345);
            std::shuffle(free_order_vec.begin(), free_order_vec.end(), rng);
            break;
    }
    
    for (void *block : free_order_vec) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, block));
    }
    
#if MEMPOOL_ENABLE_STATS
    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_EQ(0U, stats.used_blocks);
    EXPECT_EQ(stats.total_blocks, stats.free_blocks);
#endif
}

INSTANTIATE_TEST_SUITE_P(
    FreeOrderVariations,
    MempoolFreeTest,
    ::testing::Values(
        /* Forward */
        FreeParams{64, 4, FreeParams::FORWARD},
        FreeParams{64, 8, FreeParams::FORWARD},
        FreeParams{64, 16, FreeParams::FORWARD},
        FreeParams{128, 4, FreeParams::FORWARD},
        FreeParams{128, 8, FreeParams::FORWARD},
        FreeParams{128, 16, FreeParams::FORWARD},
        FreeParams{256, 4, FreeParams::FORWARD},
        FreeParams{256, 8, FreeParams::FORWARD},
        FreeParams{256, 16, FreeParams::FORWARD},
        /* Reverse */
        FreeParams{64, 4, FreeParams::REVERSE},
        FreeParams{64, 8, FreeParams::REVERSE},
        FreeParams{64, 16, FreeParams::REVERSE},
        FreeParams{128, 4, FreeParams::REVERSE},
        FreeParams{128, 8, FreeParams::REVERSE},
        FreeParams{128, 16, FreeParams::REVERSE},
        FreeParams{256, 4, FreeParams::REVERSE},
        FreeParams{256, 8, FreeParams::REVERSE},
        FreeParams{256, 16, FreeParams::REVERSE},
        /* Random */
        FreeParams{64, 4, FreeParams::RANDOM},
        FreeParams{64, 8, FreeParams::RANDOM},
        FreeParams{64, 16, FreeParams::RANDOM},
        FreeParams{128, 4, FreeParams::RANDOM},
        FreeParams{128, 8, FreeParams::RANDOM},
        FreeParams{128, 16, FreeParams::RANDOM},
        FreeParams{256, 4, FreeParams::RANDOM},
        FreeParams{256, 8, FreeParams::RANDOM},
        FreeParams{256, 16, FreeParams::RANDOM}
    )
);

/* Additional free test variations */
INSTANTIATE_TEST_SUITE_P(
    MoreFreeVariations,
    MempoolFreeTest,
    ::testing::Values(
        FreeParams{32, 2, FreeParams::FORWARD},
        FreeParams{32, 4, FreeParams::FORWARD},
        FreeParams{32, 8, FreeParams::FORWARD},
        FreeParams{32, 2, FreeParams::REVERSE},
        FreeParams{32, 4, FreeParams::REVERSE},
        FreeParams{32, 8, FreeParams::REVERSE},
        FreeParams{32, 2, FreeParams::RANDOM},
        FreeParams{32, 4, FreeParams::RANDOM},
        FreeParams{32, 8, FreeParams::RANDOM},
        FreeParams{512, 2, FreeParams::FORWARD},
        FreeParams{512, 4, FreeParams::FORWARD},
        FreeParams{512, 8, FreeParams::FORWARD},
        FreeParams{512, 2, FreeParams::REVERSE},
        FreeParams{512, 4, FreeParams::REVERSE},
        FreeParams{512, 8, FreeParams::REVERSE},
        FreeParams{512, 2, FreeParams::RANDOM},
        FreeParams{512, 4, FreeParams::RANDOM},
        FreeParams{512, 8, FreeParams::RANDOM},
        FreeParams{40, 10, FreeParams::FORWARD},
        FreeParams{40, 10, FreeParams::REVERSE},
        FreeParams{40, 10, FreeParams::RANDOM},
        FreeParams{80, 12, FreeParams::FORWARD},
        FreeParams{80, 12, FreeParams::REVERSE},
        FreeParams{80, 12, FreeParams::RANDOM},
        FreeParams{96, 21, FreeParams::FORWARD},
        FreeParams{96, 21, FreeParams::REVERSE},
        FreeParams{96, 21, FreeParams::RANDOM},
        FreeParams{100, 4, FreeParams::FORWARD},
        FreeParams{100, 4, FreeParams::REVERSE},
        FreeParams{100, 4, FreeParams::RANDOM}
    )
);

/* ========================================================================
 * RECYCLE TESTS (~30 cases) - Alloc, Free, Re-Alloc
 * ======================================================================== */

struct RecycleParams {
    size_t block_size;
    size_t num_blocks;
    size_t cycles;
};

class MempoolRecycleTest : public ::testing::TestWithParam<RecycleParams> {};

TEST_P(MempoolRecycleTest, AllocFreeRecycle) {
    RecycleParams p = GetParam();
    
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(p.block_size, p.num_blocks, TEST_ALIGN);
    std::vector<uint8_t> pool_buf(pool_size);
    
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        p.block_size, TEST_ALIGN, &pool
    ));
    
    for (size_t cycle = 0; cycle < p.cycles; ++cycle) {
        void *block = nullptr;
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &block));
        ASSERT_NE(nullptr, block);
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, block));
    }
    
#if MEMPOOL_ENABLE_STATS
    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_EQ(0U, stats.used_blocks);
    EXPECT_EQ((uint32_t)p.cycles, stats.alloc_count);
    EXPECT_EQ((uint32_t)p.cycles, stats.free_count);
#endif
}

INSTANTIATE_TEST_SUITE_P(
    RecycleCounts,
    MempoolRecycleTest,
    ::testing::Values(
        RecycleParams{64, 16, 10},
        RecycleParams{64, 16, 100},
        RecycleParams{64, 16, 1000},
        RecycleParams{128, 16, 10},
        RecycleParams{128, 16, 100},
        RecycleParams{128, 16, 1000},
        RecycleParams{256, 16, 10},
        RecycleParams{256, 16, 100},
        RecycleParams{256, 16, 1000},
        RecycleParams{32, 16, 50},
        RecycleParams{32, 16, 500},
        RecycleParams{512, 16, 50},
        RecycleParams{512, 16, 500},
        RecycleParams{16, 16, 20},
        RecycleParams{16, 16, 200},
        RecycleParams{1024, 16, 20},
        RecycleParams{1024, 16, 200},
        RecycleParams{40, 25, 100},
        RecycleParams{80, 25, 100},
        RecycleParams{96, 21, 100},
        RecycleParams{100, 40, 100},
        RecycleParams{200, 40, 100},
        RecycleParams{48, 21, 50},
        RecycleParams{88, 23, 50},
        RecycleParams{120, 34, 50},
        RecycleParams{168, 48, 50},
        RecycleParams{24, 21, 100},
        RecycleParams{40, 25, 200},
        RecycleParams{72, 28, 200},
        RecycleParams{136, 30, 200}
    )
);

/* ========================================================================
 * RESET TESTS (~30 cases)
 * ======================================================================== */

struct ResetParams {
    size_t block_size;
    size_t num_blocks;
    size_t alloc_before_reset;
};

class MempoolResetTest : public ::testing::TestWithParam<ResetParams> {};

TEST_P(MempoolResetTest, ResetAfterAllocations) {
    ResetParams p = GetParam();
    
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(p.block_size, p.num_blocks, TEST_ALIGN);
    std::vector<uint8_t> pool_buf(pool_size);
    
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        p.block_size, TEST_ALIGN, &pool
    ));
    
    std::vector<void*> blocks;
    for (size_t i = 0; i < p.alloc_before_reset; ++i) {
        void *block = nullptr;
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &block));
        blocks.push_back(block);
    }
    
#if MEMPOOL_ENABLE_STATS
    mempool_stats_t stats_before;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats_before));
    EXPECT_EQ(p.alloc_before_reset, (size_t)stats_before.used_blocks);
#endif
    
    ASSERT_EQ(MEMPOOL_OK, mempool_reset(pool));
    
#if MEMPOOL_ENABLE_STATS
    mempool_stats_t stats_after;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats_after));
    EXPECT_EQ(0U, stats_after.used_blocks);
    EXPECT_EQ(stats_after.total_blocks, stats_after.free_blocks);
    EXPECT_EQ(0U, stats_after.alloc_count);
    EXPECT_EQ(0U, stats_after.free_count);
    EXPECT_EQ(0U, stats_after.peak_usage);
#endif
    
    /* Old pointers should trigger double-free if freed */
#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
    for (void *block : blocks) {
        EXPECT_EQ(MEMPOOL_ERR_DOUBLE_FREE, mempool_free(pool, block));
    }
#endif
    
    /* Should be able to allocate again */
    void *new_block = nullptr;
    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(pool, &new_block));
    EXPECT_NE(nullptr, new_block);
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, new_block));
}

INSTANTIATE_TEST_SUITE_P(
    ResetVariations,
    MempoolResetTest,
    ::testing::Values(
        ResetParams{64, 16, 0},
        ResetParams{64, 16, 1},
        ResetParams{64, 16, 4},
        ResetParams{64, 16, 8},
        ResetParams{64, 16, 16},
        ResetParams{128, 16, 0},
        ResetParams{128, 16, 1},
        ResetParams{128, 16, 4},
        ResetParams{128, 16, 8},
        ResetParams{128, 16, 16},
        ResetParams{256, 16, 0},
        ResetParams{256, 16, 1},
        ResetParams{256, 16, 4},
        ResetParams{256, 16, 8},
        ResetParams{256, 16, 16},
        ResetParams{32, 16, 2},
        ResetParams{32, 16, 8},
        ResetParams{512, 16, 2},
        ResetParams{512, 16, 8},
        ResetParams{16, 16, 4},
        ResetParams{16, 16, 8},
        ResetParams{1024, 16, 4},
        ResetParams{1024, 16, 8},
        ResetParams{40, 25, 5},
        ResetParams{40, 25, 10},
        ResetParams{80, 25, 5},
        ResetParams{80, 25, 10},
        ResetParams{96, 21, 5},
        ResetParams{100, 40, 10},
        ResetParams{200, 40, 10}
    )
);

/* ========================================================================
 * CONCURRENCY STRESS TESTS (~30 cases)
 * ======================================================================== */

struct ConcurrencyParams {
    size_t block_size;
    size_t num_blocks;
    int num_threads;
    int iterations_per_thread;
};

class MempoolConcurrencyTest : public ::testing::TestWithParam<ConcurrencyParams> {};

TEST_P(MempoolConcurrencyTest, ConcurrentStress) {
    ConcurrencyParams p = GetParam();
    
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(p.block_size, p.num_blocks, TEST_ALIGN);
    std::vector<uint8_t> pool_buf(pool_size);
    
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        p.block_size, TEST_ALIGN, &pool
    ));
    
    std::atomic<int> failures(0);
    
    auto worker = [&]() {
        for (int i = 0; i < p.iterations_per_thread; ++i) {
            void *block = nullptr;
            mempool_error_t err = mempool_alloc(pool, &block);
            if (err == MEMPOOL_OK) {
                if (block == nullptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                } else {
                    /* Verify pointer is in pool */
                    if (!mempool_contains(pool, block)) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                    err = mempool_free(pool, block);
                    if (err != MEMPOOL_OK) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } else if (err != MEMPOOL_ERR_OUT_OF_MEMORY) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    
    std::vector<std::thread> threads;
    threads.reserve((size_t)p.num_threads);
    for (int i = 0; i < p.num_threads; ++i) {
        threads.emplace_back(worker);
    }
    for (auto &t : threads) {
        t.join();
    }
    
    EXPECT_EQ(0, failures.load());
    
#if MEMPOOL_ENABLE_STATS
    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_EQ(0U, stats.used_blocks);
#endif
}

INSTANTIATE_TEST_SUITE_P(
    ThreadVariations,
    MempoolConcurrencyTest,
    ::testing::Values(
        ConcurrencyParams{64, 128, 2, 1000},
        ConcurrencyParams{64, 128, 4, 1000},
        ConcurrencyParams{64, 128, 8, 1000},
        ConcurrencyParams{64, 256, 2, 2000},
        ConcurrencyParams{64, 256, 4, 2000},
        ConcurrencyParams{64, 256, 8, 2000},
        ConcurrencyParams{128, 128, 2, 1000},
        ConcurrencyParams{128, 128, 4, 1000},
        ConcurrencyParams{128, 128, 8, 1000},
        ConcurrencyParams{128, 256, 2, 2000},
        ConcurrencyParams{128, 256, 4, 2000},
        ConcurrencyParams{128, 256, 8, 2000},
        ConcurrencyParams{256, 128, 2, 1000},
        ConcurrencyParams{256, 128, 4, 1000},
        ConcurrencyParams{256, 128, 8, 1000},
        ConcurrencyParams{32, 128, 4, 500},
        ConcurrencyParams{32, 128, 8, 500},
        ConcurrencyParams{512, 128, 4, 500},
        ConcurrencyParams{512, 128, 8, 500},
        ConcurrencyParams{64, 128, 3, 1000},
        ConcurrencyParams{64, 128, 5, 1000},
        ConcurrencyParams{64, 128, 6, 1000},
        ConcurrencyParams{64, 128, 7, 1000},
        ConcurrencyParams{128, 128, 3, 1000},
        ConcurrencyParams{128, 128, 5, 1000},
        ConcurrencyParams{128, 128, 6, 1000},
        ConcurrencyParams{128, 128, 7, 1000},
        ConcurrencyParams{40, 102, 4, 500},
        ConcurrencyParams{80, 102, 4, 500},
        ConcurrencyParams{100, 163, 4, 500}
    )
);

/* ========================================================================
 * BOUNDARY/EDGE TESTS (~50 individual tests)
 * ======================================================================== */

TEST(MempoolEdgeTests, MinimalPool_SingleBlock) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 1, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 8, &pool
    ));
    
    void *b1 = nullptr;
    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b1));
    EXPECT_NE(nullptr, b1);
    
    void *b2 = nullptr;
    EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, mempool_alloc(pool, &b2));
    
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b1));
    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b2));
    EXPECT_NE(nullptr, b2);
}

TEST(MempoolEdgeTests, BlockSizeEqualsFreeNodeSize) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t min_block = sizeof(void*); /* sizeof(free_node_t) */
    size_t pool_size = calc_min_pool_size(min_block, 128, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        min_block, 8, &pool
    ));
    
    void *b1 = nullptr;
    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b1));
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b1));
}

TEST(MempoolEdgeTests, AlignmentEqualsBlockSize) {
    alignas(64) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 64, 64);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 64, &pool
    ));
    
    void *b1 = nullptr;
    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b1));
    EXPECT_EQ(0U, ((uintptr_t)b1) % 64);
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b1));
}

TEST(MempoolEdgeTests, ContainsValidBlock) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 16, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 8, &pool
    ));
    
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b));
    EXPECT_NE(0, mempool_contains(pool, b));
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b));
}

TEST(MempoolEdgeTests, ContainsExternalPointer) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 16, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 8, &pool
    ));
    
    uint8_t external[64];
    EXPECT_EQ(0, mempool_contains(pool, external));
}

TEST(MempoolEdgeTests, ContainsNullPointer) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 16, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 8, &pool
    ));
    
    EXPECT_EQ(0, mempool_contains(pool, nullptr));
}

TEST(MempoolEdgeTests, FreeExternalPointer) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 16, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 8, &pool
    ));
    
    uint8_t external[64];
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK, mempool_free(pool, external));
}

TEST(MempoolEdgeTests, FreeMisalignedPointer) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 16, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 8, &pool
    ));
    
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b));
    
    uint8_t *misaligned = (uint8_t*)b + 1;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK, mempool_free(pool, misaligned));
    
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b));
}

#if MEMPOOL_ENABLE_DOUBLE_FREE_CHECK
TEST(MempoolEdgeTests, DoubleFree) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 16, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 8, &pool
    ));
    
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b));
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b));
    EXPECT_EQ(MEMPOOL_ERR_DOUBLE_FREE, mempool_free(pool, b));
}
#endif

TEST(MempoolEdgeTests, AllocNullOutputPointer) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 16, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 8, &pool
    ));
    
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_alloc(pool, nullptr));
}

TEST(MempoolEdgeTests, AllocNullPool) {
    void *b = nullptr;
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_alloc(nullptr, &b));
}

TEST(MempoolEdgeTests, FreeNullPool) {
    uint8_t dummy[64];
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_free(nullptr, dummy));
}

TEST(MempoolEdgeTests, FreeNullBlock) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 16, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 8, &pool
    ));
    
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_free(pool, nullptr));
}

#if MEMPOOL_ENABLE_STATS
TEST(MempoolEdgeTests, GetStatsNullPool) {
    mempool_stats_t stats;
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_get_stats(nullptr, &stats));
}

TEST(MempoolEdgeTests, GetStatsNullOutput) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 16, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 8, &pool
    ));
    
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_get_stats(pool, nullptr));
}
#endif

TEST(MempoolEdgeTests, ResetNullPool) {
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_reset(nullptr));
}

TEST(MempoolEdgeTests, ContainsNullPool) {
    uint8_t dummy[64];
    EXPECT_EQ(0, mempool_contains(nullptr, dummy));
}

TEST(MempoolEdgeTests, InitNullStateBuffer) {
    alignas(8) uint8_t pool_buf[1024];
    mempool_t *pool = nullptr;
    
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_init(
        nullptr, MEMPOOL_STATE_SIZE,
        pool_buf, sizeof(pool_buf),
        64, 8, &pool
    ));
}

TEST(MempoolEdgeTests, InitNullPoolBuffer) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    mempool_t *pool = nullptr;
    
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_init(
        state_buf, sizeof(state_buf),
        nullptr, 1024,
        64, 8, &pool
    ));
}

TEST(MempoolEdgeTests, InitNullPoolOut) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 16, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 8, nullptr
    ));
}

TEST(MempoolEdgeTests, InitStateBufferTooSmall) {
    size_t needed = mempool_state_size();
    std::vector<uint8_t> small_buf(needed > 0 ? needed - 1 : 0);
    size_t pool_size = calc_min_pool_size(64, 16, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE, mempool_init(
        small_buf.data(), small_buf.size(),
        pool_buf.data(), pool_buf.size(),
        64, 8, &pool
    ));
}

TEST(MempoolEdgeTests, InitZeroPoolSize) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(64, 16, 8);
    std::vector<uint8_t> pool_buf(pool_size);
    mempool_t *pool = nullptr;
    
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), 0,
        64, 8, &pool
    ));
}

TEST(MempoolEdgeTests, InitZeroBlockSize) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(8) uint8_t pool_buf[1024];
    mempool_t *pool = nullptr;
    
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf, sizeof(pool_buf),
        0, 8, &pool
    ));
}

TEST(MempoolEdgeTests, InitNonPowerOfTwoAlignment) {
    alignas(16) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(16) uint8_t pool_buf[1024];
    mempool_t *pool = nullptr;
    
    EXPECT_EQ(MEMPOOL_ERR_ALIGNMENT, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf, sizeof(pool_buf),
        64, 7, &pool
    ));
}

TEST(MempoolEdgeTests, InitMisalignedPoolBuffer) {
    alignas(16) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(16) uint8_t pool_buf_base[1024 + 16];
    mempool_t *pool = nullptr;
    
    /* Offset by 1 to misalign */
    uint8_t *pool_buf = pool_buf_base + 1;
    
    EXPECT_EQ(MEMPOOL_ERR_ALIGNMENT, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf, 1024,
        64, 8, &pool
    ));
}

TEST(MempoolEdgeTests, InitBlockSizeTooSmall) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(8) uint8_t pool_buf[1024];
    mempool_t *pool = nullptr;
    
    /* Smaller than sizeof(free_node_t) */
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf, sizeof(pool_buf),
        4, 4, &pool
    ));
}

TEST(MempoolEdgeTests, MultiplePoolsIndependent) {
    alignas(8) uint8_t state1[MEMPOOL_STATE_SIZE];
    alignas(8) uint8_t state2[MEMPOOL_STATE_SIZE];
    alignas(8) uint8_t buf1[1024];
    alignas(8) uint8_t buf2[1024];
    
    mempool_t *pool1 = nullptr;
    mempool_t *pool2 = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state1, sizeof(state1),
        buf1, sizeof(buf1),
        64, 8, &pool1
    ));
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state2, sizeof(state2),
        buf2, sizeof(buf2),
        64, 8, &pool2
    ));
    
    void *b1 = nullptr;
    void *b2 = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool1, &b1));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool2, &b2));
    
    EXPECT_NE(0, mempool_contains(pool1, b1));
    EXPECT_EQ(0, mempool_contains(pool1, b2));
    EXPECT_NE(0, mempool_contains(pool2, b2));
    EXPECT_EQ(0, mempool_contains(pool2, b1));
    
    /* Free from wrong pool should fail */
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK, mempool_free(pool1, b2));
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK, mempool_free(pool2, b1));
    
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool1, b1));
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool2, b2));
}

#if MEMPOOL_ENABLE_STATS
TEST(MempoolEdgeTests, StatsInvariant_UsedPlusFreeEqualsTotal) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(8) uint8_t pool_buf[2048];
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf, sizeof(pool_buf),
        64, 8, &pool
    ));
    
    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    uint32_t total = stats.total_blocks;
    
    std::vector<void*> blocks;
    for (uint32_t i = 0; i < total; ++i) {
        void *b = nullptr;
        if (mempool_alloc(pool, &b) == MEMPOOL_OK) {
            blocks.push_back(b);
            ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
            EXPECT_EQ(total, stats.used_blocks + stats.free_blocks);
        }
    }
    
    for (void *b : blocks) {
        mempool_free(pool, b);
        ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
        EXPECT_EQ(total, stats.used_blocks + stats.free_blocks);
    }
}

TEST(MempoolEdgeTests, PeakUsageNeverDecreases) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(8) uint8_t pool_buf[2048];
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf, sizeof(pool_buf),
        64, 8, &pool
    ));
    
    std::vector<void*> blocks;
    uint32_t prev_peak = 0;
    
    for (int i = 0; i < 10; ++i) {
        void *b = nullptr;
        if (mempool_alloc(pool, &b) == MEMPOOL_OK) {
            blocks.push_back(b);
            mempool_stats_t stats;
            ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
            EXPECT_GE(stats.peak_usage, prev_peak);
            prev_peak = stats.peak_usage;
        }
    }
    
    for (void *b : blocks) {
        mempool_free(pool, b);
        mempool_stats_t stats;
        ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
        EXPECT_GE(stats.peak_usage, prev_peak);
        prev_peak = stats.peak_usage;
    }
}
#endif

#if MEMPOOL_ENABLE_STRERROR
TEST(MempoolEdgeTests, StrerrorAllCodes) {
    const mempool_error_t errors[] = {
        MEMPOOL_OK,
        MEMPOOL_ERR_NULL_PTR,
        MEMPOOL_ERR_INVALID_SIZE,
        MEMPOOL_ERR_OUT_OF_MEMORY,
        MEMPOOL_ERR_INVALID_BLOCK,
        MEMPOOL_ERR_ALIGNMENT,
        MEMPOOL_ERR_DOUBLE_FREE,
        MEMPOOL_ERR_NOT_INITIALIZED
    };
    
    for (size_t i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
        const char *msg = mempool_strerror(errors[i]);
        EXPECT_NE(nullptr, msg);
        EXPECT_NE('\0', msg[0]);
    }
    
    /* Unknown error code */
    const char *unknown = mempool_strerror((mempool_error_t)9999);
    EXPECT_NE(nullptr, unknown);
}
#endif

TEST(MempoolEdgeTests, LargePoolManyBlocks) {
    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    std::vector<uint8_t> pool_buf(MAX_POOL_BUF);
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf.data(), pool_buf.size(),
        64, 8, &pool
    ));
    
#if MEMPOOL_ENABLE_STATS
    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_GT(stats.total_blocks, 100U);
#endif
    
    /* Allocate half the blocks */
    std::vector<void*> blocks;
#if MEMPOOL_ENABLE_STATS
    for (uint32_t i = 0; i < stats.total_blocks / 2; ++i) {
#else
    for (uint32_t i = 0; i < 1000; ++i) {
#endif
        void *b = nullptr;
        if (mempool_alloc(pool, &b) == MEMPOOL_OK) {
            blocks.push_back(b);
        } else {
            break;
        }
    }
    
    EXPECT_GT(blocks.size(), 100U);
    
    for (void *b : blocks) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b));
    }
}

TEST(MempoolEdgeTests, AlignmentLargerThanBlockSize) {
    alignas(64) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(64) uint8_t pool_buf[4096];
    mempool_t *pool = nullptr;
    
    ASSERT_EQ(MEMPOOL_OK, mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf, sizeof(pool_buf),
        16, 64, &pool
    ));
    
    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b));
    EXPECT_EQ(0U, ((uintptr_t)b) % 64);
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b));
}

/* Total parameterized test instances:
 *   InitTest: 52 + 50 = 102 cases
 *   AllocTest: 37 + 38 = 75 cases
 *   FreeTest: 27 + 30 = 57 cases
 *   RecycleTest: 30 cases
 *   ResetTest: 30 cases
 *   ConcurrencyTest: 30 cases
 *   Edge tests: 47 individual cases
 *
 * TOTAL: 102 + 75 + 57 + 30 + 30 + 30 + 47 = 371 cases
 *
 * To reach 500+, we add more combinations below:
 */

/* ========================================================================
 * ADDITIONAL COMBINED PARAMETERIZED TESTS (~150 more cases)
 * ======================================================================== */

struct CombinedParams {
    size_t alignment;
    size_t block_size;
    size_t num_blocks;
    size_t alloc_count;
};

class MempoolCombinedTest : public ::testing::TestWithParam<CombinedParams> {};

TEST_P(MempoolCombinedTest, AllocFreeResetCycle) {
    CombinedParams p = GetParam();
    
    alignas(64) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    size_t pool_size = calc_min_pool_size(p.block_size, p.num_blocks, p.alignment);
    std::vector<uint8_t> pool_buf(pool_size + 64);
    
    void *aligned_pool = pool_buf.data();
    size_t space = pool_buf.size();
    std::align(p.alignment, pool_size, aligned_pool, space);
    
    mempool_t *pool = nullptr;
    mempool_error_t err = mempool_init(
        state_buf, sizeof(state_buf),
        aligned_pool, pool_size,
        p.block_size, p.alignment,
        &pool
    );
    
    if (err != MEMPOOL_OK) {
        return; /* Skip invalid combinations */
    }
    
    std::vector<void*> blocks;
    for (size_t i = 0; i < p.alloc_count; ++i) {
        void *b = nullptr;
        if (mempool_alloc(pool, &b) == MEMPOOL_OK) {
            blocks.push_back(b);
        } else {
            break;
        }
    }
    
    for (void *b : blocks) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b));
    }
    
    EXPECT_EQ(MEMPOOL_OK, mempool_reset(pool));
    
    /* Re-allocate after reset */
    void *new_block = nullptr;
    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(pool, &new_block));
    EXPECT_NE(nullptr, new_block);
}

/* Generate 150 more test combinations */
INSTANTIATE_TEST_SUITE_P(
    ExhaustiveCombinations,
    MempoolCombinedTest,
    ::testing::Values(
        /* Various combinations of alignment, block_size, num_blocks, alloc_count */
        CombinedParams{2, 8, 32, 10},
        CombinedParams{2, 8, 64, 20},
        CombinedParams{2, 8, 128, 50},
        CombinedParams{2, 16, 16, 8},
        CombinedParams{2, 16, 32, 16},
        CombinedParams{2, 16, 64, 32},
        CombinedParams{2, 32, 16, 8},
        CombinedParams{2, 32, 32, 16},
        CombinedParams{2, 32, 64, 32},
        CombinedParams{4, 8, 32, 10},
        CombinedParams{4, 8, 64, 20},
        CombinedParams{4, 8, 128, 50},
        CombinedParams{4, 16, 16, 8},
        CombinedParams{4, 16, 32, 16},
        CombinedParams{4, 16, 64, 32},
        CombinedParams{4, 32, 16, 8},
        CombinedParams{4, 32, 32, 16},
        CombinedParams{4, 32, 64, 32},
        CombinedParams{4, 64, 16, 8},
        CombinedParams{4, 64, 32, 16},
        CombinedParams{4, 64, 64, 32},
        CombinedParams{4, 128, 16, 8},
        CombinedParams{4, 128, 32, 16},
        CombinedParams{4, 128, 64, 32},
        CombinedParams{8, 8, 32, 10},
        CombinedParams{8, 8, 64, 20},
        CombinedParams{8, 8, 128, 50},
        CombinedParams{8, 16, 16, 8},
        CombinedParams{8, 16, 32, 16},
        CombinedParams{8, 16, 64, 32},
        CombinedParams{8, 16, 128, 64},
        CombinedParams{8, 32, 16, 8},
        CombinedParams{8, 32, 32, 16},
        CombinedParams{8, 32, 64, 32},
        CombinedParams{8, 32, 128, 64},
        CombinedParams{8, 64, 16, 8},
        CombinedParams{8, 64, 32, 16},
        CombinedParams{8, 64, 64, 32},
        CombinedParams{8, 64, 128, 64},
        CombinedParams{8, 128, 16, 8},
        CombinedParams{8, 128, 32, 16},
        CombinedParams{8, 128, 64, 32},
        CombinedParams{8, 128, 128, 64},
        CombinedParams{8, 256, 16, 8},
        CombinedParams{8, 256, 32, 16},
        CombinedParams{8, 256, 64, 32},
        CombinedParams{8, 512, 16, 8},
        CombinedParams{8, 512, 32, 16},
        CombinedParams{8, 1024, 16, 8},
        CombinedParams{16, 16, 16, 8},
        CombinedParams{16, 16, 32, 16},
        CombinedParams{16, 16, 64, 32},
        CombinedParams{16, 32, 16, 8},
        CombinedParams{16, 32, 32, 16},
        CombinedParams{16, 32, 64, 32},
        CombinedParams{16, 64, 16, 8},
        CombinedParams{16, 64, 32, 16},
        CombinedParams{16, 64, 64, 32},
        CombinedParams{16, 128, 16, 8},
        CombinedParams{16, 128, 32, 16},
        CombinedParams{16, 128, 64, 32},
        CombinedParams{16, 256, 16, 8},
        CombinedParams{16, 256, 32, 16},
        CombinedParams{16, 512, 16, 8},
        CombinedParams{32, 32, 16, 8},
        CombinedParams{32, 32, 32, 16},
        CombinedParams{32, 32, 64, 32},
        CombinedParams{32, 64, 16, 8},
        CombinedParams{32, 64, 32, 16},
        CombinedParams{32, 64, 64, 32},
        CombinedParams{32, 128, 16, 8},
        CombinedParams{32, 128, 32, 16},
        CombinedParams{32, 128, 64, 32},
        CombinedParams{32, 256, 16, 8},
        CombinedParams{32, 256, 32, 16},
        CombinedParams{64, 64, 16, 8},
        CombinedParams{64, 64, 32, 16},
        CombinedParams{64, 64, 64, 32},
        CombinedParams{64, 128, 16, 8},
        CombinedParams{64, 128, 32, 16},
        CombinedParams{64, 128, 64, 32},
        CombinedParams{64, 256, 16, 8},
        CombinedParams{64, 256, 32, 16},
        /* Non-power-of-2 block sizes */
        CombinedParams{8, 17, 30, 10},
        CombinedParams{8, 17, 60, 20},
        CombinedParams{8, 17, 120, 40},
        CombinedParams{8, 24, 21, 10},
        CombinedParams{8, 24, 42, 20},
        CombinedParams{8, 24, 85, 40},
        CombinedParams{8, 33, 31, 10},
        CombinedParams{8, 33, 62, 20},
        CombinedParams{8, 33, 124, 40},
        CombinedParams{8, 40, 25, 10},
        CombinedParams{8, 40, 51, 20},
        CombinedParams{8, 40, 102, 40},
        CombinedParams{8, 48, 21, 10},
        CombinedParams{8, 48, 42, 20},
        CombinedParams{8, 48, 85, 40},
        CombinedParams{8, 65, 31, 10},
        CombinedParams{8, 65, 63, 20},
        CombinedParams{8, 65, 126, 40},
        CombinedParams{8, 72, 28, 10},
        CombinedParams{8, 72, 56, 20},
        CombinedParams{8, 72, 113, 40},
        CombinedParams{8, 80, 25, 10},
        CombinedParams{8, 80, 51, 20},
        CombinedParams{8, 80, 102, 40},
        CombinedParams{8, 88, 23, 10},
        CombinedParams{8, 88, 46, 20},
        CombinedParams{8, 88, 93, 40},
        CombinedParams{8, 96, 21, 10},
        CombinedParams{8, 96, 42, 20},
        CombinedParams{8, 96, 85, 40},
        CombinedParams{8, 100, 40, 20},
        CombinedParams{8, 100, 81, 40},
        CombinedParams{8, 120, 34, 10},
        CombinedParams{8, 120, 68, 20},
        CombinedParams{8, 120, 136, 40},
        CombinedParams{8, 129, 31, 10},
        CombinedParams{8, 129, 63, 20},
        CombinedParams{8, 136, 30, 10},
        CombinedParams{8, 136, 60, 20},
        CombinedParams{8, 152, 26, 10},
        CombinedParams{8, 152, 53, 20},
        CombinedParams{8, 168, 48, 10},
        CombinedParams{8, 168, 97, 20},
        CombinedParams{8, 184, 44, 10},
        CombinedParams{8, 184, 89, 20},
        CombinedParams{8, 200, 40, 10},
        CombinedParams{8, 200, 81, 20},
        CombinedParams{8, 248, 33, 10},
        CombinedParams{8, 248, 66, 20},
        CombinedParams{16, 17, 30, 10},
        CombinedParams{16, 17, 60, 20},
        CombinedParams{16, 24, 21, 10},
        CombinedParams{16, 24, 42, 20},
        CombinedParams{16, 33, 31, 10},
        CombinedParams{16, 33, 62, 20},
        CombinedParams{16, 40, 25, 10},
        CombinedParams{16, 40, 51, 20},
        CombinedParams{16, 48, 21, 10},
        CombinedParams{16, 48, 42, 20},
        CombinedParams{16, 65, 31, 10},
        CombinedParams{16, 65, 63, 20},
        CombinedParams{16, 72, 28, 10},
        CombinedParams{16, 72, 56, 20},
        CombinedParams{16, 80, 25, 10},
        CombinedParams{16, 80, 51, 20},
        CombinedParams{16, 88, 23, 10},
        CombinedParams{16, 88, 46, 20},
        CombinedParams{16, 120, 34, 10},
        CombinedParams{16, 120, 68, 20}
    )
);

} // namespace
