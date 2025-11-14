#include <gtest/gtest.h>
#include <cstdint>
#include <cstddef>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>

extern "C" {
#include "mempool.h"
}

namespace {

constexpr std::size_t TEST_ALIGN = 8U;

/* --------------------------------------------------------------------------
 * Meta tests
 * -------------------------------------------------------------------------- */

TEST(MempoolMetaTests, StateSizeWithinBound)
{
    std::size_t needed = mempool_state_size();
    EXPECT_GT(needed, static_cast<std::size_t>(0));
    EXPECT_LE(needed, static_cast<std::size_t>(MEMPOOL_STATE_SIZE));
}

/* --------------------------------------------------------------------------
 * Initialization tests
 * -------------------------------------------------------------------------- */

TEST(MempoolBasicTests, InitSuccess)
{
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t pool_buf[4096U];

    mempool_t *pool = nullptr;
    ASSERT_LE(mempool_state_size(), sizeof(state_buf));

    mempool_error_t err = mempool_init(
        state_buf, sizeof(state_buf),
        pool_buf, sizeof(pool_buf),
        64U, TEST_ALIGN,
        &pool
    );
    EXPECT_EQ(MEMPOOL_OK, err);
    ASSERT_NE(nullptr, pool);

    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_GT(stats.total_blocks, 0U);
    EXPECT_EQ(stats.total_blocks, stats.free_blocks);
    EXPECT_EQ(0U, stats.used_blocks);
}

TEST(MempoolBasicTests, InitNullPointers)
{
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t pool_buf[4096U];
    mempool_t *pool = nullptr;

    ASSERT_LE(mempool_state_size(), sizeof(state_buf));

    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR,
              mempool_init(nullptr, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));

    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR,
              mempool_init(state_buf, sizeof(state_buf),
                           nullptr, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));

    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, nullptr));
}

TEST(MempoolBasicTests, AlignmentAndSizeChecks)
{
    alignas(16) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(16) uint8_t pool_buf[4096U];
    mempool_t *pool = nullptr;

    ASSERT_LE(mempool_state_size(), sizeof(state_buf));

    /* Non power-of-two alignment */
    EXPECT_EQ(MEMPOOL_ERR_ALIGNMENT,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, 7U, &pool));

    /* Misaligned pool buffer (offset by 1) */
    EXPECT_EQ(MEMPOOL_ERR_ALIGNMENT,
              mempool_init(state_buf, sizeof(state_buf),
                           &pool_buf[1], sizeof(pool_buf) - 1U,
                           64U, TEST_ALIGN, &pool));

    /* Too small block size */
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           1U, TEST_ALIGN, &pool));
}

TEST(MempoolBasicTests, InitStateBufferTooSmall)
{
    std::size_t needed = mempool_state_size();
    std::size_t small  = (needed > 1U) ? (needed - 1U) : 0U;

    std::vector<uint8_t> state_buf(small ? small : 1U);
    alignas(TEST_ALIGN) uint8_t pool_buf[4096U];

    mempool_t *pool = nullptr;

    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE,
              mempool_init(state_buf.data(), small,
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));
}

TEST(MempoolBasicTests, PoolTooSmallForSingleBlock)
{
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t pool_buf[32U]; /* smaller than 64U block size */

    mempool_t *pool = nullptr;
    ASSERT_LE(mempool_state_size(), sizeof(state_buf));

    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));
}

/* --------------------------------------------------------------------------
 * Allocation / free / stats tests
 * -------------------------------------------------------------------------- */

TEST(MempoolBasicTests, AllocFreeAndStats)
{
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t pool_buf[4096U];

    mempool_t *pool = nullptr;
    ASSERT_LE(mempool_state_size(), sizeof(state_buf));

    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));

    void *b1 = nullptr;
    void *b2 = nullptr;

    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b1));
    EXPECT_NE(nullptr, b1);
    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b2));
    EXPECT_NE(nullptr, b2);
    EXPECT_NE(b1, b2);

    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_EQ(2U, stats.used_blocks);

    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b1));
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b2));

    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_EQ(0U, stats.used_blocks);
    EXPECT_EQ(stats.total_blocks, stats.free_blocks);
}

TEST(MempoolBasicTests, ExhaustiveAllocAndOutOfMemory)
{
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t pool_buf[4096U];

    mempool_t *pool = nullptr;
    ASSERT_LE(mempool_state_size(), sizeof(state_buf));
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));

    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    const uint32_t total = stats.total_blocks;

    std::vector<void*> blocks;
    blocks.reserve(total + 4U);

    uint32_t allocated = 0U;
    for (;;) {
        void *b = nullptr;
        mempool_error_t err = mempool_alloc(pool, &b);
        if (err == MEMPOOL_OK) {
            ASSERT_NE(nullptr, b);
            blocks.push_back(b);
            ++allocated;
        } else {
            EXPECT_EQ(MEMPOOL_ERR_OUT_OF_MEMORY, err);
            break;
        }
    }

    EXPECT_EQ(total, allocated);

    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_EQ(total, stats.used_blocks);
    EXPECT_EQ(0U, stats.free_blocks);

    for (void *b : blocks) {
        EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b));
    }

    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_EQ(0U, stats.used_blocks);
    EXPECT_EQ(stats.total_blocks, stats.free_blocks);
}

TEST(MempoolBasicTests, PeakUsageTracking)
{
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t pool_buf[4096U];

    mempool_t *pool = nullptr;
    ASSERT_LE(mempool_state_size(), sizeof(state_buf));
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));

    void *b1 = nullptr;
    void *b2 = nullptr;
    void *b3 = nullptr;

    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b1));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b2));

    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_EQ(2U, stats.used_blocks);
    EXPECT_EQ(2U, stats.peak_usage);

    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b3));
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_EQ(3U, stats.used_blocks);
    EXPECT_EQ(3U, stats.peak_usage);

    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b1));
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b2));
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b3));

    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_EQ(0U, stats.used_blocks);
    EXPECT_EQ(3U, stats.peak_usage);
}

TEST(MempoolBasicTests, ResetResetsStatsAndFreeList)
{
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t pool_buf[4096U];

    mempool_t *pool = nullptr;
    ASSERT_LE(mempool_state_size(), sizeof(state_buf));
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));

    void *b1 = nullptr;
    void *b2 = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b1));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b2));

    mempool_stats_t stats_before;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats_before));
    EXPECT_EQ(2U, stats_before.used_blocks);
    EXPECT_GE(stats_before.alloc_count, 2U);

    ASSERT_EQ(MEMPOOL_OK, mempool_reset(pool));

    mempool_stats_t stats_after;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats_after));
    EXPECT_EQ(stats_before.total_blocks, stats_after.total_blocks);
    EXPECT_EQ(0U, stats_after.used_blocks);
    EXPECT_EQ(stats_after.total_blocks, stats_after.free_blocks);
    EXPECT_EQ(0U, stats_after.alloc_count);
    EXPECT_EQ(0U, stats_after.free_count);
    EXPECT_EQ(0U, stats_after.peak_usage);

    /* Old pointers should be treated as already-free pointers. */
    EXPECT_EQ(MEMPOOL_ERR_DOUBLE_FREE, mempool_free(pool, b1));
    EXPECT_EQ(MEMPOOL_ERR_DOUBLE_FREE, mempool_free(pool, b2));
}

/* --------------------------------------------------------------------------
 * Pointer validation & contains
 * -------------------------------------------------------------------------- */

TEST(MempoolBasicTests, DoubleFreeDetection)
{
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t pool_buf[2048U];

    mempool_t *pool = nullptr;
    ASSERT_LE(mempool_state_size(), sizeof(state_buf));

    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));

    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b));
    ASSERT_NE(nullptr, b);

    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b));
    EXPECT_EQ(MEMPOOL_ERR_DOUBLE_FREE, mempool_free(pool, b));
}

TEST(MempoolBasicTests, InvalidPointerDetection)
{
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t pool_buf[2048U];

    mempool_t *pool = nullptr;
    ASSERT_LE(mempool_state_size(), sizeof(state_buf));

    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));

    uint8_t external[64];

    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK,
              mempool_free(pool, external));

    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b));
    ASSERT_NE(nullptr, b);

    uint8_t *misaligned = static_cast<uint8_t *>(b) + 1;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK,
              mempool_free(pool, misaligned));

    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b));
}

TEST(MempoolBasicTests, ContainsChecks)
{
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t pool_buf[4096U];

    mempool_t *pool = nullptr;
    ASSERT_LE(mempool_state_size(), sizeof(state_buf));

    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));

    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool, &b));
    ASSERT_NE(nullptr, b);

    EXPECT_TRUE(mempool_contains(pool, b));

    uint8_t external[16];
    EXPECT_FALSE(mempool_contains(pool, external));
    EXPECT_FALSE(mempool_contains(pool, nullptr));

    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool, b));
}

/* --------------------------------------------------------------------------
 * Multiple pools independence
 * -------------------------------------------------------------------------- */

TEST(MultiPoolTests, IndependentPools)
{
    alignas(TEST_ALIGN) uint8_t state1[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t state2[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t buf1[2048U];
    alignas(TEST_ALIGN) uint8_t buf2[2048U];

    mempool_t *pool1 = nullptr;
    mempool_t *pool2 = nullptr;

    ASSERT_LE(mempool_state_size(), sizeof(state1));
    ASSERT_LE(mempool_state_size(), sizeof(state2));

    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state1, sizeof(state1),
                           buf1, sizeof(buf1),
                           64U, TEST_ALIGN, &pool1));

    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state2, sizeof(state2),
                           buf2, sizeof(buf2),
                           64U, TEST_ALIGN, &pool2));

    void *a1 = nullptr;
    void *a2 = nullptr;

    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool1, &a1));
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(pool2, &a2));

    EXPECT_TRUE(mempool_contains(pool1, a1));
    EXPECT_TRUE(mempool_contains(pool2, a2));

    EXPECT_FALSE(mempool_contains(pool1, a2));
    EXPECT_FALSE(mempool_contains(pool2, a1));

    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool1, a1));
    EXPECT_EQ(MEMPOOL_OK, mempool_free(pool2, a2));
}

/* --------------------------------------------------------------------------
 * strerror tests
 * -------------------------------------------------------------------------- */

TEST(MempoolStringTests, ErrorStringsNonNullAndNonEmpty)
{
    mempool_error_t errors[] = {
        MEMPOOL_OK,
        MEMPOOL_ERR_NULL_PTR,
        MEMPOOL_ERR_INVALID_SIZE,
        MEMPOOL_ERR_OUT_OF_MEMORY,
        MEMPOOL_ERR_INVALID_BLOCK,
        MEMPOOL_ERR_ALIGNMENT,
        MEMPOOL_ERR_DOUBLE_FREE,
        MEMPOOL_ERR_NOT_INITIALIZED
    };

    for (mempool_error_t e : errors) {
        const char *msg = mempool_strerror(e);
        EXPECT_NE(nullptr, msg);
        EXPECT_STRNE("", msg);
    }

    const char *unknown = mempool_strerror(static_cast<mempool_error_t>(9999));
    EXPECT_NE(nullptr, unknown);
}

/* --------------------------------------------------------------------------
 * Thread-safety tests using mempool_set_sync + std::mutex
 * -------------------------------------------------------------------------- */

struct MutexContext {
    std::mutex m;
};

static void lock_callback(void *ctx)
{
    auto *mc = static_cast<MutexContext *>(ctx);
    mc->m.lock();
}

static void unlock_callback(void *ctx)
{
    auto *mc = static_cast<MutexContext *>(ctx);
    mc->m.unlock();
}

TEST(MempoolThreadSafeTests, ConcurrentAllocFree)
{
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t pool_buf[16U * 1024U];

    mempool_t *pool = nullptr;
    ASSERT_LE(mempool_state_size(), sizeof(state_buf));

    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));

    MutexContext ctx;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_set_sync(pool, lock_callback, unlock_callback, &ctx));

    constexpr int THREADS = 8;
    constexpr int ITERS   = 2000;

    std::atomic<int> failures(0);

    auto worker = [&]() {
        for (int i = 0; i < ITERS; ++i) {
            void *block = nullptr;
            mempool_error_t err = mempool_alloc(pool, &block);
            if (err == MEMPOOL_OK) {
                if (block == nullptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                } else {
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
    threads.reserve(THREADS);
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker);
    }
    for (auto &t : threads) {
        t.join();
    }

    EXPECT_EQ(0, failures.load());

    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(pool, &stats));
    EXPECT_EQ(0U, stats.used_blocks);
    EXPECT_EQ(stats.total_blocks, stats.free_blocks);
}

TEST(MempoolThreadSafeTests, ContainsUnderConcurrency)
{
    alignas(TEST_ALIGN) uint8_t state_buf[MEMPOOL_STATE_SIZE];
    alignas(TEST_ALIGN) uint8_t pool_buf[8U * 1024U];

    mempool_t *pool = nullptr;
    ASSERT_LE(mempool_state_size(), sizeof(state_buf));

    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(state_buf, sizeof(state_buf),
                           pool_buf, sizeof(pool_buf),
                           64U, TEST_ALIGN, &pool));

    MutexContext ctx;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_set_sync(pool, lock_callback, unlock_callback, &ctx));

    constexpr int THREADS = 4;
    constexpr int ITERS   = 1000;
    std::atomic<int> failures(0);

    auto worker = [&]() {
        for (int i = 0; i < ITERS; ++i) {
            void *block = nullptr;
            mempool_error_t err = mempool_alloc(pool, &block);
            if (err == MEMPOOL_OK) {
                bool inside = mempool_contains(pool, block);
                if (!inside) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                err = mempool_free(pool, block);
                if (err != MEMPOOL_OK) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (err != MEMPOOL_ERR_OUT_OF_MEMORY) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker);
    }
    for (auto &t : threads) {
        t.join();
    }

    EXPECT_EQ(0, failures.load());
}

} // namespace
