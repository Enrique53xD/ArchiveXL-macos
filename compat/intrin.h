#pragma once
#include <cstdint>
// MSVC intrinsics used by RED4ext.SDK, mapped to clang builtins.
#define _InterlockedIncrement(p) (__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST))
#define _InterlockedDecrement(p) (__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST))
#define _InterlockedIncrement64(p) (__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST))
#define _InterlockedDecrement64(p) (__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST))
#define _InterlockedExchange(p, v) (__atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST))
#define _InterlockedExchange64(p, v) (__atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST))
#define _InterlockedExchangeAdd(p, v) (__atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST))
#define _InterlockedExchangeAdd64(p, v) (__atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST))
static inline long _InterlockedCompareExchange(volatile long* p, long x, long c) { __atomic_compare_exchange_n(p, &c, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
static inline long long _InterlockedCompareExchange64(volatile long long* p, long long x, long long c) { __atomic_compare_exchange_n(p, &c, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
static inline void _mm_pause() { __asm__ __volatile__("yield"); }
static inline void _ReadWriteBarrier() { __asm__ __volatile__("" ::: "memory"); }
static inline unsigned long long __rdtsc() { unsigned long long v; __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v)); return v; }

static inline char _InterlockedCompareExchange8(volatile char* p, char x, char c) { __atomic_compare_exchange_n(p, &c, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
#define InterlockedExchange8(p, v) (__atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST))
#define _InterlockedExchange8(p, v) (__atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST))
#define _InterlockedExchangeAdd8(p, v) (__atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST))
