#include "Application.hpp"
#include "App/Archives/ArchiveService.hpp"
#include "App/Environment.hpp"
#include "App/Extensions/ExtensionService.hpp"
#include "App/Migration.hpp"
#include "App/Patches/EntitySpawnerPatch.hpp"
#include "App/Patches/WorldWidgetLimitPatch.hpp"
#include "App/Project.hpp"
#include "App/Shared/ResourcePathRegistry.hpp"
#include "Core/Foundation/RuntimeProvider.hpp"
#ifndef __APPLE__
#include "Support/MinHook/MinHookProvider.hpp"
#endif
#include "Support/RED4ext/RED4extProvider.hpp"
#include "Support/RedLib/RedLibProvider.hpp"
#include "Support/Spdlog/SpdlogProvider.hpp"

App::Application::Application(HMODULE aHandle, const RED4ext::v1::Sdk* aSdk)
{
    Register<Core::RuntimeProvider>(aHandle)
#ifdef __APPLE__
        // <game>/Cyberpunk2077.app/Contents/MacOS/Cyberpunk2077 -> <game> (Windows: <game>/bin/x64/Cyberpunk2077.exe)
        ->SetBaseImagePathDepth(3);
#else
        ->SetBaseImagePathDepth(2);
#endif

#ifndef __APPLE__
    Register<Support::MinHookProvider>();
#endif
    Register<Support::SpdlogProvider>()
        ->AppendTimestampToLogName()
        ->CreateRecentLogSymlink()
        ->SetMaxLogFiles(5);
    Register<Support::RED4extProvider>(aHandle, aSdk)
#ifdef __APPLE__
        ->EnableHooking() // no MinHook on macOS: hooks go through the RED4ext host
#endif
        ->EnableAddressLibrary()
        ->RegisterScripts(Env::ScriptsDir());
    Register<Support::RedLibProvider>();

    Register<App::ResourcePathRegistry>();
    {
        auto archives = Register<App::ArchiveService>(Env::GameDir(), Env::BundleDir());
#ifdef __APPLE__
        // The macOS build of the game does not load archive/pc/mod on its own: mount it like the Windows game does,
        // so Windows-style mod packages can be unpacked into the game folder as they are.
        archives->RegisterDirectory(Env::GameDir() / L"archive" / L"pc" / L"mod");
#endif
    }
    // macOS port, stage 2: archive mounting + .xl loading. Most extensions need game functions that are not located yet.
    Register<App::ExtensionService>(Env::BundleDir());
#ifndef __APPLE__
    Register<App::EntitySpawnerPatch>();
#endif
#ifndef __APPLE__
    Register<App::WorldWidgetLimitPatch>(); // x86-64 byte patch
#endif
}

void App::Application::OnStarting()
{
    LogInfo("{} {} is starting...", Project::Name, Project::Version.to_string());

    Migration::CleanUp(Env::LegacyBundleDir());
    Migration::CleanUp(Env::LegacyScriptsDir());
}
