#include "Facade.hpp"
#include "App/Archives/ArchiveService.hpp"
#include "App/Extensions/ExtensionService.hpp"
#include "App/Extensions/Garment/Extension.hpp"
#include "App/Extensions/PuppetState/Extension.hpp"
#include "Core/Facades/Container.hpp"

bool App::Facade::RegisterDir(Red::CString& aPath)
{
    return Core::Resolve<ArchiveService>()->RegisterDirectory(aPath.c_str());
}

bool App::Facade::RegisterArchive(Red::CString& aPath)
{
    return Core::Resolve<ArchiveService>()->RegisterArchive(aPath.c_str());
}

Red::CName App::Facade::GetBodyType(const Red::WeakHandle<Red::GameObject>& aPuppet)
{
    return PuppetStateExtension::GetBodyType(aPuppet);
}

Red::CString App::Facade::ResolveDynamicAppearanceString(const Red::WeakHandle<Red::GameObject>& aPuppet,
                                                         const Red::CString& aAppearance, const Red::CString& aString)
{
    auto controller = GarmentExtension::GetDynamicAppearanceController();
    auto appearance = DynamicAppearanceName(aAppearance);

    return controller->ResolveString(aPuppet.Lock(), appearance.parts, aString);
}

Red::CString App::Facade::ResolveDynamicAppearancePath(const Red::WeakHandle<Red::GameObject>& aPuppet,
                                                         const Red::CString& aAppearance, const Red::CString& aPath)
{
    auto controller = GarmentExtension::GetDynamicAppearanceController();
    auto pathRegistry = Core::Resolve<ResourcePathRegistry>();

    auto appearance = DynamicAppearanceName(aAppearance);
    auto path = pathRegistry->RegisterPath(aPath.c_str());

    path = controller->ResolvePath(aPuppet.Lock(), appearance.parts, path);

    return pathRegistry->ResolvePath(path);
}

void App::Facade::EnableGarmentOffsets()
{
    GarmentExtension::EnableGarmentOffsets();
}

void App::Facade::DisableGarmentOffsets()
{
    GarmentExtension::DisableGarmentOffsets();
}

void App::Facade::Reload()
{
    Core::Resolve<ExtensionService>()->Configure();
}

bool App::Facade::Require(Red::CString& aVersion)
{
    const auto requirement = semver::from_string_noexcept(aVersion.c_str());
    return requirement.has_value() && Project::Version >= requirement.value();
}

Red::CString App::Facade::GetVersion()
{
    return Project::Version.to_string().c_str();
}

#ifdef __APPLE__
namespace App { void MacLogLine(const char* aPrefix, const char* aText); void MacSetPendingController(uint64_t aPtr, uint64_t aCtrl); void MacQueueController(uint64_t aPtr, uint64_t aCtrl, const char* aClass); void MacProbeLogicOffset(uint64_t aWidget, uint64_t aCtrl); void MacDumpObject(uint64_t aPtr, const char* aTag); }
#endif

bool App::Facade::Log(Red::CString& aText)
{
#ifdef __APPLE__
    MacLogLine("S", aText.c_str());
#endif
    return true;
}

bool App::Facade::SetPendingController(const Red::Handle<Red::IScriptable>& aController)
{
#ifdef __APPLE__
    MacSetPendingController(reinterpret_cast<uint64_t>(aController.instance), reinterpret_cast<uint64_t>(aController.refCount));
#endif
    return true;
}

bool App::Facade::QueueController(const Red::Handle<Red::IScriptable>& aController, Red::CString& aClassName)
{
#ifdef __APPLE__
    MacQueueController(reinterpret_cast<uint64_t>(aController.instance), reinterpret_cast<uint64_t>(aController.refCount), aClassName.c_str());
#endif
    return true;
}

bool App::Facade::ProbeLogicOffset(const Red::Handle<Red::IScriptable>& aWidget, const Red::Handle<Red::IScriptable>& aController)
{
#ifdef __APPLE__
    MacProbeLogicOffset(reinterpret_cast<uint64_t>(aWidget.instance), reinterpret_cast<uint64_t>(aController.instance));
#endif
    return true;
}

bool App::Facade::DumpObject(const Red::Handle<Red::IScriptable>& aObject, Red::CString& aTag)
{
#ifdef __APPLE__
    MacDumpObject(reinterpret_cast<uint64_t>(aObject.instance), aTag.c_str());
#endif
    return true;
}
