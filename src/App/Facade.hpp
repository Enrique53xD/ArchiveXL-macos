#pragma once

#include "App/Project.hpp"

namespace App
{
class Facade : public Red::IScriptable
{
public:
    static bool RegisterDir(Red::CString& aPath);
    static bool RegisterArchive(Red::CString& aPath);
    static Red::CName GetBodyType(const Red::WeakHandle<Red::GameObject>& aPuppet);
    static Red::CString ResolveDynamicAppearanceString(const Red::WeakHandle<Red::GameObject>& aPuppet,
                                                       const Red::CString& aAppearance, const Red::CString& aString);
    static Red::CString ResolveDynamicAppearancePath(const Red::WeakHandle<Red::GameObject>& aPuppet,
                                                     const Red::CString& aAppearance, const Red::CString& aPath);
    static void EnableGarmentOffsets();
    static void DisableGarmentOffsets();
    static void Reload();
    static bool Require(Red::CString& aVersion);
    static Red::CString GetVersion();
    static bool Log(Red::CString& aText);
    static bool SetPendingController(const Red::Handle<Red::IScriptable>& aController);
    static bool QueueController(const Red::Handle<Red::IScriptable>& aController, Red::CString& aClassName);
    static bool DumpObject(const Red::Handle<Red::IScriptable>& aObject, Red::CString& aTag);
    static bool ProbeLogicOffset(const Red::Handle<Red::IScriptable>& aWidget, const Red::Handle<Red::IScriptable>& aController);

    RTTI_IMPL_TYPEINFO(Facade);
};
}

RTTI_DEFINE_CLASS(App::Facade, App::Project::Name, {
    RTTI_ABSTRACT();
    RTTI_METHOD(RegisterDir);
    RTTI_METHOD(RegisterArchive);
    RTTI_METHOD(GetBodyType);
    RTTI_METHOD(ResolveDynamicAppearanceString);
    RTTI_METHOD(ResolveDynamicAppearancePath);
    RTTI_METHOD(EnableGarmentOffsets);
    RTTI_METHOD(DisableGarmentOffsets);
    RTTI_METHOD(Reload);
    RTTI_METHOD(Require);
    RTTI_METHOD(GetVersion, "Version");
    RTTI_METHOD(Log);
    RTTI_METHOD(SetPendingController);
    RTTI_METHOD(QueueController);
    RTTI_METHOD(ProbeLogicOffset);
    RTTI_METHOD(DumpObject);
})
