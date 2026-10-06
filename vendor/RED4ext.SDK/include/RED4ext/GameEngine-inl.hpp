#pragma once

#ifdef RED4EXT_STATIC_LIB
#include <RED4ext/GameEngine.hpp>
#endif

#include <RED4ext/Detail/AddressHashes.hpp>
#include <RED4ext/Relocation.hpp>

RED4EXT_INLINE RED4ext::CGameEngine* RED4ext::CGameEngine::Get()
{
#ifdef __APPLE__
    // macOS port: the engine singleton is not located yet; callers treat null as "not ready" and retry later.
    return nullptr;
#endif
    static UniversalRelocPtr<CGameEngine*> ptr(Detail::AddressHashes::CGameEngine);
    return ptr;
}
