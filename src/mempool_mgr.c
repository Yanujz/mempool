#include "mempool_mgr.h"
#include <stddef.h>

/* Simple insertion-sort by ascending block_size (pool count is tiny). */
static void mgr_sort(mempool_t **arr, uint32_t n)
{
    uint32_t i;
    for (i = 1U; i < n; i++) {
        mempool_t *key    = arr[i];
        uint32_t   key_bs = mempool_block_size(key);
        uint32_t   j      = i;
        while (j > 0U) {
            if (mempool_block_size(arr[j - 1U]) <= key_bs) { break; }
            arr[j] = arr[j - 1U];
            j--;
        }
        arr[j] = key;
    }
}

mempool_error_t mempool_mgr_init(mempool_mgr_t *mgr,
                                 mempool_t    **pools,
                                 uint32_t       count)
{
    uint32_t i;

    if ((mgr == NULL) || (pools == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }
    if ((count == 0U) || (count > (uint32_t)MEMPOOL_MGR_MAX_POOLS)) {
        return MEMPOOL_ERR_INVALID_SIZE;
    }
    for (i = 0U; i < count; i++) {
        if (pools[i] == NULL) { return MEMPOOL_ERR_NULL_PTR; }
        mgr->pools[i] = pools[i];
    }
    mgr->count = count;

    mgr_sort(mgr->pools, count);
    return MEMPOOL_OK;
}

mempool_error_t mempool_mgr_alloc(mempool_mgr_t *mgr,
                                  size_t         min_size,
                                  void         **block,
                                  mempool_t    **pool_out)
{
    uint32_t i;
    int      found_candidate = 0;

    if ((mgr == NULL) || (block == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }

    for (i = 0U; i < mgr->count; i++) {
        mempool_error_t err;

        if ((size_t)mempool_block_size(mgr->pools[i]) < min_size) { continue; }

        found_candidate = 1;
        err = mempool_alloc(mgr->pools[i], block);
        if (err == MEMPOOL_OK) {
            if (pool_out != NULL) { *pool_out = mgr->pools[i]; }
            return MEMPOOL_OK;
        }
        /* Pool is full; try the next larger pool */
    }

    return found_candidate ? MEMPOOL_ERR_OUT_OF_MEMORY
                           : MEMPOOL_ERR_INVALID_SIZE;
}

mempool_error_t mempool_mgr_free(mempool_mgr_t *mgr, void *block)
{
    uint32_t i;

    if ((mgr == NULL) || (block == NULL)) {
        return MEMPOOL_ERR_NULL_PTR;
    }

    for (i = 0U; i < mgr->count; i++) {
        if (mempool_contains(mgr->pools[i], block)) {
            return mempool_free(mgr->pools[i], block);
        }
    }

    return MEMPOOL_ERR_INVALID_BLOCK;
}
