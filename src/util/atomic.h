#ifndef ZRI_ATOMIC_H
#define ZRI_ATOMIC_H

/*
 * 极简原子原语：只覆盖本项目用到的“计数器自增 / 无锁读 / 一次性置位”。
 *
 * 项目编译标准是 C99，没有 <stdatomic.h>，因此对 GCC/Clang 使用 __atomic
 * 内建，对 MSVC 使用 Interlocked 内建。所有操作都是 relaxed 语义——这些计数
 * 只用于统计与幂等短路，不需要跨线程的顺序保证。
 */

#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h>

static inline void zri_atomic_add_u64(uint64_t *p, uint64_t v)
{
    _InterlockedExchangeAdd64((volatile __int64 *)p, (__int64)v);
}

static inline void zri_atomic_add_u32(uint32_t *p, uint32_t v)
{
    _InterlockedExchangeAdd((volatile long *)p, (long)v);
}

static inline void zri_atomic_sub_u64(uint64_t *p, uint64_t v)
{
    _InterlockedExchangeAdd64((volatile __int64 *)p, -(__int64)v);
}

static inline uint32_t zri_atomic_sub_u32(uint32_t *p, uint32_t v)
{
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)p, -(long)v);
}

static inline uint32_t zri_atomic_exchange_u32(uint32_t *p, uint32_t v)
{
    return (uint32_t)_InterlockedExchange((volatile long *)p, (long)v);
}

static inline uint64_t zri_atomic_load_u64(const uint64_t *p)
{
    return (uint64_t)_InterlockedCompareExchange64((volatile __int64 *)p, 0, 0);
}

static inline uint32_t zri_atomic_load_u32(const uint32_t *p)
{
    return (uint32_t)_InterlockedCompareExchange((volatile long *)p, 0, 0);
}

static inline void zri_atomic_store_u64(uint64_t *p, uint64_t v)
{
    _InterlockedExchange64((volatile __int64 *)p, (__int64)v);
}

static inline void zri_atomic_store_u32(uint32_t *p, uint32_t v)
{
    _InterlockedExchange((volatile long *)p, (long)v);
}

/* 比较并交换：成功返回 1。*expected 为期望值，失败时写回当前值。 */
static inline int zri_atomic_cas_u32(uint32_t *p, uint32_t *expected, uint32_t desired)
{
    long cur = _InterlockedCompareExchange((volatile long *)p, (long)desired, (long)*expected);
    if ((uint32_t)cur == *expected) {
        return 1;
    }
    *expected = (uint32_t)cur;
    return 0;
}

#else

static inline void zri_atomic_add_u64(uint64_t *p, uint64_t v)
{
    __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}

static inline void zri_atomic_add_u32(uint32_t *p, uint32_t v)
{
    __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}

static inline void zri_atomic_sub_u64(uint64_t *p, uint64_t v)
{
    __atomic_fetch_sub(p, v, __ATOMIC_RELAXED);
}

static inline uint32_t zri_atomic_sub_u32(uint32_t *p, uint32_t v)
{
    return __atomic_fetch_sub(p, v, __ATOMIC_RELAXED);
}

static inline uint32_t zri_atomic_exchange_u32(uint32_t *p, uint32_t v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_RELAXED);
}

static inline uint64_t zri_atomic_load_u64(const uint64_t *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static inline uint32_t zri_atomic_load_u32(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static inline void zri_atomic_store_u64(uint64_t *p, uint64_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

static inline void zri_atomic_store_u32(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

static inline int zri_atomic_cas_u32(uint32_t *p, uint32_t *expected, uint32_t desired)
{
    return __atomic_compare_exchange_n(p, expected, desired, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

#endif

/* metrics_t 全是 uint64_t 计数；下面两个宏让调用点保持简短。 */
#define ZRI_METRIC_INC(field) zri_atomic_add_u64((uint64_t *)&(field), 1u)
#define ZRI_METRIC_ADD(field, v) zri_atomic_add_u64((uint64_t *)&(field), (uint64_t)(v))
#define ZRI_METRIC_SUB(field, v) zri_atomic_sub_u64((uint64_t *)&(field), (uint64_t)(v))

#endif /* ZRI_ATOMIC_H */
