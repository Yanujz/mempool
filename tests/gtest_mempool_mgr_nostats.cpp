/*
 * Minimal regression test for mempool_mgr compiled with MEMPOOL_ENABLE_STATS=0.
 *
 * This file intentionally does NOT call mempool_get_stats() or
 * mempool_reset_stats() — it verifies that the pool manager and the
 * always-available query API (mempool_block_size, mempool_capacity) build and
 * work correctly without the statistics subsystem.
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>

static std::mutex g_mempool_mtx;
static std::mutex g_mempool_isr_mtx;
extern "C" {
void mempool_test_lock(void)       { g_mempool_mtx.lock();       }
void mempool_test_unlock(void)     { g_mempool_mtx.unlock();     }
void mempool_test_isr_lock(void)   { g_mempool_isr_mtx.lock();   }
void mempool_test_isr_unlock(void) { g_mempool_isr_mtx.unlock(); }
#include "mempool.h"
#include "mempool_mgr.h"
}

namespace {

static constexpr size_t pool_buf_for_nostats(size_t block_size, size_t n)
{
    size_t stride = ((block_size + 4U + 7U) & ~7U);
    size_t tags   = ((n * 4U + 7U) & ~7U);
    size_t bmp    = (((n + 7U) / 8U + 7U) & ~7U);
    return bmp + tags + n * stride + 64U;
}

static mempool_t *make_pool(void *state, size_t ssz,
                             void *pbuf,  size_t psz,
                             size_t bs)
{
    mempool_t *p = nullptr;
    if (mempool_init(state, ssz, pbuf, psz, bs, 8U, &p) != MEMPOOL_OK) {
        return nullptr;
    }
    return p;
}

/* -----------------------------------------------------------------------
 * mempool_block_size / mempool_capacity — no-stats build
 * -------------------------------------------------------------------- */

TEST(NoStatsBlockInfo, BlockSizeAndCapacityReturnSaneValues) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for_nostats(32U, 4U)]{};

    mempool_t *pool = make_pool(state, sizeof state, pbuf, sizeof pbuf, 32U);
    ASSERT_NE(nullptr, pool);

    EXPECT_GE(mempool_block_size(pool), 32U);
    EXPECT_GE(mempool_capacity(pool),   4U);
    EXPECT_EQ(0U, mempool_block_size(nullptr));
    EXPECT_EQ(0U, mempool_capacity(nullptr));
}

/* -----------------------------------------------------------------------
 * mempool_mgr — no-stats build (exercises the mgr_sort + alloc path that
 * previously called mempool_get_stats() unconditionally)
 * -------------------------------------------------------------------- */

class MgrNoStatsTest : public ::testing::Test {
protected:
    alignas(8) uint8_t s0[MEMPOOL_STATE_SIZE]{}, s1[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t p0[pool_buf_for_nostats(32U,  4U)]{};
    alignas(8) uint8_t p1[pool_buf_for_nostats(128U, 2U)]{};

    mempool_t *pool0 = nullptr, *pool1 = nullptr;
    mempool_mgr_t mgr{};

    void SetUp() override {
        pool0 = make_pool(s0, sizeof s0, p0, sizeof p0, 32U);
        pool1 = make_pool(s1, sizeof s1, p1, sizeof p1, 128U);
        ASSERT_NE(nullptr, pool0);
        ASSERT_NE(nullptr, pool1);

        mempool_t *pools[2] = { pool1, pool0 }; /* deliberately reversed */
        ASSERT_EQ(MEMPOOL_OK, mempool_mgr_init(&mgr, pools, 2U));
    }
};

TEST_F(MgrNoStatsTest, SmallAllocRoutedToSmallPool) {
    void *blk = nullptr; mempool_t *owner = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 1U, &blk, &owner));
    EXPECT_EQ(pool0, owner);
    EXPECT_EQ(MEMPOOL_OK, mempool_mgr_free(&mgr, blk));
}

TEST_F(MgrNoStatsTest, LargeAllocRoutedToLargePool) {
    void *blk = nullptr; mempool_t *owner = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 64U, &blk, &owner));
    EXPECT_EQ(pool1, owner);
    EXPECT_EQ(MEMPOOL_OK, mempool_mgr_free(&mgr, blk));
}

TEST_F(MgrNoStatsTest, TooLargeReturnsInvalidSize) {
    void *blk = nullptr;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE,
              mempool_mgr_alloc(&mgr, 512U, &blk, nullptr));
}

TEST_F(MgrNoStatsTest, FallbackWhenSmallExhausted) {
    /* Drain the 32-byte pool */
    std::vector<void*> small;
    void *b = nullptr; mempool_t *owner = nullptr;
    while (mempool_mgr_alloc(&mgr, 1U, &b, &owner) == MEMPOOL_OK &&
           owner == pool0) {
        small.push_back(b); b = nullptr;
    }
    if (b) { (void)mempool_mgr_free(&mgr, b); }

    /* Next 1-byte alloc must fall through to pool1 */
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 1U, &b, &owner));
    EXPECT_EQ(pool1, owner);
    (void)mempool_mgr_free(&mgr, b);

    for (auto sb : small) { (void)mempool_mgr_free(&mgr, sb); }
}

} /* anonymous namespace */
