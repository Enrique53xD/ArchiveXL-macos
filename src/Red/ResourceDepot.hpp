#pragma once

namespace Raw::ResourceDepot
{
constexpr auto InitializeArchives = Core::RawFunc<
    /* addr = */ Red::AddressLib::ResourceDepot_InitializeArchives,
    /* type = */ void (*)(Red::ResourceDepot* aDepot)>{};

#ifdef __APPLE__
// macOS ABI (verified by disassembly of 0x10407f174): x0 is unused, the DynArrays use the Mac layout
// {entries, capacity, size} instead of the SDK's {entries, size, capacity}, and there is a 6th argument (always 2).
constexpr auto LoadArchives = Core::RawFunc<
    /* addr = */ Red::AddressLib::ResourceDepot_LoadArchives,
    /* type = */ void (*)(void* aDepot,
                          void* aGroup,
                          const void* aArchivePaths,
                          void* aLoadedResourcePaths,
                          uint32_t aMemoryResident,
                          uint32_t aMode)>{};

// Appends a group {scope, basePath} to depot->groups (DynArray of 0x38-byte groups) and returns it.
constexpr auto EmplaceGroup = Core::RawFunc<
    /* addr = */ Red::AddressLib::ResourceDepot_EmplaceGroup,
    /* type = */ void* (*)(void* aGroups, const uint32_t* aScope, const Red::CString* aBasePath)>{};
#else
constexpr auto LoadArchives = Core::RawFunc<
    /* addr = */ Red::AddressLib::ResourceDepot_LoadArchives,
    /* type = */ void (*)(Red::ResourceDepot* aDepot,
                          Red::ArchiveGroup& aGroup,
                          const Red::DynArray<Red::CString>& aArchivePaths,
                          Red::DynArray<Red::ResourcePath>& aLoadedResourcePaths,
                          bool aMemoryResident)>{};
#endif

constexpr auto RequestResource = Core::RawFunc<
    /* addr = */ Red::AddressLib::ResourceDepot_RequestResource,
    /* type = */ uintptr_t* (*)(Red::ResourceDepot* aDepot,
                                const uintptr_t* aOutResourceHandle,
                                Red::ResourcePath aPath,
                                const int32_t* aArchiveHandle)>{};

constexpr auto CheckResource = Core::RawFunc<
    /* addr = */ Red::AddressLib::ResourceDepot_CheckResource,
    /* type = */ bool (*)(Red::ResourceDepot* aDepot, Red::ResourcePath aPath)>{};
}
