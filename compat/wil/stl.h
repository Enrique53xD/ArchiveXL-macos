#pragma once
#include <string>
#include <Windows.h>
namespace wil
{
// Replacement for wil::GetModuleFileNameW (std::wstring overload).
inline int GetModuleFileNameW(HMODULE aModule, std::wstring& aOut)
{
    wchar_t buffer[4096];
    const auto len = ::GetModuleFileNameW(aModule, buffer, 4096);
    aOut.assign(buffer, len);
    return 0;
}
} // namespace wil
