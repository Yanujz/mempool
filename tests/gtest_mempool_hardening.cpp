/*
 * Hardening test suite for mempool v0.4.0
 *
 * Exercises all production-grade features added in 0.4.0:
 *   - Guard canary write / validate / violation detection
 *   - Poison fill on alloc and free
 *   - OOM hook invocation
 *   - ISR-safe deferred free queue
 *   - Per-block caller tags
 *   - mempool_alloc_zero
 *   - mempool_reset_stats (sticky peak)
 *   - Pool-of-pools manager (mempool_mgr)
 *
 * All features are compiled IN for this test binary (see CMakeLists.txt).
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
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

/* -----------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------- */

/* Build a valid pool; alignment 8, block_size as specified.
 * Returns pointers into the supplied buffers (which must outlive the pool). */
static mempool_t *make_pool(void        *state, size_t state_sz,
                             void        *pbuf,  size_t pbuf_sz,
                             size_t       block_size)
{
    mempool_t *pool = nullptr;
    EXPECT_EQ(MEMPOOL_OK, mempool_init(state, state_sz,
                                       pbuf,  pbuf_sz,
                                       block_size, 8U, &pool));
    return pool;
}

/* Compute pool buffer size needed for exactly N blocks (with guard + tags
 * overhead handled by mempool_init's layout algorithm — we over-provision
 * by 2× to guarantee N blocks always fit regardless of feature overhead). */
static constexpr size_t pool_buf_for(size_t block_size, size_t n_blocks)
{
    /* stride with guard overhead = block_size + 4 (canary), aligned to 8 */
    size_t stride = ((block_size + 4U + 7U) & ~7U);
    /* tags overhead = n_blocks * 4 bytes, rounded to 8 */
    size_t tags   = ((n_blocks * 4U + 7U) & ~7U);
    /* bitmap overhead = ceil(n_blocks/8), rounded to 8 */
    size_t bmp    = (((n_blocks + 7U) / 8U + 7U) & ~7U);
    return bmp + tags + n_blocks * stride + 64U; /* +64 for alignment headroom */
}

/* -----------------------------------------------------------------------
 * Guard canary tests
 * -------------------------------------------------------------------- */

class GuardTest : public ::testing::Test {
protected:
    static constexpr size_t BLOCK  = 32U;
    static constexpr size_t NBLK   = 8U;

    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE]{};
    std::vector<uint8_t> pool_buf;
    mempool_t *pool = nullptr;

    void SetUp() override {
        pool_buf.assign(pool_buf_for(BLOCK, NBLK), 0U);
        /* align pool buffer to 8 */
        pool_buf.resize(pool_buf.size() + 8U);
        auto *raw = pool_buf.data();
        size_t space = pool_buf.size();
        void *aligned = nullptr;
        std::align(8U, pool_buf.size() - 8U, reinterpret_cast<void*&>(raw), space);
        aligned = raw;
        pool = make_pool(state_buf, sizeof state_buf,
                         aligned, pool_buf.size() - 8U, BLOCK);
        ASSERT_NE(nullptr, pool);
    }
};

TEST_F(GuardTest, CanaryWrittenOnAlloc) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));
    ASSERT_NE(nullptr, blk);

    /* Canary must be at byte offset BLOCK from the block start */
    uint32_t canary;
    std::memcpy(&canary, static_cast<uint8_t*>(blk) + BLOCK, sizeof canary);
    EXPECT_EQ(static_cast<uint32_t>(MEMPOOL_CANARY_VALUE), canary);
}

TEST_F(GuardTest, FreeAcceptsIntactCanary) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, blk));
}

TEST_F(GuardTest, FreeRejectsCorruptedCanary) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    /* Corrupt the post-canary (simulates a 1-byte overrun past BLOCK bytes) */
    uint32_t bad = 0xDEAD1234U;
    std::memcpy(static_cast<uint8_t*>(blk) + BLOCK, &bad, sizeof bad);

    EXPECT_EQ(MEMPOOL_ERR_GUARD_CORRUPTED, mempool_free(pool, blk));
}

TEST_F(GuardTest, ViolationCountedInStats) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    uint32_t bad = 0U;
    std::memcpy(static_cast<uint8_t*>(blk) + BLOCK, &bad, sizeof bad);
    (void)mempool_free(pool, blk); /* ignore GUARD_CORRUPTED return */

    mempool_stats_t st{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &st));
    EXPECT_EQ(1U, st.guard_violations);
}

TEST_F(GuardTest, StrerrorGuardCorrupted) {
    const char *s = mempool_strerror(MEMPOOL_ERR_GUARD_CORRUPTED);
    ASSERT_NE(nullptr, s);
    EXPECT_NE(std::string("Unknown error"), std::string(s));
}

/* -----------------------------------------------------------------------
 * Poison fill tests
 * -------------------------------------------------------------------- */

class PoisonTest : public ::testing::Test {
protected:
    static constexpr size_t BLOCK = 32U;
    static constexpr size_t NBLK  = 4U;

    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE]{};
    std::vector<uint8_t> pool_buf;
    mempool_t *pool = nullptr;

    void SetUp() override {
        pool_buf.assign(pool_buf_for(BLOCK, NBLK) + 8U, 0U);
        auto *raw = pool_buf.data();
        size_t space = pool_buf.size();
        std::align(8U, pool_buf.size() - 8U, reinterpret_cast<void*&>(raw), space);
        pool = make_pool(state_buf, sizeof state_buf,
                         raw, pool_buf.size() - 8U, BLOCK);
        ASSERT_NE(nullptr, pool);
    }
};

TEST_F(PoisonTest, AllocFillsWithAllocPattern) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    auto *bytes = static_cast<uint8_t*>(blk);
    for (size_t i = 0U; i < BLOCK; i++) {
        EXPECT_EQ(MEMPOOL_ALLOC_POISON_BYTE, bytes[i])
            << "Alloc pattern mismatch at byte " << i;
    }
}

TEST_F(PoisonTest, FreeFillsWithFreePattern) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    /* Write something to make sure free overwrites it */
    std::memset(blk, 0xAB, BLOCK);

    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool, blk));

    /* After free, the free-list next pointer occupies the first sizeof(void*)
     * bytes of the block (intrusive linked list).  Check the remaining bytes
     * for the FREE pattern. */
    auto *bytes = static_cast<uint8_t*>(blk);
    for (size_t i = sizeof(void*); i < BLOCK; i++) {
        EXPECT_EQ(MEMPOOL_FREE_POISON_BYTE, bytes[i])
            << "Free pattern mismatch at byte " << i;
    }
}

TEST_F(PoisonTest, AllocZeroOverridesPoisonFill) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_zero(pool, &blk));

    auto *bytes = static_cast<uint8_t*>(blk);
    for (size_t i = 0U; i < BLOCK; i++) {
        EXPECT_EQ(0U, bytes[i]) << "alloc_zero byte " << i << " not zero";
    }
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool, blk));
}

/* -----------------------------------------------------------------------
 * mempool_alloc_zero tests
 * -------------------------------------------------------------------- */

class AllocZeroTest : public ::testing::Test {
protected:
    static constexpr size_t BLOCK = 64U;

    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pool_buf[pool_buf_for(64, 16)]{};
    mempool_t *pool = nullptr;

    void SetUp() override {
        pool = make_pool(state_buf, sizeof state_buf,
                         pool_buf,  sizeof pool_buf, BLOCK);
        ASSERT_NE(nullptr, pool);
    }
};

TEST_F(AllocZeroTest, BlockIsZeroed) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_zero(pool, &blk));
    auto *b = static_cast<uint8_t*>(blk);
    for (size_t i = 0U; i < BLOCK; i++) { EXPECT_EQ(0U, b[i]); }
    (void)mempool_free(pool, blk);
}

TEST_F(AllocZeroTest, ExhaustedReturnsOOM) {
    std::vector<void*> blks;
    mempool_error_t err = MEMPOOL_OK;
    void *blk = nullptr;
    while ((err = mempool_alloc_zero(pool, &blk)) == MEMPOOL_OK) {
        blks.push_back(blk);
    }
    EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, err);
    for (auto b : blks) { (void)mempool_free(pool, b); }
}

/* -----------------------------------------------------------------------
 * OOM hook tests
 * -------------------------------------------------------------------- */

class OomHookTest : public ::testing::Test {
protected:
    static constexpr size_t BLOCK = 16U;

    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pool_buf[pool_buf_for(16, 2)]{};
    mempool_t *pool = nullptr;

    static int oom_call_count;
    static mempool_t *oom_pool_arg;
    static void *oom_userdata_arg;

    static void oom_cb(mempool_t *p, void *ud) {
        oom_call_count++;
        oom_pool_arg     = p;
        oom_userdata_arg = ud;
    }

    void SetUp() override {
        oom_call_count   = 0;
        oom_pool_arg     = nullptr;
        oom_userdata_arg = nullptr;
        pool = make_pool(state_buf, sizeof state_buf,
                         pool_buf,  sizeof pool_buf, BLOCK);
        ASSERT_NE(nullptr, pool);
    }
};
int        OomHookTest::oom_call_count   = 0;
mempool_t *OomHookTest::oom_pool_arg     = nullptr;
void      *OomHookTest::oom_userdata_arg = nullptr;

TEST_F(OomHookTest, HookCalledOnExhaustion) {
    int userdata = 42;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_set_oom_hook(pool, oom_cb, &userdata));

    /* Find out how many blocks the pool actually has — use mempool_capacity()
     * which works regardless of MEMPOOL_ENABLE_STATS. */
    uint32_t cap = mempool_capacity(pool);

    /* Exhaust all blocks */
    std::vector<void*> blks(cap, nullptr);
    for (uint32_t i = 0U; i < cap; i++) {
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blks[i]));
    }

    /* Next alloc must fail and invoke the hook */
    void *extra = nullptr;
    EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, mempool_alloc(pool, &extra));
    EXPECT_EQ(1, oom_call_count);
    EXPECT_EQ(pool, oom_pool_arg);
    EXPECT_EQ(static_cast<void*>(&userdata), oom_userdata_arg);

    for (auto b : blks) { if (b) (void)mempool_free(pool, b); }
}

TEST_F(OomHookTest, HookNotCalledOnSuccess) {
    ASSERT_EQ(MEMPOOL_OK, mempool_set_oom_hook(pool, oom_cb, nullptr));
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));
    EXPECT_EQ(0, oom_call_count);
    (void)mempool_free(pool, blk);
}

TEST_F(OomHookTest, HookCanBeCleared) {
    ASSERT_EQ(MEMPOOL_OK, mempool_set_oom_hook(pool, oom_cb, nullptr));
    ASSERT_EQ(MEMPOOL_OK, mempool_set_oom_hook(pool, nullptr, nullptr));

    void *b0 = nullptr, *b1 = nullptr, *b2 = nullptr;
    (void)mempool_alloc(pool, &b0);
    (void)mempool_alloc(pool, &b1);
    (void)mempool_alloc(pool, &b2); /* exhausted, no hook → no crash */
    EXPECT_EQ(0, oom_call_count);

    if (b0) (void)mempool_free(pool, b0);
    if (b1) (void)mempool_free(pool, b1);
}

TEST_F(OomHookTest, NullPoolReturnsNullPtr) {
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_set_oom_hook(nullptr, oom_cb, nullptr));
}

/* -----------------------------------------------------------------------
 * ISR deferred free tests
 * -------------------------------------------------------------------- */

class IsrFreeTest : public ::testing::Test {
protected:
    static constexpr size_t BLOCK = 32U;
    static constexpr size_t NBLK  = 16U;

    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE]{};
    std::vector<uint8_t> pool_buf_storage;
    mempool_t *pool = nullptr;

    void SetUp() override {
        pool_buf_storage.assign(pool_buf_for(BLOCK, NBLK) + 8U, 0U);
        auto *raw = pool_buf_storage.data();
        size_t space = pool_buf_storage.size();
        std::align(8U, pool_buf_storage.size() - 8U,
                   reinterpret_cast<void*&>(raw), space);
        pool = make_pool(state_buf, sizeof state_buf,
                         raw, pool_buf_storage.size() - 8U, BLOCK);
        ASSERT_NE(nullptr, pool);
    }
};

TEST_F(IsrFreeTest, DeferredFreeRestoresBlock) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    /* "ISR" queues the free */
    EXPECT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, blk));

    /* Block is NOT yet in pool (not drained) */
    void *blk2 = nullptr;

    /* After drain, block is back */
    EXPECT_EQ(MEMPOOL_OK, mempool_drain_isr_queue(pool));

    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk2));
    EXPECT_NE(nullptr, blk2);
    (void)mempool_free(pool, blk2);
}

TEST_F(IsrFreeTest, QueueFullReturnsError) {
    std::vector<void*> blks(MEMPOOL_ISR_QUEUE_CAPACITY + 1U, nullptr);
    for (auto &b : blks) {
        (void)mempool_alloc(pool, &b);
    }

    /* Fill queue to capacity */
    for (size_t i = 0U; i < MEMPOOL_ISR_QUEUE_CAPACITY; i++) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, blks[i]));
    }

    /* One more must fail */
    EXPECT_EQ(MEMPOOL_ERR_ISR_QUEUE_FULL,
              mempool_free_from_isr(pool, blks[MEMPOOL_ISR_QUEUE_CAPACITY]));

    /* Drain and clean up */
    ASSERT_EQ(MEMPOOL_OK, mempool_drain_isr_queue(pool));
    (void)mempool_free(pool, blks[MEMPOOL_ISR_QUEUE_CAPACITY]);
}

TEST_F(IsrFreeTest, AllocDrainsQueueImplicitly) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));
    ASSERT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, blk));

    /* Next alloc must drain the queue and succeed */
    void *blk2 = nullptr;
    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk2));
    ASSERT_NE(nullptr, blk2);
    (void)mempool_free(pool, blk2);
}

TEST_F(IsrFreeTest, StrerrorIsrQueueFull) {
    const char *s = mempool_strerror(MEMPOOL_ERR_ISR_QUEUE_FULL);
    ASSERT_NE(nullptr, s);
    EXPECT_NE(std::string("Unknown error"), std::string(s));
}

/* -----------------------------------------------------------------------
 * Block tag tests
 * -------------------------------------------------------------------- */

class TagTest : public ::testing::Test {
protected:
    static constexpr size_t BLOCK = 48U;
    static constexpr size_t NBLK  = 8U;

    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE]{};
    std::vector<uint8_t> pool_buf_storage;
    mempool_t *pool = nullptr;

    void SetUp() override {
        pool_buf_storage.assign(pool_buf_for(BLOCK, NBLK) + 8U, 0U);
        auto *raw = pool_buf_storage.data();
        size_t space = pool_buf_storage.size();
        std::align(8U, pool_buf_storage.size() - 8U,
                   reinterpret_cast<void*&>(raw), space);
        pool = make_pool(state_buf, sizeof state_buf,
                         raw, pool_buf_storage.size() - 8U, BLOCK);
        ASSERT_NE(nullptr, pool);
    }
};

TEST_F(TagTest, SetAndGetTag) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    EXPECT_EQ(MEMPOOL_OK, mempool_set_block_tag(pool, blk, 0xCAFEBABEU));

    uint32_t tag = 0U;
    EXPECT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, blk, &tag));
    EXPECT_EQ(0xCAFEBABEU, tag);

    (void)mempool_free(pool, blk);
}

TEST_F(TagTest, TagInitialisedToZero) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    uint32_t tag = 0xFFFFFFFFU;
    EXPECT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, blk, &tag));
    EXPECT_EQ(0U, tag);

    (void)mempool_free(pool, blk);
}

TEST_F(TagTest, AllocTaggedSetsTagAtomically) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_tagged(pool, &blk, 0x1234U));

    uint32_t tag = 0U;
    EXPECT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, blk, &tag));
    EXPECT_EQ(0x1234U, tag);

    (void)mempool_free(pool, blk);
}

TEST_F(TagTest, MacroAllocTagged) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, MEMPOOL_ALLOC_TAGGED(pool, &blk, 99U));

    uint32_t tag = 0U;
    EXPECT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, blk, &tag));
    EXPECT_EQ(99U, tag);

    (void)mempool_free(pool, blk);
}

TEST_F(TagTest, MultipleBlocksHaveIndependentTags) {
    void *b0 = nullptr, *b1 = nullptr, *b2 = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_tagged(pool, &b0, 10U));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_tagged(pool, &b1, 20U));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_tagged(pool, &b2, 30U));

    uint32_t t0 = 0U, t1 = 0U, t2 = 0U;
    EXPECT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, b0, &t0));
    EXPECT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, b1, &t1));
    EXPECT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, b2, &t2));
    EXPECT_EQ(10U, t0);
    EXPECT_EQ(20U, t1);
    EXPECT_EQ(30U, t2);

    (void)mempool_free(pool, b0);
    (void)mempool_free(pool, b1);
    (void)mempool_free(pool, b2);
}

TEST_F(TagTest, InvalidBlockReturnError) {
    uint8_t not_in_pool[48]{};
    uint32_t tag = 0U;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK,
              mempool_get_block_tag(pool, not_in_pool, &tag));
}

/* -----------------------------------------------------------------------
 * mempool_reset_stats (sticky peak-usage) tests
 * -------------------------------------------------------------------- */

class ResetStatsTest : public ::testing::Test {
protected:
    static constexpr size_t BLOCK = 32U;

    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pool_buf[pool_buf_for(32, 8)]{};
    mempool_t *pool = nullptr;

    void SetUp() override {
        pool = make_pool(state_buf, sizeof state_buf,
                         pool_buf,  sizeof pool_buf, BLOCK);
        ASSERT_NE(nullptr, pool);
    }
};

TEST_F(ResetStatsTest, ResetStatsPreservesBlockState) {
    void *b0 = nullptr, *b1 = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b0));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b1));

    ASSERT_EQ(MEMPOOL_OK, mempool_reset_stats(pool));

    mempool_stats_t st{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &st));

    /* Live allocations must still be reported */
    EXPECT_EQ(2U, st.used_blocks);
    /* Counters are reset */
    EXPECT_EQ(0U, st.alloc_count);
    EXPECT_EQ(0U, st.free_count);
    /* peak is reset to current used_blocks */
    EXPECT_EQ(2U, st.peak_usage);

    (void)mempool_free(pool, b0);
    (void)mempool_free(pool, b1);
}

TEST_F(ResetStatsTest, StickyPeakPattern) {
    /* Alloc 3, free all, reset_stats, then alloc 1 — peak stays 1 not 3 */
    void *bs[3]{};
    for (auto &b : bs) { ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b)); }
    for (auto b : bs)  { (void)mempool_free(pool, b); }

    ASSERT_EQ(MEMPOOL_OK, mempool_reset_stats(pool));

    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));
    (void)mempool_free(pool, blk);

    mempool_stats_t st{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &st));
    EXPECT_EQ(1U, st.peak_usage);
}

TEST_F(ResetStatsTest, FullResetAlsoZerosPeak) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));
    (void)mempool_free(pool, blk);

    ASSERT_EQ(MEMPOOL_OK, mempool_reset(pool));

    mempool_stats_t st{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &st));
    EXPECT_EQ(0U, st.peak_usage);
    EXPECT_EQ(0U, st.alloc_count);
}

/* -----------------------------------------------------------------------
 * Pool manager (mempool_mgr) tests
 * -------------------------------------------------------------------- */

class MgrTest : public ::testing::Test {
protected:
    /* Three pools: 32 B, 128 B, 512 B */
    alignas(8) uint8_t s0[MEMPOOL_STATE_SIZE]{}, s1[MEMPOOL_STATE_SIZE]{}, s2[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t p0[pool_buf_for(32,  8)]{};
    alignas(8) uint8_t p1[pool_buf_for(128, 4)]{};
    alignas(8) uint8_t p2[pool_buf_for(512, 2)]{};

    mempool_t *pool0 = nullptr, *pool1 = nullptr, *pool2 = nullptr;
    mempool_mgr_t mgr{};

    void SetUp() override {
        pool0 = make_pool(s0, sizeof s0, p0, sizeof p0, 32U);
        pool1 = make_pool(s1, sizeof s1, p1, sizeof p1, 128U);
        pool2 = make_pool(s2, sizeof s2, p2, sizeof p2, 512U);
        ASSERT_NE(nullptr, pool0);
        ASSERT_NE(nullptr, pool1);
        ASSERT_NE(nullptr, pool2);

        mempool_t *pools[3] = { pool2, pool0, pool1 }; /* deliberately unsorted */
        ASSERT_EQ(MEMPOOL_OK, mempool_mgr_init(&mgr, pools, 3U));
    }
};

TEST_F(MgrTest, SmallAllocGoesToSmallPool) {
    void *blk = nullptr; mempool_t *owner = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 1U, &blk, &owner));
    EXPECT_EQ(pool0, owner);
    EXPECT_EQ(MEMPOOL_OK, mempool_mgr_free(&mgr, blk));
}

TEST_F(MgrTest, MediumAllocGoesToMediumPool) {
    void *blk = nullptr; mempool_t *owner = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 64U, &blk, &owner));
    EXPECT_EQ(pool1, owner);
    EXPECT_EQ(MEMPOOL_OK, mempool_mgr_free(&mgr, blk));
}

TEST_F(MgrTest, LargeAllocGoesToLargePool) {
    void *blk = nullptr; mempool_t *owner = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 256U, &blk, &owner));
    EXPECT_EQ(pool2, owner);
    EXPECT_EQ(MEMPOOL_OK, mempool_mgr_free(&mgr, blk));
}

TEST_F(MgrTest, TooLargeReturnsInvalidSize) {
    void *blk = nullptr;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE,
              mempool_mgr_alloc(&mgr, 1024U, &blk, nullptr));
}

TEST_F(MgrTest, FallbackToLargerPoolWhenSmallExhausted) {
    /* Exhaust the 32-byte pool */
    std::vector<void*> small_blks;
    void *b = nullptr; mempool_t *owner = nullptr;
    while (mempool_mgr_alloc(&mgr, 1U, &b, &owner) == MEMPOOL_OK &&
           owner == pool0) {
        small_blks.push_back(b); b = nullptr;
    }
    if (b) { (void)mempool_mgr_free(&mgr, b); }

    /* Next alloc for 1 byte should fall through to pool1 */
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 1U, &b, &owner));
    EXPECT_EQ(pool1, owner);
    (void)mempool_mgr_free(&mgr, b);

    for (auto sb : small_blks) { (void)mempool_mgr_free(&mgr, sb); }
}

TEST_F(MgrTest, FreeIdentifiesOwnerAutomatically) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 400U, &blk, nullptr));
    EXPECT_EQ(MEMPOOL_OK, mempool_mgr_free(&mgr, blk));
}

TEST_F(MgrTest, FreeUnknownPointerReturnsError) {
    uint8_t not_in_any_pool[64]{};
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK,
              mempool_mgr_free(&mgr, not_in_any_pool));
}

TEST_F(MgrTest, NullArgsReturnNullPtr) {
    void *blk = nullptr;
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_mgr_alloc(nullptr, 1U, &blk, nullptr));
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_mgr_alloc(&mgr, 1U, nullptr, nullptr));
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_mgr_free(nullptr, &blk));
}

TEST_F(MgrTest, MgrInitRejectsZeroCount) {
    mempool_mgr_t m{};
    mempool_t *p[1] = { pool0 };
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE, mempool_mgr_init(&m, p, 0U));
}

TEST_F(MgrTest, MgrInitRejectsTooManyPools) {
    mempool_mgr_t m{};
    mempool_t *p[1] = { pool0 };
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE,
              mempool_mgr_init(&m, p,
                               static_cast<uint32_t>(MEMPOOL_MGR_MAX_POOLS) + 1U));
}

TEST_F(MgrTest, PoolOutNullIsAccepted) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 10U, &blk, nullptr));
    EXPECT_NE(nullptr, blk);
    (void)mempool_mgr_free(&mgr, blk);
}

/* -----------------------------------------------------------------------
 * mempool_block_size / mempool_capacity (always-available query API)
 * -------------------------------------------------------------------- */

class BlockInfoTest : public ::testing::Test {
protected:
    static constexpr size_t BLOCK = 64U;
    static constexpr size_t NBLK  = 4U;

    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pool_buf[pool_buf_for(64, 4)]{};
    mempool_t *pool = nullptr;

    void SetUp() override {
        pool = make_pool(state_buf, sizeof state_buf,
                         pool_buf,  sizeof pool_buf, BLOCK);
        ASSERT_NE(nullptr, pool);
    }
};

TEST_F(BlockInfoTest, BlockSizeIsNonZero) {
    EXPECT_GT(mempool_block_size(pool), 0U);
}

TEST_F(BlockInfoTest, BlockSizeIncludesGuardOverhead) {
    /* With GUARD ON the stride = align_up(BLOCK + 4, alignment). */
    uint32_t bs = mempool_block_size(pool);
#if MEMPOOL_ENABLE_GUARD
    EXPECT_GE(bs, static_cast<uint32_t>(BLOCK + 4U));
#else
    EXPECT_GE(bs, static_cast<uint32_t>(BLOCK));
#endif
    (void)bs;
}

TEST_F(BlockInfoTest, CapacityAtLeastNblk) {
    /* pool_buf_for() over-provisions so we must fit at least NBLK blocks. */
    EXPECT_GE(mempool_capacity(pool), static_cast<uint32_t>(NBLK));
}

TEST_F(BlockInfoTest, NullPoolReturnsZero) {
    EXPECT_EQ(0U, mempool_block_size(nullptr));
    EXPECT_EQ(0U, mempool_capacity(nullptr));
}

/* -----------------------------------------------------------------------
 * ISR free + DOUBLE_FREE_CHECK bitmap interaction
 * These tests catch the bug where mp_flush_isr_queue() was not clearing
 * the bitmap bit, allowing a subsequent mempool_free() to silently
 * double-add a block to the free list and corrupt the pool.
 * -------------------------------------------------------------------- */

class IsrBitmapTest : public ::testing::Test {
protected:
    static constexpr size_t BLOCK = 32U;
    static constexpr size_t NBLK  = 8U;

    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE]{};
    std::vector<uint8_t> pool_buf_storage;
    mempool_t *pool = nullptr;

    void SetUp() override {
        pool_buf_storage.assign(pool_buf_for(BLOCK, NBLK) + 8U, 0U);
        auto *raw = pool_buf_storage.data();
        size_t space = pool_buf_storage.size();
        std::align(8U, pool_buf_storage.size() - 8U,
                   reinterpret_cast<void*&>(raw), space);
        pool = make_pool(state_buf, sizeof state_buf,
                         raw, pool_buf_storage.size() - 8U, BLOCK);
        ASSERT_NE(nullptr, pool);
    }
};

TEST_F(IsrBitmapTest, NormalFreeAfterIsrDrainDetectsDoubleFree) {
    /* Alloc a block, free it via ISR, drain, then attempt a normal free.
     * The bitmap must correctly detect the double-free after the drain. */
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    ASSERT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, blk));
    ASSERT_EQ(MEMPOOL_OK, mempool_drain_isr_queue(pool));

    /* Block is now free; a second free must be caught. */
    EXPECT_EQ(MEMPOOL_ERR_DOUBLE_FREE, mempool_free(pool, blk));
}

TEST_F(IsrBitmapTest, PoolRemainsConsistentAfterIsrDrain) {
    /* Exhaust pool, free all via ISR, drain, then re-exhaust to confirm
     * every block is reachable exactly once after the ISR path. */
    uint32_t cap = mempool_capacity(pool);

    std::vector<void*> blks(cap, nullptr);
    for (uint32_t i = 0U; i < cap; i++) {
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blks[i]));
    }
    {
        void *tmp = nullptr;
        EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, mempool_alloc(pool, &tmp));
    }

    for (uint32_t i = 0U; i < cap; i++) {
        ASSERT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, blks[i]));
        if ((i + 1U) % MEMPOOL_ISR_QUEUE_CAPACITY == 0U) {
            ASSERT_EQ(MEMPOOL_OK, mempool_drain_isr_queue(pool));
        }
    }
    ASSERT_EQ(MEMPOOL_OK, mempool_drain_isr_queue(pool));

    /* All blocks must be allocatable again — exactly cap of them. */
    uint32_t count = 0U;
    std::vector<void*> blks2;
    void *b = nullptr;
    while (mempool_alloc(pool, &b) == MEMPOOL_OK) {
        blks2.push_back(b);
        count++;
    }
    EXPECT_EQ(cap, count);
    for (auto bb : blks2) { (void)mempool_free(pool, bb); }
}

/* -----------------------------------------------------------------------
 * mempool_free_from_isr — block validation (range and alignment)
 * -------------------------------------------------------------------- */

class IsrValidateTest : public ::testing::Test {
protected:
    static constexpr size_t BLOCK = 32U;
    static constexpr size_t NBLK  = 4U;

    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE]{};
    std::vector<uint8_t> pool_buf_storage;
    mempool_t *pool = nullptr;

    void SetUp() override {
        pool_buf_storage.assign(pool_buf_for(BLOCK, NBLK) + 8U, 0U);
        auto *raw = pool_buf_storage.data();
        size_t space = pool_buf_storage.size();
        std::align(8U, pool_buf_storage.size() - 8U,
                   reinterpret_cast<void*&>(raw), space);
        pool = make_pool(state_buf, sizeof state_buf,
                         raw, pool_buf_storage.size() - 8U, BLOCK);
        ASSERT_NE(nullptr, pool);
    }
};

TEST_F(IsrValidateTest, PointerOutsidePoolRejected) {
    uint8_t not_in_pool[32]{};
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK,
              mempool_free_from_isr(pool, not_in_pool));
}

TEST_F(IsrValidateTest, MisalignedPointerRejected) {
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    /* Shift by 1 byte — misaligned within the block region. */
    void *misaligned = static_cast<uint8_t*>(blk) + 1U;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK,
              mempool_free_from_isr(pool, misaligned));

    (void)mempool_free(pool, blk);
}

TEST_F(IsrValidateTest, NullBlockRejected) {
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_free_from_isr(pool, nullptr));
}

/* -----------------------------------------------------------------------
 * ISR drain path: GUARD validation and POISON fill
 * These tests verify that blocks freed via mempool_free_from_isr() have
 * their canary checked and are poisoned on drain, matching the protection
 * provided by the normal mempool_free() path.
 * -------------------------------------------------------------------- */

class IsrGuardPoisonTest : public ::testing::Test {
protected:
    static constexpr size_t BLOCK = 32U;
    static constexpr size_t NBLK  = 4U;

    alignas(8) uint8_t state_buf[MEMPOOL_STATE_SIZE]{};
    std::vector<uint8_t> pool_buf_storage;
    mempool_t *pool = nullptr;

    void SetUp() override {
        pool_buf_storage.assign(pool_buf_for(BLOCK, NBLK) + 8U, 0U);
        auto *raw = pool_buf_storage.data();
        size_t space = pool_buf_storage.size();
        std::align(8U, pool_buf_storage.size() - 8U,
                   reinterpret_cast<void*&>(raw), space);
        pool = make_pool(state_buf, sizeof state_buf,
                         raw, pool_buf_storage.size() - 8U, BLOCK);
        ASSERT_NE(nullptr, pool);
    }
};

TEST_F(IsrGuardPoisonTest, GuardViolationDetectedOnIsrDrain) {
    /* Alloc a block, corrupt its canary, queue it via ISR, drain.
     * The block must NOT be returned to the free list and the
     * guard_violations counter must be incremented. */
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    /* Corrupt the post-canary (byte just past the user area) */
    uint32_t bad = 0xDEAD1234U;
    std::memcpy(static_cast<uint8_t*>(blk) + BLOCK, &bad, sizeof bad);

    ASSERT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, blk));
    ASSERT_EQ(MEMPOOL_OK, mempool_drain_isr_queue(pool));

    mempool_stats_t st{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &st));
    EXPECT_EQ(1U, st.guard_violations);

    /* The corrupted block must not be in the free list — pool is one block
     * short.  Allocating capacity blocks must fail with OOM (not succeed). */
    uint32_t cap = mempool_capacity(pool);
    std::vector<void*> blks;
    void *b = nullptr;
    while (mempool_alloc(pool, &b) == MEMPOOL_OK) { blks.push_back(b); }
    /* One block is permanently withdrawn — can only get (cap - 1) blocks. */
    EXPECT_EQ(cap - 1U, static_cast<uint32_t>(blks.size()));
    for (auto bb : blks) { (void)mempool_free(pool, bb); }
}

TEST_F(IsrGuardPoisonTest, PoisonFillAppliedOnIsrDrain) {
    /* Alloc a block, fill it with 0xAB, queue via ISR, drain.
     * After drain the user bytes (past the free-list pointer) must be 0xDD. */
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));
    std::memset(blk, 0xAB, BLOCK);

    ASSERT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, blk));
    ASSERT_EQ(MEMPOOL_OK, mempool_drain_isr_queue(pool));

    auto *bytes = static_cast<uint8_t*>(blk);
    /* The first sizeof(void*) bytes hold the intrusive free-list pointer. */
    for (size_t i = sizeof(void*); i < BLOCK; i++) {
        EXPECT_EQ(MEMPOOL_FREE_POISON_BYTE, bytes[i])
            << "ISR drain poison mismatch at byte " << i;
    }
}

/* -----------------------------------------------------------------------
 * mempool_is_initialized() — new always-available validity check
 * -------------------------------------------------------------------- */

TEST(IsInitializedTest, ValidPoolReturnsOne) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 4)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));
    EXPECT_EQ(1, mempool_is_initialized(pool));
}

TEST(IsInitializedTest, NullPoolReturnsZero) {
    EXPECT_EQ(0, mempool_is_initialized(nullptr));
}

TEST(IsInitializedTest, UninitializedBufferReturnsZero) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};  /* zeroed, not init'd */
    EXPECT_EQ(0, mempool_is_initialized(reinterpret_cast<mempool_t*>(state)));
}

/* -----------------------------------------------------------------------
 * mempool_mgr_init() — rejects uninitialized pool handles
 * -------------------------------------------------------------------- */

TEST(MgrInitValidationTest, RejectsUninitializedPool) {
    alignas(8) uint8_t s0[MEMPOOL_STATE_SIZE]{}, s1[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t p0[pool_buf_for(32, 4)]{};
    mempool_t *pool0 = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(s0, sizeof s0, p0, sizeof p0,
                                       32U, 8U, &pool0));

    /* s1 is zeroed (not initialised) — reinterpret as a pool handle */
    mempool_t *uninit = reinterpret_cast<mempool_t*>(s1);

    mempool_mgr_t mgr{};
    mempool_t *pools[2] = { pool0, uninit };
    EXPECT_EQ(MEMPOOL_ERR_NOT_INITIALIZED,
              mempool_mgr_init(&mgr, pools, 2U));
}

/* -----------------------------------------------------------------------
 * mempool_alloc_zero() — *block is NULL on error
 * -------------------------------------------------------------------- */

TEST(AllocZeroNullOnErrorTest, BlockNulledOnOOM) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 2)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    /* Exhaust the pool */
    uint32_t cap = mempool_capacity(pool);
    std::vector<void*> blks(cap, nullptr);
    for (uint32_t i = 0U; i < cap; i++) {
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blks[i]));
    }

    void *blk = reinterpret_cast<void*>(0xDEADDEADUL); /* non-null sentinel */
    EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, mempool_alloc_zero(pool, &blk));
    EXPECT_EQ(nullptr, blk); /* must be cleared on error */

    for (auto b : blks) { (void)mempool_free(pool, b); }
}

TEST(AllocZeroNullOnErrorTest, NullPoolSetsBlockNull) {
    void *blk = reinterpret_cast<void*>(0xDEADDEADUL);
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_alloc_zero(nullptr, &blk));
    EXPECT_EQ(nullptr, blk);
}

/* ====================================================================
 * AUDIT PASS 4 — Fix verifications
 * ==================================================================== */

/* -------------------------------------------------------------------- *
 *  mempool_free() — guard check must precede bitmap clear               *
 *  A guard-corrupted block must:                                        *
 *   (a) return MEMPOOL_ERR_GUARD_CORRUPTED                             *
 *   (b) leave the bitmap bit SET (block quarantined, not "free")       *
 *       The canary remains broken → every subsequent free also returns  *
 *       GUARD_CORRUPTED (guard fires before double-free check), which  *
 *       prevents the block from ever being accidentally added to the   *
 *       free list.                                                      *
 *   (c) decrement stats.used_blocks                                    *
 *   (d) the pool's free_blocks count must NOT increase after quarantine *
 * -------------------------------------------------------------------- */
TEST(GuardBitmapConsistencyTest, GuardCorruptedLeaveBitmapSet) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 4)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    uint32_t cap = mempool_capacity(pool);
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));
    ASSERT_NE(nullptr, blk);

    mempool_stats_t before{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &before));
    uint32_t free_before = before.free_blocks;

    /* Corrupt the post-canary word — canary sits at byte offset equal to the
     * user block size (32), not the full stride. */
    static constexpr size_t USER_BLOCK = 32U;
    uint32_t *canary = reinterpret_cast<uint32_t *>(
            static_cast<uint8_t *>(blk) + USER_BLOCK);
    *canary ^= 0xFFFFFFFFU;

    /* First free: must return GUARD_CORRUPTED */
    EXPECT_EQ(MEMPOOL_ERR_GUARD_CORRUPTED, mempool_free(pool, blk));

    /* The bitmap bit is still SET: guard fires before double-free check, so
     * a second free also returns GUARD_CORRUPTED (not DOUBLE_FREE).  This
     * is correct — the block is permanently quarantined. */
    EXPECT_EQ(MEMPOOL_ERR_GUARD_CORRUPTED, mempool_free(pool, blk));

    /* free_blocks must NOT increase — the block was never added to the free list */
    mempool_stats_t after{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &after));
    EXPECT_EQ(free_before, after.free_blocks);
    EXPECT_EQ(cap - 1U, after.free_blocks + after.used_blocks);
}

TEST(GuardBitmapConsistencyTest, GuardCorruptedDecrementsUsedBlocks) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 4)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    mempool_stats_t before{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &before));
    EXPECT_EQ(1U, before.used_blocks);

    /* Corrupt canary — at byte offset 32 (user block size) */
    static constexpr size_t USER_BLOCK2 = 32U;
    *reinterpret_cast<uint32_t *>(
            static_cast<uint8_t *>(blk) + USER_BLOCK2) ^= 0xFFFFFFFFU;

    EXPECT_EQ(MEMPOOL_ERR_GUARD_CORRUPTED, mempool_free(pool, blk));

    mempool_stats_t after{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &after));
    EXPECT_EQ(0U, after.used_blocks);          /* decremented on quarantine */
    EXPECT_EQ(1U, after.guard_violations);
}

/* -------------------------------------------------------------------- *
 *  mp_flush_isr_queue() — ISR double-free must be discarded            *
 *  Queuing the same block twice via mempool_free_from_isr() must not   *
 *  corrupt the free list; the duplicate is silently discarded on drain. *
 * -------------------------------------------------------------------- */
TEST(IsrDoubleFreeTest, DuplicateIsrFreeDiscarded) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 4)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    uint32_t cap = mempool_capacity(pool);

    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    /* Queue the same block twice from ISR context */
    EXPECT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, blk));
    EXPECT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, blk));

    /* Drain — should silently drop the duplicate */
    mempool_drain_isr_queue(pool);

    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &s));
    EXPECT_EQ(0U,   s.used_blocks); /* block was logically freed exactly once */
    EXPECT_EQ(cap, s.free_blocks); /* all cap blocks are now free — NOT cap+1 */

    /* The block must be normally allocatable exactly once after drain */
    void *blk2 = nullptr;
    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk2));
    EXPECT_EQ(blk, blk2);          /* LIFO: same block comes back first */

    /* Pool should have (cap-1) more allocatable blocks, not (cap) */
    std::vector<void*> rest;
    mempool_error_t err = MEMPOOL_OK;
    while (err == MEMPOOL_OK) {
        void *b = nullptr;
        err = mempool_alloc(pool, &b);
        if (err == MEMPOOL_OK) { rest.push_back(b); }
    }
    /* We already hold blk2, so max additional = cap-1 */
    EXPECT_EQ(cap - 1U, static_cast<uint32_t>(rest.size()));

    mempool_free(pool, blk2);
    for (auto b : rest) { mempool_free(pool, b); }
}

/* -------------------------------------------------------------------- *
 *  mempool_alloc() — *block must be NULL on every error path           *
 * -------------------------------------------------------------------- */
TEST(AllocNullOnErrorTest, NotInitializedClearsBlock) {
    /* Pass null pool — ensures *block is cleared even without an initialized pool */
    void *blk = reinterpret_cast<void *>(0xDEADBEEFUL);
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_alloc(nullptr, &blk));
    EXPECT_EQ(nullptr, blk);
}

TEST(AllocNullOnErrorTest, OomClearsBlock) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 2)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    uint32_t cap = mempool_capacity(pool);
    std::vector<void*> blks(cap, nullptr);
    for (uint32_t i = 0U; i < cap; i++) {
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blks[i]));
    }

    void *blk = reinterpret_cast<void *>(0xDEADBEEFUL);
    EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, mempool_alloc(pool, &blk));
    EXPECT_EQ(nullptr, blk); /* must be cleared on OOM */

    for (auto b : blks) { mempool_free(pool, b); }
}

/* -------------------------------------------------------------------- *
 *  mempool_mgr_alloc() — *block must be NULL on every error path       *
 * -------------------------------------------------------------------- */
TEST(MgrAllocNullOnErrorTest, InvalidSizeClearsBlock) {
    alignas(8) uint8_t s16[MEMPOOL_STATE_SIZE]{}, s64[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t p16[pool_buf_for(16, 4)]{};
    alignas(8) uint8_t p64[pool_buf_for(64, 4)]{};
    mempool_t *pool16 = nullptr, *pool64 = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(s16, sizeof s16, p16, sizeof p16, 16U, 8U, &pool16));
    ASSERT_EQ(MEMPOOL_OK, mempool_init(s64, sizeof s64, p64, sizeof p64, 64U, 8U, &pool64));

    mempool_mgr_t mgr{};
    mempool_t *pools[] = {pool16, pool64};
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_init(&mgr, pools, 2U));

    /* Request a size larger than the biggest pool */
    void *blk = reinterpret_cast<void *>(0xCAFECAFEUL);
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE, mempool_mgr_alloc(&mgr, 128U, &blk, nullptr));
    EXPECT_EQ(nullptr, blk);
}

TEST(MgrAllocNullOnErrorTest, OomClearsBlock) {
    alignas(8) uint8_t s[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t p[pool_buf_for(32, 2)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(s, sizeof s, p, sizeof p, 32U, 8U, &pool));

    mempool_mgr_t mgr{};
    mempool_t *pools[] = {pool};
    ASSERT_EQ(MEMPOOL_OK, mempool_mgr_init(&mgr, pools, 1U));

    uint32_t cap = mempool_capacity(pool);
    std::vector<void*> blks(cap, nullptr);
    for (uint32_t i = 0U; i < cap; i++) {
        ASSERT_EQ(MEMPOOL_OK, mempool_mgr_alloc(&mgr, 32U, &blks[i], nullptr));
    }

    void *blk = reinterpret_cast<void *>(0xCAFECAFEUL);
    EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, mempool_mgr_alloc(&mgr, 32U, &blk, nullptr));
    EXPECT_EQ(nullptr, blk);

    for (auto b : blks) { mempool_free(pool, b); }
}

/* ====================================================================
 * Audit pass 5 — new API: pool_buffer_size, walk, is_block_allocated,
 *                          ISR drain guard failure used_blocks fix
 * ==================================================================== */

/* -------------------------------------------------------------------- *
 *  mempool_pool_buffer_size() — runtime sizing helper                   *
 * -------------------------------------------------------------------- */
TEST(PoolBufferSizeTest, ReturnsNonZeroForValidInputs) {
    /* With all features ON the result must be >= n * stride */
    size_t sz = mempool_pool_buffer_size(32U, 8U, 8U);
    EXPECT_GT(sz, 0U);
    EXPECT_GE(sz, 8U * 32U); /* at minimum n * user_block_size */
}

TEST(PoolBufferSizeTest, ReturnsZeroForBadInputs) {
    EXPECT_EQ(0U, mempool_pool_buffer_size(0U,  8U, 8U)); /* block_size=0 */
    EXPECT_EQ(0U, mempool_pool_buffer_size(32U, 0U, 8U)); /* n=0 */
    EXPECT_EQ(0U, mempool_pool_buffer_size(32U, 8U, 3U)); /* alignment not power-of-two */
}

TEST(PoolBufferSizeTest, BufferSufficesForActualInit) {
    /* Allocate a buffer exactly sized by the runtime helper and verify
     * that mempool_init succeeds inside it. */
    const size_t BLK = 32U;
    const uint32_t N = 4U;
    size_t sz = mempool_pool_buffer_size(BLK, N, 8U);
    ASSERT_GT(sz, 0U);

    std::vector<uint8_t> pbuf(sz + 8U, 0U); /* +8 for alignment headroom */
    void *raw = pbuf.data();
    size_t space = pbuf.size();
    std::align(8U, sz, raw, space);
    ASSERT_NE(nullptr, raw);

    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    mempool_t *pool = nullptr;
    mempool_error_t err = mempool_init(state, sizeof state, raw, sz,
                                       BLK, 8U, &pool);
    EXPECT_EQ(MEMPOOL_OK, err);
    if (err == MEMPOOL_OK) {
        EXPECT_GE(mempool_capacity(pool), 1U); /* at least one block */
    }
}

/* -------------------------------------------------------------------- *
 *  MEMPOOL_POOL_BUFFER_SIZE macro — compile-time upper-bound           *
 * -------------------------------------------------------------------- */
TEST(PoolBufferSizeMacroTest, MacroUpperBoundCoversRuntime) {
    constexpr size_t BLK  = 32U;
    constexpr size_t N    = 8U;
    constexpr size_t ALGN = 8U;
    constexpr size_t macro_sz = MEMPOOL_POOL_BUFFER_SIZE(BLK, N, ALGN);
    size_t runtime_sz = mempool_pool_buffer_size(BLK, static_cast<uint32_t>(N), ALGN);
    EXPECT_GE(macro_sz, runtime_sz);
}

/* -------------------------------------------------------------------- *
 *  mempool_walk() — iterate allocated blocks                            *
 * -------------------------------------------------------------------- */
namespace {

struct WalkCtx {
    std::vector<const void *> blocks;
    std::vector<uint32_t>     indices;
};

static void walk_collector(const mempool_t * /*pool*/, const void *blk,
                            uint32_t idx, void *ctx)
{
    auto *wctx = static_cast<WalkCtx *>(ctx);
    wctx->blocks.push_back(blk);
    wctx->indices.push_back(idx);
}

} // unnamed namespace

TEST(WalkTest, WalkFindsAllAllocatedBlocks) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 4)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    uint32_t cap = mempool_capacity(pool);
    std::vector<void*> blks(cap / 2U);
    for (auto &b : blks) {
        ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b));
    }

    WalkCtx ctx;
    EXPECT_EQ(MEMPOOL_OK, mempool_walk(pool, walk_collector, &ctx));
    EXPECT_EQ(blks.size(), ctx.blocks.size());

    /* Every collected pointer must be in the allocated set */
    for (auto *b : ctx.blocks) {
        bool found = false;
        for (auto *a : blks) { if (a == b) { found = true; break; } }
        EXPECT_TRUE(found) << "walk returned unexpected block " << b;
    }

    for (auto b : blks) { mempool_free(pool, b); }
}

TEST(WalkTest, WalkOnEmptyPoolCallsCallbackZeroTimes) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 4)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    WalkCtx ctx;
    EXPECT_EQ(MEMPOOL_OK, mempool_walk(pool, walk_collector, &ctx));
    EXPECT_EQ(0U, ctx.blocks.size());
}

TEST(WalkTest, NullArgsCovered) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 2)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_walk(pool, nullptr, nullptr));
    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR, mempool_walk(nullptr, walk_collector, nullptr));
}

/* -------------------------------------------------------------------- *
 *  mempool_is_block_allocated() — per-block allocation query           *
 * -------------------------------------------------------------------- */
TEST(IsBlockAllocatedTest, AllocatedBlockReturnsOne) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 4)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));
    EXPECT_EQ(1, mempool_is_block_allocated(pool, blk));

    mempool_free(pool, blk);
    EXPECT_EQ(0, mempool_is_block_allocated(pool, blk)); /* now free */
}

TEST(IsBlockAllocatedTest, InvalidPtrReturnsZero) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 4)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    uint8_t outside = 0U;
    EXPECT_EQ(0, mempool_is_block_allocated(nullptr, &outside));
    EXPECT_EQ(0, mempool_is_block_allocated(pool, nullptr));
    EXPECT_EQ(0, mempool_is_block_allocated(pool, &outside)); /* out-of-range */
}

/* -------------------------------------------------------------------- *
 *  ISR drain guard failure: used_blocks must be decremented            *
 *  (fix in mp_flush_isr_queue matching the mempool_free() fix)        *
 * -------------------------------------------------------------------- */
TEST(IsrGuardFailureStatsTest, UsedBlocksDecrementedOnIsrGuardFailure) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 4)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk));

    /* Corrupt the canary */
    static constexpr size_t USER_SZ = 32U;
    *reinterpret_cast<uint32_t *>(
            static_cast<uint8_t *>(blk) + USER_SZ) ^= 0xFFFFFFFFU;

    /* Queue via ISR and drain */
    ASSERT_EQ(MEMPOOL_OK, mempool_free_from_isr(pool, blk));
    mempool_drain_isr_queue(pool);

    mempool_stats_t s{};
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &s));
    /* Guard failure during drain: used_blocks decremented (block quarantined),
     * free_blocks unchanged (block not returned to pool). */
    EXPECT_EQ(0U, s.used_blocks);
    EXPECT_EQ(1U, s.guard_violations);
    /* free_blocks must NOT increase — quarantined block never reached free list */
    EXPECT_EQ(mempool_capacity(pool) - 1U, s.free_blocks);
}

/* -------------------------------------------------------------------- *
 *  Tag cleared on mempool_free                                         *
 * -------------------------------------------------------------------- */
TEST(TagClearedOnFreeTest, ReallocDoesNotInheritPreviousOwnerTag) {
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 1)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    /* Allocate and tag the block. */
    void *blk = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc_tagged(pool, &blk, 0xCAFEBABEU));

    uint32_t t = 0U;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, blk, &t));
    EXPECT_EQ(0xCAFEBABEU, t);

    /* Free the block — tag must be cleared. */
    ASSERT_EQ(MEMPOOL_OK, mempool_free(pool, blk));

    /* Reallocate — same physical block; tag must be 0. */
    void *blk2 = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &blk2));
    EXPECT_EQ(blk, blk2); /* LIFO free-list: same block */

    uint32_t t2 = 0xFFFFFFFFU;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, blk2, &t2));
    EXPECT_EQ(0U, t2);
}

TEST(TagClearedOnFreeTest, PlainAllocSeesFreshTag) {
    /* A fresh alloc (no previous tag written) must read 0. */
    alignas(8) uint8_t state[MEMPOOL_STATE_SIZE]{};
    alignas(8) uint8_t pbuf[pool_buf_for(32, 2)]{};
    mempool_t *pool = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_init(state, sizeof state,
                                       pbuf, sizeof pbuf, 32U, 8U, &pool));

    void *b0 = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b0));
    uint32_t t0 = 0xDEADU;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_block_tag(pool, b0, &t0));
    EXPECT_EQ(0U, t0);
}

} /* anonymous namespace */
