#pragma once
// macOS port: minimal replacement for TiltedCore's StlAllocator / New / Delete (the original pulls in mimalloc).
#include <cstddef>
#include <cstdlib>
#include <new>
#include <utility>

namespace TiltedPhoques
{
template<typename T>
struct StlAllocator
{
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    constexpr StlAllocator() noexcept = default;
    template<typename U>
    constexpr StlAllocator(const StlAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t aCount)
    {
        void* p = std::malloc(aCount * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* aPtr, std::size_t) noexcept { std::free(aPtr); }

    template<typename U>
    bool operator==(const StlAllocator<U>&) const noexcept { return true; }
    template<typename U>
    bool operator!=(const StlAllocator<U>&) const noexcept { return false; }
};

template<typename T, typename... Args>
T* New(Args&&... aArgs)
{
    void* p = std::malloc(sizeof(T));
    if (!p) throw std::bad_alloc();
    return new (p) T(std::forward<Args>(aArgs)...);
}

template<typename T>
void Delete(T* aPtr) noexcept
{
    if (!aPtr) return;
    aPtr->~T();
    std::free(aPtr);
}
} // namespace TiltedPhoques
