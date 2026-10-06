#pragma once

namespace App::Migration
{
inline void CleanUp(const std::filesystem::path& aPath)
{
#ifdef __APPLE__
    // macOS port: DISABLED. This runs remove_all() on a path derived from the module directory. With a wrong module
    // directory (and a case-insensitive filesystem, where "Archive" == "archive") it deleted the whole game data folder.
    (void)aPath;
    return;
#endif
    std::error_code error;
    if (std::filesystem::exists(aPath, error))
    {
        std::filesystem::remove_all(aPath, error);
    }
}
}
