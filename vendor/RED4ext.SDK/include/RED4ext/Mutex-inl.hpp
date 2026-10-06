#pragma once

#ifdef RED4EXT_STATIC_LIB
#include <RED4ext/Mutex.hpp>
#endif

RED4EXT_INLINE RED4ext::Mutex::Mutex()
{
    #ifdef __APPLE__
    pthread_mutex_init(&m_cs, nullptr);
#else
    InitializeCriticalSection(&m_cs);
#endif
}

RED4EXT_INLINE void RED4ext::Mutex::Lock()
{
    #ifdef __APPLE__
    pthread_mutex_lock(&m_cs);
#else
    EnterCriticalSection(&m_cs);
#endif
}

RED4EXT_INLINE void RED4ext::Mutex::Unlock()
{
    #ifdef __APPLE__
    pthread_mutex_unlock(&m_cs);
#else
    LeaveCriticalSection(&m_cs);
#endif
}

RED4EXT_INLINE void RED4ext::Mutex::lock()
{
    Lock();
}

RED4EXT_INLINE void RED4ext::Mutex::unlock()
{
    Unlock();
}
