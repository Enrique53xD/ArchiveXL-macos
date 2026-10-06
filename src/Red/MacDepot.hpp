#pragma once

#ifdef __APPLE__
namespace Red::Mac
{
// macOS layout of game containers (verified by disassembly): DynArray = {entries, capacity, size}.
struct Array
{
    void* entries;
    uint32_t capacity;
    uint32_t size;
};
static_assert(sizeof(Array) == 0x10);

// Same field offsets as the SDK ArchiveGroup, but with the Mac DynArray.
struct Group
{
    Array archives;         // 00
    uint8_t basePath[0x20]; // 10 (CString)
    uint32_t scope;         // 30
    uint32_t pad;           // 34

    const Red::CString& BasePath() const
    {
        return *reinterpret_cast<const Red::CString*>(basePath);
    }
};
static_assert(sizeof(Group) == 0x38);

constexpr uint32_t ModScope = 4;

// The depot pointer, captured when the game initializes its archives (there is no static getter on macOS).
inline uint8_t* g_depot = nullptr;

inline Array* Groups()
{
    return g_depot ? reinterpret_cast<Array*>(g_depot + 0x10) : nullptr;
}
}
#endif
