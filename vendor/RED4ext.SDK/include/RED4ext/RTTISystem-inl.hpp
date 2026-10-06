#pragma once

#ifdef RED4EXT_STATIC_LIB
#include <RED4ext/RTTISystem.hpp>
#endif

#include <RED4ext/Detail/AddressHashes.hpp>
#include <RED4ext/Relocation.hpp>

#include <atomic>

RED4EXT_INLINE RED4ext::CRTTISystem* RED4ext::CRTTISystem::Get()
{
    static UniversalRelocFunc<CRTTISystem* (*)()> func(Detail::AddressHashes::CRTTISystem_Get);
    return func();
}

RED4EXT_INLINE void RED4ext::CRTTISystem::RegisterType(rtti::IType* aType)
{
    RegisterType(aType, RTTIRegistrator::GetNextId());
}

RED4EXT_INLINE const uint32_t RED4ext::RTTIRegistrator::GetNextId()
{
#ifdef __APPLE__
    // macOS port: the game's counter is not located. Use a private id range far from the game's own ids.
    static std::atomic<uint32_t> s_next{0x40000000u};
    return ++s_next;
#else
    static UniversalRelocPtr<volatile uint32_t> ptr(Detail::AddressHashes::CRTTIRegistrator_RTTIAsyncId);
    return InterlockedIncrement(ptr.GetAddr());
#endif
}
