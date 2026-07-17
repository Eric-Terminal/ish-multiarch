#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "guest/memory/file-pager.h"

#define GUEST_FILE_PAGER_BUCKET_COUNT 64

_Static_assert((GUEST_FILE_PAGER_BUCKET_COUNT &
        (GUEST_FILE_PAGER_BUCKET_COUNT - 1)) == 0,
        "文件页缓存桶数必须为二次幂");

enum cached_file_page_state {
    CACHED_FILE_PAGE_LOADING,
    CACHED_FILE_PAGE_READY,
};

struct cached_file_page {
    qword_t file_offset;
    enum cached_file_page_state state;
    struct guest_page_backing *backing;
    struct cached_file_page *next;
};

struct guest_file_pager {
    atomic_uint references;
    struct guest_file_source *file_source;
    struct guest_file_pager_provider provider;
    pthread_mutex_t cache_lock;
    pthread_cond_t cache_changed;
    struct cached_file_page *buckets[GUEST_FILE_PAGER_BUCKET_COUNT];
};

#if defined(GUEST_FILE_PAGER_TESTING)
static size_t allocation_fail_at = SIZE_MAX;
static size_t allocation_count;

void guest_file_pager_test_fail_allocation_at(size_t index) {
    allocation_fail_at = index;
    allocation_count = 0;
}

size_t guest_file_pager_test_allocation_count(void) {
    return allocation_count;
}

unsigned guest_file_pager_test_reference_count(
        const struct guest_file_pager *pager) {
    assert(pager != NULL);
    return atomic_load_explicit(&pager->references, memory_order_relaxed);
}

static void *pager_calloc(size_t count, size_t size) {
    if (allocation_count++ == allocation_fail_at)
        return NULL;
    return calloc(count, size);
}
#else
#define pager_calloc calloc
#endif

static void check_pthread(int result) {
    if (result != 0)
        abort();
}

static unsigned cache_bucket(qword_t file_offset) {
    qword_t page = file_offset >> GUEST_MEMORY_PAGE_BITS;
    page ^= page >> 17;
    page *= UINT64_C(0x9e3779b97f4a7c15);
    page ^= page >> 32;
    return (unsigned) (page & (GUEST_FILE_PAGER_BUCKET_COUNT - 1));
}

static struct cached_file_page *find_cached_page(
        struct guest_file_pager *pager, qword_t file_offset) {
    struct cached_file_page *entry =
            pager->buckets[cache_bucket(file_offset)];
    while (entry != NULL && entry->file_offset != file_offset)
        entry = entry->next;
    return entry;
}

static void remove_cached_page(struct guest_file_pager *pager,
        struct cached_file_page *target) {
    unsigned bucket = cache_bucket(target->file_offset);
    struct cached_file_page **link = &pager->buckets[bucket];
    while (*link != target) {
        assert(*link != NULL);
        link = &(*link)->next;
    }
    *link = target->next;
    target->next = NULL;
}

struct guest_file_pager *guest_file_pager_create(qword_t identity,
        struct guest_file_pager_provider provider) {
    assert(identity != 0);
    assert(provider.read_page != NULL);
    assert((provider.opaque == NULL) == (provider.release == NULL));

    struct guest_file_pager *pager = pager_calloc(1, sizeof(*pager));
    if (pager == NULL)
        return NULL;
    if (pthread_mutex_init(&pager->cache_lock, NULL) != 0) {
        free(pager);
        return NULL;
    }
    if (pthread_cond_init(&pager->cache_changed, NULL) != 0) {
        check_pthread(pthread_mutex_destroy(&pager->cache_lock));
        free(pager);
        return NULL;
    }
    pager->file_source = guest_file_source_create(identity, NULL, NULL);
    if (pager->file_source == NULL) {
        check_pthread(pthread_cond_destroy(&pager->cache_changed));
        check_pthread(pthread_mutex_destroy(&pager->cache_lock));
        free(pager);
        return NULL;
    }
    atomic_init(&pager->references, 1);
    pager->provider = provider;
    return pager;
}

bool guest_file_pager_try_retain(struct guest_file_pager *pager) {
    if (pager == NULL)
        return false;
    unsigned references = atomic_load_explicit(
            &pager->references, memory_order_acquire);
    while (references != 0) {
        assert(references != UINT_MAX);
        if (atomic_compare_exchange_weak_explicit(
                &pager->references, &references, references + 1,
                memory_order_acquire, memory_order_relaxed))
            return true;
    }
    return false;
}

struct guest_file_pager *guest_file_pager_retain(
        struct guest_file_pager *pager) {
    if (pager == NULL)
        return NULL;
    bool retained = guest_file_pager_try_retain(pager);
    assert(retained);
    return pager;
}

static void destroy_cache(struct guest_file_pager *pager) {
    for (unsigned bucket = 0;
            bucket < GUEST_FILE_PAGER_BUCKET_COUNT; bucket++) {
        struct cached_file_page *entry = pager->buckets[bucket];
        while (entry != NULL) {
            struct cached_file_page *next = entry->next;
            assert(entry->state == CACHED_FILE_PAGE_READY);
            assert(entry->backing != NULL);
            guest_page_backing_release(entry->backing);
            free(entry);
            entry = next;
        }
    }
    check_pthread(pthread_cond_destroy(&pager->cache_changed));
    check_pthread(pthread_mutex_destroy(&pager->cache_lock));
}

void guest_file_pager_release(struct guest_file_pager *pager) {
    if (pager == NULL)
        return;
    unsigned previous = atomic_fetch_sub_explicit(
            &pager->references, 1, memory_order_acq_rel);
    assert(previous != 0);
    if (previous != 1)
        return;

    /* 对象在同步回调返回前保持完整，供 provider 条件摘除弱引用。 */
    if (pager->provider.release != NULL)
        pager->provider.release(pager, pager->provider.opaque);
    destroy_cache(pager);
    guest_file_source_release(pager->file_source);
    free(pager);
}

enum guest_file_page_result guest_file_pager_get_page(
        struct guest_file_pager *pager, qword_t file_offset,
        struct guest_page_backing **backing) {
    assert(pager != NULL && backing != NULL);
    assert((file_offset & GUEST_MEMORY_PAGE_MASK) == 0);
    *backing = NULL;

    check_pthread(pthread_mutex_lock(&pager->cache_lock));
    struct cached_file_page *entry;
    for (;;) {
        entry = find_cached_page(pager, file_offset);
        if (entry == NULL)
            break;
        if (entry->state == CACHED_FILE_PAGE_READY) {
            assert(entry->backing != NULL);
            guest_page_backing_retain(entry->backing);
            *backing = entry->backing;
            check_pthread(pthread_mutex_unlock(&pager->cache_lock));
            return GUEST_FILE_PAGE_OK;
        }
        assert(entry->state == CACHED_FILE_PAGE_LOADING &&
                entry->backing == NULL);
        check_pthread(pthread_cond_wait(
                &pager->cache_changed, &pager->cache_lock));
    }

    entry = pager_calloc(1, sizeof(*entry));
    if (entry == NULL) {
        check_pthread(pthread_mutex_unlock(&pager->cache_lock));
        return GUEST_FILE_PAGE_OUT_OF_MEMORY;
    }
    *entry = (struct cached_file_page) {
        .file_offset = file_offset,
        .state = CACHED_FILE_PAGE_LOADING,
    };
    unsigned bucket = cache_bucket(file_offset);
    entry->next = pager->buckets[bucket];
    pager->buckets[bucket] = entry;
    check_pthread(pthread_mutex_unlock(&pager->cache_lock));

    struct guest_page_backing *loaded = guest_page_backing_create();
    enum guest_file_page_result result = GUEST_FILE_PAGE_OUT_OF_MEMORY;
    if (loaded != NULL) {
        byte_t *bytes = guest_page_backing_bytes(loaded);
        memset(bytes, 0, GUEST_MEMORY_PAGE_SIZE);
        dword_t valid_bytes = 0;
        result = pager->provider.read_page(
                pager->provider.opaque, file_offset,
                bytes, &valid_bytes);
        if (result == GUEST_FILE_PAGE_OK &&
                (valid_bytes == 0 ||
                valid_bytes > GUEST_MEMORY_PAGE_SIZE)) {
            result = GUEST_FILE_PAGE_IO_ERROR;
        } else if (result == GUEST_FILE_PAGE_OK &&
                valid_bytes < GUEST_MEMORY_PAGE_SIZE) {
            memset(bytes + valid_bytes, 0,
                    GUEST_MEMORY_PAGE_SIZE - valid_bytes);
        }
    }

    check_pthread(pthread_mutex_lock(&pager->cache_lock));
    assert(find_cached_page(pager, file_offset) == entry);
    if (result == GUEST_FILE_PAGE_OK) {
        assert(loaded != NULL);
        entry->state = CACHED_FILE_PAGE_READY;
        entry->backing = loaded;
        guest_page_backing_retain(loaded);
        *backing = loaded;
    } else {
        remove_cached_page(pager, entry);
    }
    check_pthread(pthread_cond_broadcast(&pager->cache_changed));
    check_pthread(pthread_mutex_unlock(&pager->cache_lock));

    if (result != GUEST_FILE_PAGE_OK) {
        if (loaded != NULL)
            guest_page_backing_release(loaded);
        free(entry);
    }
    return result;
}

struct guest_file_source *guest_file_pager_file_source(
        struct guest_file_pager *pager) {
    assert(pager != NULL && pager->file_source != NULL);
    return pager->file_source;
}
