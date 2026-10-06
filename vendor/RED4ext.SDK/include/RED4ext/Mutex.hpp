#pragma once

#include <RED4ext/Common.hpp>
#ifdef __APPLE__
#include <pthread.h>
#endif
#include <windows.h>

namespace RED4ext
{
struct Mutex
{
    Mutex();
    Mutex(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    void Lock();
    void Unlock();

    // --------------------------------------------
    // -- support for lock_guard --
    // --------------------------------------------

    void lock();
    void unlock();

private:
#ifdef __APPLE__
    // macOS: the game's Mutex is a plain pthread_mutex_t (64 bytes, signature 'MUTX') - verified against the live RTTISystem.
    pthread_mutex_t m_cs;
#else
    CRITICAL_SECTION m_cs;
#endif
};
RED4EXT_ASSERT_SIZE(Mutex, 40);
} // namespace RED4ext

#ifdef RED4EXT_HEADER_ONLY
#include <RED4ext/Mutex-inl.hpp>
#endif
