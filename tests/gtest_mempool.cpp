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

TEST(MempoolBasicTests, InitSuccess)
{
    static constexpr std::size_t BUFFER_SIZE = 4096U;
    static constexpr std::size_t BLOCK_SIZE = 64U;
    static constexpr std::size_t ALIGN = 8U;

    alignas(ALIGN) static uint8_t buffer[BUFFER_SIZE];

    mempool_t pool;
    mempool_error_t err = mempool_init(&pool, buffer, BUFFER_SIZE,
                                       BLOCK_SIZE, ALIGN);
    EXPECT_EQ(MEMPOOL_OK, err);
    EXPECT_TRUE(pool.initialized);
    EXPECT_GT(pool.total_blocks, 0U);
    EXPECT_EQ(pool.free_blocks, pool.total_blocks);
}

TEST(MempoolBasicTests, InitNullPointers)
{
    static constexpr std::size_t BUFFER_SIZE = 4096U;
    static constexpr std::size_t BLOCK_SIZE = 64U;
    static constexpr std::size_t ALIGN = 8U;

    alignas(ALIGN) static uint8_t buffer[BUFFER_SIZE];
    mempool_t pool;

    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR,
              mempool_init(nullptr, buffer, BUFFER_SIZE, BLOCK_SIZE, ALIGN));

    EXPECT_EQ(MEMPOOL_ERR_NULL_PTR,
              mempool_init(&pool, nullptr, BUFFER_SIZE, BLOCK_SIZE, ALIGN));
}

TEST(MempoolBasicTests, AlignmentAndSizeChecks)
{
    static constexpr std::size_t BUFFER_SIZE = 4096U;
    static constexpr std::size_t BLOCK_SIZE = 64U;
    static constexpr std::size_t ALIGN = 8U;

    alignas(16) static uint8_t buffer[BUFFER_SIZE];
    mempool_t pool;

    /* Non power-of-two alignment */
    EXPECT_EQ(MEMPOOL_ERR_ALIGNMENT,
              mempool_init(&pool, buffer, BUFFER_SIZE, BLOCK_SIZE, 7U));

    /* Misaligned buffer: +1 offset */
    EXPECT_EQ(MEMPOOL_ERR_ALIGNMENT,
              mempool_init(&pool, &buffer[1], BUFFER_SIZE, BLOCK_SIZE, ALIGN));

    /* Too small block size */
    EXPECT_EQ(MEMPOOL_ERR_INVALID_SIZE,
              mempool_init(&pool, buffer, BUFFER_SIZE, 4U, ALIGN));
}

TEST(MempoolBasicTests, AllocFreeAndStats)
{
    static constexpr std::size_t BUFFER_SIZE = 4096U;
    static constexpr std::size_t BLOCK_SIZE = 64U;
    static constexpr std::size_t ALIGN = 8U;

    alignas(ALIGN) static uint8_t buffer[BUFFER_SIZE];
    mempool_t pool;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(&pool, buffer, BUFFER_SIZE, BLOCK_SIZE, ALIGN));

    void *b1 = nullptr;
    void *b2 = nullptr;

    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(&pool, &b1));
    EXPECT_NE(nullptr, b1);
    EXPECT_EQ(MEMPOOL_OK, mempool_alloc(&pool, &b2));
    EXPECT_NE(nullptr, b2);
    EXPECT_NE(b1, b2);

    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(&pool, &stats));
    EXPECT_EQ(2U, stats.used_blocks);

    EXPECT_EQ(MEMPOOL_OK, mempool_free(&pool, b1));
    EXPECT_EQ(MEMPOOL_OK, mempool_free(&pool, b2));

    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(&pool, &stats));
    EXPECT_EQ(0U, stats.used_blocks);
    EXPECT_EQ(stats.total_blocks, stats.free_blocks);
}

TEST(MempoolBasicTests, Reset)
{
    static constexpr std::size_t BUFFER_SIZE = 4096U;
    static constexpr std::size_t BLOCK_SIZE = 64U;
    static constexpr std::size_t ALIGN = 8U;

    alignas(ALIGN) static uint8_t buffer[BUFFER_SIZE];
    mempool_t pool;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(&pool, buffer, BUFFER_SIZE, BLOCK_SIZE, ALIGN));

    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(&pool, &b));

    mempool_stats_t stats_before;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(&pool, &stats_before));
    EXPECT_GT(stats_before.used_blocks, 0U);

    ASSERT_EQ(MEMPOOL_OK, mempool_reset(&pool));

    mempool_stats_t stats_after;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(&pool, &stats_after));
    EXPECT_EQ(0U, stats_after.used_blocks);
    EXPECT_EQ(stats_before.total_blocks, stats_after.total_blocks);
}

TEST(MempoolBasicTests, DoubleFreeDetection)
{
    static constexpr std::size_t BUFFER_SIZE = 2048U;
    static constexpr std::size_t BLOCK_SIZE = 64U;
    static constexpr std::size_t ALIGN = 8U;

    alignas(ALIGN) static uint8_t buffer[BUFFER_SIZE];
    mempool_t pool;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(&pool, buffer, BUFFER_SIZE, BLOCK_SIZE, ALIGN));

    void *b = nullptr;
    ASSERT_EQ(MEMPOOL_OK, mempool_alloc(&pool, &b));
    ASSERT_NE(nullptr, b);

    EXPECT_EQ(MEMPOOL_OK, mempool_free(&pool, b));
    EXPECT_EQ(MEMPOOL_ERR_DOUBLE_FREE, mempool_free(&pool, b));
}

TEST(MempoolBasicTests, InvalidPointerDetection)
{
    static constexpr std::size_t BUFFER_SIZE = 2048U;
    static constexpr std::size_t BLOCK_SIZE = 64U;
    static constexpr std::size_t ALIGN = 8U;

    alignas(ALIGN) static uint8_t buffer[BUFFER_SIZE];
    mempool_t pool;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(&pool, buffer, BUFFER_SIZE, BLOCK_SIZE, ALIGN));

    uint8_t external[64];

    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK,
              mempool_free(&pool, external));

    /* Misaligned pointer inside pool region. */
    uint8_t *misaligned = (uint8_t *)pool.blocks_start;
    misaligned += 1;
    EXPECT_EQ(MEMPOOL_ERR_INVALID_BLOCK,
              mempool_free(&pool, misaligned));
}

/* Multithreaded tests using std::thread and user-supplied mutex callbacks. */

struct MutexContext {
    std::mutex m;
};

static void lock_callback(void *ctx)
{
    MutexContext *mc = static_cast<MutexContext *>(ctx);
    mc->m.lock();
}

static void unlock_callback(void *ctx)
{
    MutexContext *mc = static_cast<MutexContext *>(ctx);
    mc->m.unlock();
}

TEST(MempoolThreadSafeTests, ConcurrentAllocFree)
{
    static constexpr std::size_t BUFFER_SIZE = 16U * 1024U;
    static constexpr std::size_t BLOCK_SIZE = 64U;
    static constexpr std::size_t ALIGN = 8U;
    static constexpr int THREADS = 8;
    static constexpr int ITERS_PER_THREAD = 2000;

    alignas(ALIGN) static uint8_t buffer[BUFFER_SIZE];
    mempool_t pool;

    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(&pool, buffer, BUFFER_SIZE, BLOCK_SIZE, ALIGN));

    MutexContext ctx;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_set_sync(&pool, lock_callback, unlock_callback, &ctx));

    std::atomic<int> failures(0);

    auto worker = [&pool, &failures]() {
        for (int i = 0; i < ITERS_PER_THREAD; ++i) {
            void *block = nullptr;
            mempool_error_t err = mempool_alloc(&pool, &block);
            if (err == MEMPOOL_OK) {
                if (block == nullptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                } else {
                    err = mempool_free(&pool, block);
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
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back(worker);
    }
    for (auto &th : threads) {
        th.join();
    }

    EXPECT_EQ(0, failures.load());

    mempool_stats_t stats;
    ASSERT_EQ(MEMPOOL_OK, mempool_get_stats(&pool, &stats));
    EXPECT_EQ(0U, stats.used_blocks);
    EXPECT_EQ(stats.total_blocks, stats.free_blocks);
}

TEST(MempoolThreadSafeTests, ContainsUnderConcurrency)
{
    static constexpr std::size_t BUFFER_SIZE = 8U * 1024U;
    static constexpr std::size_t BLOCK_SIZE = 64U;
    static constexpr std::size_t ALIGN = 8U;
    static constexpr int THREADS = 4;
    static constexpr int ITERS_PER_THREAD = 1000;

    alignas(ALIGN) static uint8_t buffer[BUFFER_SIZE];
    mempool_t pool;

    ASSERT_EQ(MEMPOOL_OK,
              mempool_init(&pool, buffer, BUFFER_SIZE, BLOCK_SIZE, ALIGN));

    MutexContext ctx;
    ASSERT_EQ(MEMPOOL_OK,
              mempool_set_sync(&pool, lock_callback, unlock_callback, &ctx));

    std::atomic<int> failures(0);

    auto worker = [&pool, &failures]() {
        for (int i = 0; i < ITERS_PER_THREAD; ++i) {
            void *block = nullptr;
            mempool_error_t err = mempool_alloc(&pool, &block);
            if (err == MEMPOOL_OK) {
                bool contains = mempool_contains(&pool, block);
                if (!contains) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                (void)mempool_free(&pool, block);
            } else if (err != MEMPOOL_ERR_OUT_OF_MEMORY) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back(worker);
    }
    for (auto &th : threads) {
        th.join();
    }

    EXPECT_EQ(0, failures.load());
}

} // namespace
