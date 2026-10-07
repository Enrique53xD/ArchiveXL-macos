public abstract native class ArchiveXL {
    public static native func GetBodyType(puppet: wref<GameObject>) -> CName
    public static native func EnableGarmentOffsets()
    public static native func DisableGarmentOffsets()
    public static native func Require(version: String) -> Bool
    public static native func Version() -> String

    // macOS port helpers (implemented in App/MacCompat.cpp)
    public static native func Log(text: String) -> Bool
    public static native func SetPendingController(controller: ref<IScriptable>) -> Bool
    public static native func QueueController(controller: ref<IScriptable>, className: String) -> Bool
    public static native func DumpObject(object: ref<IScriptable>, tag: String) -> Bool
    public static native func ProbeLogicOffset(widget: ref<IScriptable>, controller: ref<IScriptable>) -> Bool
}
