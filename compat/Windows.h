// Minimal Windows API compatibility layer for building ArchiveXL / RED4ext.SDK on macOS (clang, arm64).
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cwchar>
#include <atomic>
#include <mach-o/dyld.h>
#include <crt_externs.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cerrno>
#include <cstdio>
#include <string>

#ifndef WINAPI
#define WINAPI
#define APIENTRY
#define CALLBACK
#define __stdcall
#define __cdecl
#define __fastcall
#define __declspec(x) __attribute__((visibility("default")))
#define __forceinline inline __attribute__((always_inline))
#define _In_
#define _Out_
#define _Inout_
#define _In_opt_
#define _Out_opt_
#define TRUE 1
#define FALSE 0
#define MAX_PATH 1024
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define DLL_THREAD_ATTACH 2
#define DLL_THREAD_DETACH 3
#endif

using BOOL = int;
using BYTE = uint8_t;
using WORD = uint16_t;
using DWORD = uint32_t;
using UINT = unsigned int;
using LONG = int32_t;
using ULONG = uint32_t;
using LONGLONG = int64_t;
using ULONGLONG = uint64_t;
using DWORD64 = uint64_t;
using SIZE_T = size_t;
using LPVOID = void*;
using LPCVOID = const void*;
using PVOID = void*;
using HANDLE = void*;
using HMODULE = void*;
using HINSTANCE = void*;
using HWND = void*;
using PWSTR = wchar_t*;
using LPWSTR = wchar_t*;
using LPCWSTR = const wchar_t*;
using LPCSTR = const char*;
using LPSTR = char*;
using WCHAR = wchar_t;
using INT = int;
using LPARAM = intptr_t;
using WPARAM = uintptr_t;

inline HMODULE GetModuleHandle(const void*) { return const_cast<void*>(static_cast<const void*>(_NSGetMachExecuteHeader())); }
inline HMODULE GetModuleHandleW(const wchar_t* aName)
{
    // Named modules (e.g. L"RED4ext.dll") are resolved through the global symbol namespace on macOS.
    return aName ? reinterpret_cast<HMODULE>(static_cast<uintptr_t>(1)) : GetModuleHandle(nullptr);
}
inline void* GetProcAddress(HMODULE, const char* aName) { return dlsym(RTLD_DEFAULT, aName); }
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
inline BOOL GetModuleHandleExW(DWORD, const void* aAddr, HMODULE* aOut)
{
    Dl_info info{};
    if (!dladdr(aAddr, &info)) return FALSE;
    *aOut = info.dli_fbase;
    return TRUE;
}
inline DWORD GetModuleFileNameW(HMODULE aModule, wchar_t* aBuf, DWORD aSize)
{
    const auto count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (reinterpret_cast<const void*>(_dyld_get_image_header(i)) != aModule) continue;
        const std::string name = _dyld_get_image_name(i);
        if (name.size() + 1 > aSize) { errno = 122; return aSize; }
        for (size_t k = 0; k < name.size(); ++k) aBuf[k] = static_cast<wchar_t>(static_cast<unsigned char>(name[k]));
        aBuf[name.size()] = 0;
        return static_cast<DWORD>(name.size());
    }
    return 0;
}
inline DWORD GetLastError() { return static_cast<DWORD>(errno); }
#define ERROR_INSUFFICIENT_BUFFER 122
#define MB_OK 0
#define MB_ICONERROR 0x10
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
inline int MessageBoxW(HWND, const wchar_t* aText, const wchar_t* aCaption, UINT)
{
    fwprintf(stderr, L"[%ls] %ls\n", aCaption ? aCaption : L"", aText ? aText : L"");
    return 1;
}
inline HANDLE GetCurrentProcess() { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)); }
inline BOOL TerminateProcess(HANDLE, UINT aCode) { _exit(static_cast<int>(aCode)); return TRUE; }
inline DWORD GetCurrentThreadId() { uint64_t t = 0; pthread_threadid_np(nullptr, &t); return static_cast<DWORD>(t); }
inline DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
inline void YieldProcessor() { __asm__ __volatile__("yield"); }
inline void Sleep(DWORD ms) { usleep(ms * 1000); }
inline void SwitchToThread() { sched_yield(); }
inline void DisableThreadLibraryCalls(HMODULE) {}
inline void OutputDebugStringA(const char*) {}

// Interlocked* (Win32) on top of clang atomics, generic over the operand type.
template<typename T> inline T InterlockedIncrement(volatile T* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
template<typename T> inline T InterlockedDecrement(volatile T* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
template<typename T> inline T InterlockedIncrement64(volatile T* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
template<typename T> inline T InterlockedDecrement64(volatile T* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
template<typename T, typename V> inline T InterlockedExchangeAdd(volatile T* p, V v) { return __atomic_fetch_add(p, static_cast<T>(v), __ATOMIC_SEQ_CST); }
template<typename T, typename V> inline T InterlockedExchangeAdd64(volatile T* p, V v) { return __atomic_fetch_add(p, static_cast<T>(v), __ATOMIC_SEQ_CST); }
template<typename T, typename V> inline T InterlockedExchange(volatile T* p, V v) { return __atomic_exchange_n(p, static_cast<T>(v), __ATOMIC_SEQ_CST); }
template<typename T, typename V> inline T InterlockedExchange64(volatile T* p, V v) { return __atomic_exchange_n(p, static_cast<T>(v), __ATOMIC_SEQ_CST); }
template<typename T, typename X, typename C> inline T InterlockedCompareExchange(volatile T* p, X x, C c)
{ T e = static_cast<T>(c); __atomic_compare_exchange_n(p, &e, static_cast<T>(x), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
template<typename T, typename X, typename C> inline T InterlockedCompareExchange64(volatile T* p, X x, C c)
{ T e = static_cast<T>(c); __atomic_compare_exchange_n(p, &e, static_cast<T>(x), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }

// CRT extras
#define TEXT(x) L##x
inline void* _aligned_malloc(size_t aSize, size_t aAlign) { void* p = nullptr; if (aAlign < sizeof(void*)) aAlign = sizeof(void*); return posix_memalign(&p, aAlign, aSize) == 0 ? p : nullptr; }
inline void _aligned_free(void* p) { free(p); }

#define CP_UTF8 65001
inline int MultiByteToWideChar(UINT, DWORD, const char* aStr, int aLen, wchar_t* aOut, int aOutLen)
{
    // UTF-8 -> UTF-32 (wchar_t is 32-bit on macOS)
    if (aLen < 0) aLen = static_cast<int>(strlen(aStr));
    int n = 0;
    for (int i = 0; i < aLen;)
    {
        unsigned char c = static_cast<unsigned char>(aStr[i]);
        uint32_t cp; int extra;
        if (c < 0x80) { cp = c; extra = 0; } else if ((c >> 5) == 6) { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; extra = 2; } else { cp = c & 0x07; extra = 3; }
        for (int k = 1; k <= extra && i + k < aLen; ++k) cp = (cp << 6) | (static_cast<unsigned char>(aStr[i + k]) & 0x3F);
        i += extra + 1;
        if (aOut && n < aOutLen) aOut[n] = static_cast<wchar_t>(cp);
        ++n;
    }
    return n;
}
inline int WideCharToMultiByte(UINT, DWORD, const wchar_t* aStr, int aLen, char* aOut, int aOutLen, const char*, BOOL*)
{
    if (aLen < 0) aLen = static_cast<int>(wcslen(aStr));
    int n = 0;
    auto put = [&](unsigned char b) { if (aOut && n < aOutLen) aOut[n] = static_cast<char>(b); ++n; };
    for (int i = 0; i < aLen; ++i)
    {
        uint32_t cp = static_cast<uint32_t>(aStr[i]);
        if (cp < 0x80) put(cp);
        else if (cp < 0x800) { put(0xC0 | (cp >> 6)); put(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { put(0xE0 | (cp >> 12)); put(0x80 | ((cp >> 6) & 0x3F)); put(0x80 | (cp & 0x3F)); }
        else { put(0xF0 | (cp >> 18)); put(0x80 | ((cp >> 12) & 0x3F)); put(0x80 | ((cp >> 6) & 0x3F)); put(0x80 | (cp & 0x3F)); }
    }
    return n;
}
