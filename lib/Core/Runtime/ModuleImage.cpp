#include "ModuleImage.hpp"

#ifdef __APPLE__
#include <dlfcn.h>
#include <stdexcept>

namespace
{
// Anchor symbol that lives inside this module, used to ask dyld which file we were loaded from.
void ModuleAnchor() {}
}
#endif

Core::ModuleImage::ModuleImage(HMODULE aHandle)
{
#ifdef __APPLE__
    // The plugin handle given by RED4ext is not a dyld image header, so it can't be used to find the file. Ask dyld
    // about an address inside this very module instead.
    (void)aHandle;
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void*>(&ModuleAnchor), &info) || !info.dli_fname)
    {
        throw std::runtime_error("ArchiveXL: unable to determine the module path");
    }
    m_path = std::filesystem::path(info.dli_fname);
    if (!m_path.is_absolute())
    {
        // A relative path here would later be resolved against the working directory (the game root). Refuse.
        throw std::runtime_error("ArchiveXL: module path is not absolute: " + m_path.string());
    }
#else
    std::wstring filePath;
    wil::GetModuleFileNameW(aHandle, filePath);

    m_path = filePath;
#endif
}

std::filesystem::path Core::ModuleImage::GetPath() const
{
    return m_path;
}

std::filesystem::path Core::ModuleImage::GetDir() const
{
    return m_path.parent_path();
}

std::string Core::ModuleImage::GetName() const
{
    return m_path.stem().string();
}

bool Core::ModuleImage::IsASI() const
{
    return m_path.extension() == L".asi";
}
