#pragma once

#ifdef RED4EXT_STATIC_LIB
#include <RED4ext/CNamePool.hpp>
#endif

#include <RED4ext/Common.hpp>
#include <RED4ext/Detail/AddressHashes.hpp>
#include <RED4ext/Relocation.hpp>

#ifdef __APPLE__
// macOS (AAPCS64): a CName is returned in x0, there is no hidden out-pointer as in the Windows x64 build.
RED4EXT_INLINE RED4ext::CName RED4ext::CNamePool::Add(const char* aText)
{
    static UniversalRelocFunc<uint64_t (*)(const char*)> func(Detail::AddressHashes::CNamePool_AddCstr);
    return CName(func(aText));
}

RED4EXT_INLINE RED4ext::CName RED4ext::CNamePool::Add(const CString& aText)
{
    return Add(aText.c_str());
}

RED4EXT_INLINE void RED4ext::CNamePool::Add(const CName& aName, const char* aText)
{
    static UniversalRelocFunc<void (*)(uint64_t, const char*)> func(Detail::AddressHashes::CNamePool_AddPair);
    func(aName.hash, aText);
}

RED4EXT_INLINE void RED4ext::CNamePool::Add(const CName& aName, const CString& aText)
{
    Add(aName, aText.c_str());
}

RED4EXT_INLINE const char* RED4ext::CNamePool::Get(const CName& aName)
{
    static UniversalRelocFunc<const char* (*)(uint64_t)> func(Detail::AddressHashes::CNamePool_Get);
    auto result = func(aName.hash);
    return result ? result : "";
}
#else
RED4EXT_INLINE RED4ext::CName RED4ext::CNamePool::Add(const char* aText)
{
    CName result;

    static UniversalRelocFunc<CName* (*)(CName&, const char*)> func(Detail::AddressHashes::CNamePool_AddCstr);
    func(result, aText);
    return result;
}

RED4EXT_INLINE RED4ext::CName RED4ext::CNamePool::Add(const CString& aText)
{
    CName result;

    static UniversalRelocFunc<CName* (*)(CName&, const CString&)> func(Detail::AddressHashes::CNamePool_AddCString);
    func(result, aText);
    return result;
}

RED4EXT_INLINE void RED4ext::CNamePool::Add(const CName& aName, const char* aText)
{
    static UniversalRelocFunc<uint8_t (*)(const CName&, const char*)> func(Detail::AddressHashes::CNamePool_AddPair);
    func(aName, aText);
}

RED4EXT_INLINE void RED4ext::CNamePool::Add(const CName& aName, const CString& aText)
{
    Add(aName, aText.c_str());
}

RED4EXT_INLINE const char* RED4ext::CNamePool::Get(const CName& aName)
{
    static UniversalRelocFunc<const char* (*)(const CName&)> func(Detail::AddressHashes::CNamePool_Get);
    auto result = func(aName);
    if (result)
    {
        return result;
    }

    return "";
}
#endif
