#include "ArchiveService.hpp"
#include "Core/Facades/Runtime.hpp"
#include "Red/MacDepot.hpp"
#include <thread>
#include <unistd.h>
#include "App/Environment.hpp"
#include "Red/TypeInfo/Registrar.hpp"

App::ArchiveService::ArchiveService(std::filesystem::path aGameDir, std::filesystem::path aBundleDir)
    : m_gameDir(std::move(aGameDir))
    , m_bundleDir(std::move(aBundleDir))
    , m_loaded(false)
{
    if (!m_bundleDir.empty())
    {
        RegisterDirectory(m_bundleDir);
    }
}

void App::ArchiveService::OnBootstrap()
{
    HookAfter<Raw::ResourceDepot::InitializeArchives>(&ArchiveService::OnInitializeArchives).OrThrow();
}

void App::ArchiveService::OnShutdown()
{
    Unhook<Raw::ResourceDepot::InitializeArchives>();
}

#ifdef __APPLE__
namespace
{
using MacArray = Red::Mac::Array;
using MacGroup = Red::Mac::Group;

}

void App::ArchiveService::OnInitializeArchives(Red::ResourceDepot* aDepot)
{
    m_loaded = true;
    Red::Mac::g_depot = reinterpret_cast<uint8_t*>(aDepot);

    LogInfo("Loading extra archives...");

    // Diagnostics: later, ask the depot whether it sees some mod resources (hash computed like the game does).
    {
        auto* depotPtr = aDepot;
        std::thread([depotPtr]() {
            sleep(25);
            auto fnv = [](const char* aPath) {
                uint64_t h = 0xCBF29CE484222325ull;
                for (const char* c = aPath; *c; ++c)
                {
                    unsigned char ch = static_cast<unsigned char>(*c);
                    if (ch == '/') ch = '\\';
                    if (ch >= 'A' && ch <= 'Z') ch += 32;
                    h ^= ch;
                    h *= 0x100000001B3ull;
                }
                return h;
            };
            FILE* f = fopen("/tmp/axl_depot_check.log", "w");
            if (!f) return;
            for (const char* path : {"base\\gameplay\\gui\\fullscreen\\vendor\\vendor.inkwidget", "engine\\textures\\editor\\grey.xbm"})
            {
                Red::ResourcePath rp(fnv(path));
                const bool found = Raw::ResourceDepot::CheckResource(depotPtr, rp);
                fprintf(f, "%s hash=%#llx found=%d\n", path, (unsigned long long)rp.hash, (int)found);
                fflush(f);
            }
            fclose(f);
        }).detach();
    }

    // Experiment (macOS RTTI registration): opt-in with the file <plugin dir>/rtti_experiment.
    if (std::filesystem::exists(Env::BundleDir().parent_path() / L"rtti_experiment"))
    {
        LogInfo("rtti_experiment: registering pending plugin RTTI types now...");
        Red::TypeInfoRegistrar::RunPendingNow();
        LogInfo("rtti_experiment: done. ArchiveXL class: {}",
                static_cast<void*>(Red::CRTTISystem::Get()->GetClass(Red::CName("ArchiveXL"))));
    }

    if (m_dirs.empty() && m_archives.empty())
        return;

    // Never touch the depot unless its layout looks exactly as expected.
    auto* base = reinterpret_cast<uint8_t*>(aDepot);
    auto* groups = reinterpret_cast<MacArray*>(base + 0x10);
    if (!aDepot || !groups->entries || groups->size == 0 || groups->size > 32 || groups->capacity < groups->size)
    {
        LogError("Unexpected ResourceDepot layout (groups: entries={}, capacity={}, size={}), aborting.",
                 groups->entries, groups->capacity, groups->size);
        return;
    }
    for (uint32_t i = 0; i < groups->size; ++i)
    {
        const auto scope = static_cast<MacGroup*>(groups->entries)[i].scope;
        if (scope == 0 || scope > Red::Mac::ModScope)
        {
            LogError("Unexpected ResourceDepot group #{} scope {}, aborting.", i, scope);
            return;
        }
    }
    LogInfo("ResourceDepot has {} archive groups.", groups->size);

    auto mount = [&](const std::filesystem::path& aBasePath, const Core::Vector<std::filesystem::path>& aPaths) {
        if (aPaths.empty())
            return;

        std::vector<Red::CString> paths;
        paths.reserve(aPaths.size());
        for (const auto& path : aPaths)
        {
            paths.emplace_back(path.string().c_str());
        }

        const Red::CString basePath(aBasePath.string().c_str());
        const uint32_t scope = Red::Mac::ModScope;
        auto* group = static_cast<MacGroup*>(Raw::ResourceDepot::EmplaceGroup(groups, &scope, &basePath));

        // Keep the "sorted" bit the same way the game does after appending a group.
        auto* flags = reinterpret_cast<uint32_t*>(base + 0x20);
        auto* all = static_cast<MacGroup*>(groups->entries);
        if (groups->size <= 1)
        {
            *flags &= ~1u;
        }
        else if (!(*flags & 1u) && !(all[groups->size - 2].scope > all[groups->size - 1].scope))
        {
            *flags |= 1u;
        }

        MacArray pathArray{paths.data(), static_cast<uint32_t>(paths.capacity()), static_cast<uint32_t>(paths.size())};
        MacArray loaded{nullptr, 0, 0};
        Raw::ResourceDepot::LoadArchives(aDepot, group, &pathArray, &loaded, 0, 2);

        LogInfo("Mounted {} archive(s) from \"{}\", {} resource(s) registered, group now holds {} archive(s).",
                aPaths.size(), aBasePath.string(), loaded.size, group->archives.size);
    };

    for (const auto& archiveDir : m_dirs)
    {
        std::error_code error;
        auto dirIt = std::filesystem::directory_iterator(archiveDir, error);
        if (error)
        {
            LogError("Can't load archive directory \"{}\": {}", archiveDir.string(), error.message());
            continue;
        }

        Core::Vector<std::filesystem::path> archives;
        for (const auto& entry : dirIt)
        {
            if (entry.is_regular_file() && entry.path().extension() == L".archive")
            {
                archives.push_back(entry.path());
            }
        }
        std::sort(archives.begin(), archives.end());
        mount(archiveDir, archives);
    }

    if (!m_archives.empty())
    {
        mount({}, m_archives);
    }
}

#else
void App::ArchiveService::OnInitializeArchives(Red::ResourceDepot* aDepot)
{
    Red::RegisterPendingTypes();

    m_loaded = true;

    LogInfo("Loading extra archives...");

    if (m_dirs.empty() && m_archives.empty())
        return;

    Core::Vector<std::filesystem::path> loadedArchives;
    Red::DynArray<Red::ResourcePath> loadedResources;

    for (const auto& archiveDir : m_dirs)
    {
        std::error_code error;
        auto dirIt = std::filesystem::directory_iterator(archiveDir, error);

        if (error)
        {
            LogError("Can't load archive directory \"{}\": {}",
                     std::filesystem::relative(archiveDir, m_gameDir).string(),
                     error.message());
            continue;
        }

        Red::DynArray<Red::CString> archivePaths;

        for (const auto& entry : dirIt)
        {
            if (entry.is_regular_file() && entry.path().extension() == L".archive")
            {
                archivePaths.PushBack(entry.path().string());

                if (archiveDir != m_bundleDir)
                {
                    loadedArchives.push_back(entry.path());
                }
            }
        }

        auto& group = ResolveArchiveGroup(aDepot, archiveDir.string());

        if (!archivePaths.IsEmpty())
        {
            Raw::ResourceDepot::LoadArchives(nullptr, group, archivePaths, loadedResources, false);
        }
    }

    if (!m_archives.empty())
    {
        Red::DynArray<Red::CString> archivePaths;

        for (const auto& archivePath : m_archives)
        {
            archivePaths.PushBack(archivePath.string());
            loadedArchives.push_back(archivePath);
        }

        auto& group = ResolveArchiveGroup(aDepot, "");
        Raw::ResourceDepot::LoadArchives(nullptr, group, archivePaths, loadedResources, false);
    }

    for (const auto& archivePath : loadedArchives)
    {
        LogInfo("Archive \"{}\" loaded.", std::filesystem::relative(archivePath, m_gameDir).string());
    }
}

#endif

#ifndef __APPLE__
Red::ArchiveGroup& App::ArchiveService::ResolveArchiveGroup(Red::ResourceDepot* aDepot, const Red::CString& aBasePath)
{
    auto existingGroup = std::find_if(aDepot->groups.begin(), aDepot->groups.end(),
                                     [&aBasePath](const Red::ArchiveGroup& aGroup) {
                                         return aGroup.basePath == aBasePath;
                                     });

    if (existingGroup != aDepot->groups.end())
    {
        return *existingGroup;
    }

    auto firstNonModGroup = std::find_if(aDepot->groups.begin(), aDepot->groups.end(),
                                         [](const Red::ArchiveGroup& aGroup) {
                                             return aGroup.scope != Red::ArchiveScope::Mod;
                                         });
    auto firstNonModGroupIndex = firstNonModGroup - aDepot->groups.begin();

    aDepot->groups.Emplace(firstNonModGroup);

    auto& group = aDepot->groups[firstNonModGroupIndex];
    group.basePath = aBasePath;
    group.scope = Red::ArchiveScope::Mod;

    return group;
}

#endif

bool App::ArchiveService::RegisterArchive(std::filesystem::path aPath)
{
    std::error_code error;

    if (aPath.is_relative())
    {
        aPath = m_gameDir / aPath;
    }

    if (!std::filesystem::exists(aPath, error) || !std::filesystem::is_regular_file(aPath, error))
    {
        LogError("Can't register archive \"{}\": path doesn't exist.",
                 std::filesystem::relative(aPath, m_gameDir).string());
        return false;
    }

    if (m_loaded)
    {
        LogError("Can't register archive \"{}\": depot is already initialized.",
                 std::filesystem::relative(aPath, m_gameDir).string());
        return false;
    }

    m_archives.emplace_back(std::move(aPath));
    return true;
}

bool App::ArchiveService::RegisterDirectory(std::filesystem::path aPath)
{
    std::error_code error;

    if (aPath.is_relative())
    {
        aPath = m_gameDir / aPath;
    }

    if (!std::filesystem::exists(aPath, error) || !std::filesystem::is_directory(aPath, error))
    {
        LogError("Can't register archive directory \"{}\": path doesn't exist.",
                 std::filesystem::relative(aPath, m_gameDir).string());
        return false;
    }

    if (m_loaded)
    {
        LogError("Can't register archive directory \"{}\": depot is already initialized.",
                 std::filesystem::relative(aPath, m_gameDir).string());
        return false;
    }

    m_dirs.emplace_back(std::move(aPath));
    return true;
}
