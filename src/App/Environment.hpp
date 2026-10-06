#pragma once

#include "Core/Facades/Runtime.hpp"

namespace App::Env
{
namespace Detail
{
// Every directory ArchiveXL touches must be absolute; a relative one would silently resolve against the cwd.
inline std::filesystem::path Require(std::filesystem::path aPath)
{
    if (aPath.empty() || !aPath.is_absolute())
    {
        throw std::runtime_error("ArchiveXL: refusing to use a non-absolute path: '" + aPath.string() + "'");
    }
    return aPath;
}
}

inline std::filesystem::path GameDir()
{
    return Detail::Require(Core::Runtime::GetRootDir());
}

inline std::filesystem::path BundleDir()
{
    return Detail::Require(Core::Runtime::GetModuleDir()) / L"Bundle";
}

inline std::filesystem::path ScriptsDir()
{
    return Detail::Require(Core::Runtime::GetModuleDir()) / L"Scripts";
}

inline std::filesystem::path LegacyBundleDir()
{
    return Detail::Require(Core::Runtime::GetModuleDir()) / L"Archive";
}

inline std::filesystem::path LegacyScriptsDir()
{
    return Detail::Require(Core::Runtime::GetRootDir()) / L"r6" / L"scripts" / L"ArchiveXL";
}
}
