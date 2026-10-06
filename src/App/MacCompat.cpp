#ifdef __APPLE__
// macOS port: fixes for game quirks that would otherwise stop scripts from loading.
#include "Core/Hooking/HookingDriver.hpp"
#include "Core/Memory/AddressResolver.hpp"

namespace App { void MacLogLine(const char* aPrefix, const char* aText); }

namespace
{
// The script <-> native consistency checks of the game's script loader reject Codeware's Windows-generated imports on
// this build (generic ResourceRef/Uint8 fields vs rRef:*/enum natives, undeclared native base classes, structs declared
// as another kind). They only guard declarations, so run them (to keep their side effects) but ignore the verdict.
template<int N>
struct LenientValidator
{
    using Fn = bool (*)(void*, void*, void*);
    inline static Fn original = nullptr;
    static bool Detour(void* aThis, void* aType, void* aReporter)
    {
        original(aThis, aType, aReporter);
        return true;
    }
};

using PropFn = bool (*)(void*, void*, RED4ext::CProperty*);
PropFn s_propOriginal = nullptr;

// Script<->native property check: some native properties have no resolved type (null) on macOS, which crashes the
// original; never fail on properties.
bool ValidatePropertyType(void* aThis, void* aScriptProp, RED4ext::CProperty* aNativeProp)
{
    if (!aNativeProp || !aNativeProp->type)
        return true;
    s_propOriginal(aThis, aScriptProp, aNativeProp);
    return true;
}
}

// Diagnostics: log every distinct plugin-defined (non-image) native/scripted function executed through the interpreter
// entry point, and note when it reports failure. Output: /tmp/axl_calls.log
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_set>
namespace
{
using ExecFn = bool (*)(void*, void*, void*, void*, void*);
ExecFn s_execOriginal = nullptr;
int s_callsFd = -1;
uintptr_t s_imgLo = 0, s_imgHi = 0;
std::mutex s_callsMutex;
std::unordered_set<uint64_t>* s_seen = nullptr;

std::atomic<bool> s_logAll{false};
std::atomic<uint32_t> s_callCounter{0};

bool ExecuteDetour(void* aFunc, void* aCtx, void* aFrame, void* aRet, void* aRetType)
{
    if ((s_callCounter.fetch_add(1, std::memory_order_relaxed) & 0x1FF) == 0)
        s_logAll.store(access("/tmp/axl_calls_all", F_OK) == 0, std::memory_order_relaxed);

    const bool result = s_execOriginal(aFunc, aCtx, aFrame, aRet, aRetType);
    const auto addr = reinterpret_cast<uintptr_t>(aFunc);
    if (s_callsFd >= 0 && (addr < s_imgLo || addr >= s_imgHi))
    {
        const uint64_t fullName = *reinterpret_cast<uint64_t*>(addr + 8);
        bool emit = s_logAll.load(std::memory_order_relaxed);
        if (!emit)
        {
            std::lock_guard<std::mutex> lock(s_callsMutex);
            emit = s_seen && s_seen->insert(fullName ^ (result ? 0 : 0x8000000000000000ull)).second;
        }
        if (emit)
        {
            char buf[320];
            const char* name = RED4ext::CNamePool::Get(RED4ext::CName{fullName});
            const int n = snprintf(buf, sizeof(buf), "%s %s\n", result ? "ok  " : "FAIL", name ? name : "?");
            if (n > 0) write(s_callsFd, buf, static_cast<size_t>(n));
        }
    }
    return result;
}
}


// External ink widget spawning (SpawnFromExternal / AsyncSpawnFromExternal).
// The game only resolves an external widget library through the dependency list of the *parent's* library resource
// (DynArray at +0x50, 0x18-byte entries {path hash, token ptr, token ctrl}); that list is the dependency table of the
// vanilla .inkwidget file. Widgets from mods are never in it, so the lookup fails. This is what ArchiveXL's and
// Codeware's InkSpawner do on Windows (InjectDependency): add the requested library to the list and load it. Those
// extensions are not usable here (their addresses are not resolved on macOS), so it is done at the lookup function.
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <thread>
namespace
{
// Types with a non-trivial destructor are returned through x8 (indirect result), like the game's SharedPtr/Handle.
struct Ret16
{
    void* a;
    void* b;
    ~Ret16() {}
};

using LoaderReqFn = Ret16 (*)(void* aLoader, uint64_t aPath, void* aParent, uint64_t aFlag);
using LookupFn = Ret16 (*)(void* aLibrary, uint64_t aPath, uint64_t aItem);
using GrowFn = void (*)(void* aArray, uint32_t aNewCapacity, uint64_t aElemSize, uint64_t aAlign, void* aMove);
using TokenLoadedFn = uint32_t (*)(void* aToken);

LoaderReqFn s_loaderOriginal = nullptr;
LookupFn s_lookupOriginal = nullptr;
GrowFn s_grow = nullptr;
TokenLoadedFn s_tokenLoaded = nullptr;
std::atomic<void*> s_loader{nullptr};
std::mutex s_injectMutex;
int s_inkLogFd = -1;
uintptr_t s_slide = 0;

void InkLog(const char* aFmt, uint64_t aA = 0, uint64_t aB = 0, uint64_t aC = 0)
{
    if (s_inkLogFd < 0)
        return;
    char buf[400];
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % 100000000;
    int off = snprintf(buf, sizeof(buf), "%8lld [N] ", (long long)ms);
    const int n = snprintf(buf + off, sizeof(buf) - off, aFmt, (unsigned long long)aA, (unsigned long long)aB, (unsigned long long)aC);
    if (n > 0)
    {
        size_t len = static_cast<size_t>(off + n);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        if (buf[len - 1] != '\n') buf[len++] = '\n';
        write(s_inkLogFd, buf, len);
    }
}

// A real "parent" handle (ptr + control block) seen in one of the game's own loading requests. The loader reads its
// loading context from it, so requests of our own reuse it. We keep one strong reference so it never dies.
uint64_t s_parent[2] = {0, 0};
std::atomic<bool> s_parentReady{false};


void DumpToken(const char* aTag, void* aToken)
{
    if (!aToken)
        return;
    const auto tp = reinterpret_cast<uintptr_t>(aToken);
    char line[400];
    for (int row = 0; row < 8; ++row)
    {
        int n = snprintf(line, sizeof(line), "%s +%02x:", aTag, row * 0x10);
        for (int c = 0; c < 2; ++c)
            n += snprintf(line + n, sizeof(line) - n, " %016llx", (unsigned long long)*reinterpret_cast<volatile uint64_t*>(tp + row * 0x10 + c * 8));
        line[n++] = '\n';
        write(s_inkLogFd, line, static_cast<size_t>(n));
    }
}

bool FindLoaderToken(void* aLoader, uint64_t aPath, uint64_t aOut[2]);

Ret16 LoaderDetour(void* aLoader, uint64_t aPath, void* aParent, uint64_t aFlag)
{
    if (!s_loader.load(std::memory_order_relaxed))
        s_loader.store(aLoader, std::memory_order_relaxed);
    if (false && aParent)
    {
        auto* h = static_cast<uint64_t*>(aParent);
        if (h[0] && h[1])
        {
            auto* count = reinterpret_cast<std::atomic<uint32_t>*>(h[1]);
            uint32_t c = count->load();
            while (c != 0 && !count->compare_exchange_weak(c, c + 1)) {}
            if (c != 0)
            {
                s_parent[0] = h[0];
                s_parent[1] = h[1];
                s_parentReady.store(true, std::memory_order_release);
                InkLog("parent captured ptr=%#llx ctrl=%#llx\n", h[0], h[1]);
            }
        }
    }
    const bool watched = aPath == 0x5a413ee04843c418ull || aPath == 0xd6b2a2bceeee5160ull;
    if (watched)
    {
        auto* h = static_cast<uint64_t*>(aParent);
        InkLog("loader request path=%#llx parent=%#llx flag=%llu\n", aPath, h ? h[0] : 0, aFlag);
    }
    Ret16 result = s_loaderOriginal(aLoader, aPath, aParent, aFlag);
    if (watched)
    {
        const auto tp = reinterpret_cast<uintptr_t>(result.a);
        InkLog("   -> token=%#llx loaded=%llu failed=%llu\n", tp, tp ? *reinterpret_cast<volatile uint32_t*>(tp + 0x58) : 0,
               tp ? *reinterpret_cast<volatile uint8_t*>(tp + 0x5c) : 0);
        DumpToken(aPath == 0xd6b2a2bceeee5160ull ? "req_buttonhints" : "req_stores", result.a);
        uint64_t probe[2] = {0, 0};
        InkLog("   probe right after request: found=%llu\n", FindLoaderToken(aLoader, aPath, probe) ? 1 : 0);
    }
    return result;
}

struct GameArray
{
    void* entries;
    uint32_t capacity;
    uint32_t size;
};

struct DepEntry
{
    uint64_t path;
    void* token;
    void* ctrl;
};

bool WaitLoaded(void* aToken, int aMillis)
{
    for (int i = 0; i < aMillis; ++i)
    {
        if (s_tokenLoaded(aToken))
            return true;
        if (*reinterpret_cast<volatile uint8_t*>(reinterpret_cast<uintptr_t>(aToken) + 0x5c))
            return false; // failed
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// Read-only probe of the loader's token map (same hashing/layout as the game's own lookup at 0x10426651c): returns
// the {ptr, ctrl} of the token the game created for this path, or false when none exists (nothing is ever created).
bool FindLoaderToken(void* aLoader, uint64_t aPath, uint64_t aOut[2])
{
    auto* l = reinterpret_cast<uint8_t*>(aLoader);
    if (*reinterpret_cast<uint32_t*>(l + 8) == 0)
        return false;
    const uint32_t hash32 = static_cast<uint32_t>(aPath >> 32) ^ static_cast<uint32_t>(aPath);
    const uint32_t buckets = *reinterpret_cast<uint32_t*>(l + 0xc);
    if (!buckets)
        return false;
    auto* table = *reinterpret_cast<uint32_t**>(l);
    auto* nodes = *reinterpret_cast<uint8_t**>(l + 0x10);
    const uint32_t stride = *reinterpret_cast<uint32_t*>(l + 0x1c);
    uint32_t idx = table[hash32 % buckets];
    for (int guard = 0; idx != 0xFFFFFFFFu && guard < 64; ++guard)
    {
        auto* node = nodes + static_cast<uint64_t>(idx) * stride;
        if (*reinterpret_cast<uint32_t*>(node + 4) == hash32 && *reinterpret_cast<uint64_t*>(node + 8) == aPath)
        {
            aOut[0] = *reinterpret_cast<uint64_t*>(node + 0x10);
            aOut[1] = *reinterpret_cast<uint64_t*>(node + 0x18);
            return aOut[0] && aOut[1];
        }
        idx = *reinterpret_cast<uint32_t*>(node);
    }
    return false;
}

// Mac equivalent of ArchiveXL's/Codeware's InjectDependency: add {path} to the parent library's externalLibraries and let
// the game request the load through its own ResourceReference::LoadAsync (0x10427b5fc, which fills the token by path).
using RequestByPathFn = Ret16 (*)(void* aLoader, uint64_t aPath);
RequestByPathFn s_requestByPath = nullptr;
void** s_loaderGlobal = nullptr;

void InjectDependency(void* aLibrary, uint64_t aPath)
{
    if (!aLibrary || !aPath)
        return;

    auto* arr = reinterpret_cast<GameArray*>(reinterpret_cast<uint8_t*>(aLibrary) + 0x50);

    DepEntry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_injectMutex);

        auto* entries = static_cast<DepEntry*>(arr->entries);
        for (uint32_t i = 0; i < arr->size; ++i)
        {
            if (entries[i].path == aPath)
                return; // already a dependency (vanilla list or injected before)
        }

        if (arr->size >= arr->capacity)
            s_grow(arr, arr->capacity < 4 ? 4 : arr->capacity * 2, sizeof(DepEntry), 8, nullptr);
        if (arr->size >= arr->capacity)
            return;
        entries = static_cast<DepEntry*>(arr->entries);
        entry = &entries[arr->size];
        *entry = DepEntry{aPath, nullptr, nullptr};
        arr->size++;

        // ResourceLoader::LoadAsync(path) -> token, exactly the request ResourceReference::LoadAsync makes (0x10427b5fc
        // also blocks waiting, which the game forbids on script threads, so only its request part is used).
        void* loader = s_loaderGlobal ? *s_loaderGlobal : nullptr;
        if (loader)
        {
            Ret16 token = s_requestByPath(loader, aPath);
            entry->token = token.a;
            entry->ctrl = token.b; // the entry takes over the reference
        }
    }

    // Like the original: give the load a moment (another thread finishes it).
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = false;
    for (int i = 0; i < 1500 && !ok; ++i)
    {
        ok = entry->token && s_tokenLoaded(entry->token);
        if (!ok)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    InkLog("inject lib=%#llx path=%#llx loaded=%llu\n", reinterpret_cast<uintptr_t>(aLibrary), aPath, ok ? 1 : 0);
    InkLog("   waited=%llums token=%#llx\n", ms, reinterpret_cast<uintptr_t>(entry->token));
}

// ArchiveXL item names may carry a script controller: "Item:Namespace.ControllerClass". The game only knows "Item".
thread_local char t_pendingController[160] = {0};

uint64_t Fnv1a64(const char* aStr, size_t aLen)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < aLen; ++i)
    {
        h ^= static_cast<uint8_t>(aStr[i]);
        h *= 0x100000001B3ull;
    }
    return h;
}


// After the game turns the library item into a WidgetLibraryItemInstance, give it the script controller requested in the
// item name ("Item:Namespace.Class"), copying over the properties of the controller it came with (what ArchiveXL's
// InkSpawner::InjectController/InheritProperties do on Windows).
Red::ClassLocator<Red::ink::IWidgetController> s_gameControllerType;
Red::ClassLocator<Red::ink::WidgetLogicController> s_logicControllerType;
using InstantiateFn = void (*)(void* aThis, void* aInOutHandle, void* aOther);
InstantiateFn s_instantiateOriginal = nullptr;

void InheritProperties(Red::IScriptable* aTarget, Red::IScriptable* aSource)
{
    auto* sourceType = aSource->GetType();
    auto* targetType = aTarget->GetType();

    Red::DynArray<Red::CProperty*> sourceProps;
    sourceType->GetProperties(sourceProps);
    for (const auto& sourceProp : sourceProps)
    {
        const auto targetProp = targetType->GetProperty(sourceProp->name);
        if (targetProp && targetProp->type == sourceProp->type)
            targetProp->SetValue(aTarget, sourceProp->GetValuePtr<void>(aSource));
    }
}

// Script-created controller handed over by ArchiveXL.SetPendingController (raw ptr + control block, one strong reference kept).
uint64_t s_pendingCtrl[2] = {0, 0};
std::map<std::string, std::deque<std::array<uint64_t, 2>>> s_controllerQueues; // className -> FIFO of {ptr, ctrl}
int s_logicOffset = -1;                                                         // byte offset of inkWidget::logicController
std::mutex s_pendingCtrlMutex;

void InjectController(Red::ink::WidgetLibraryItemInstance* aInstance, const char* aControllerName, void* aRequest = nullptr)
{
    if (strstr(aControllerName, "ListItemController") && access("/tmp/axl_no_item_ctrl", F_OK) == 0)
    {
        App::MacLogLine("N", "item controller injection skipped (flag file)");
        return;
    }
    std::array<uint64_t, 2> handle{0, 0};
    {
        std::lock_guard<std::mutex> lock(s_pendingCtrlMutex);
        auto it = s_controllerQueues.find(aControllerName);
        if (it != s_controllerQueues.end() && !it->second.empty())
        {
            handle = it->second.front();
            it->second.pop_front();
        }
        else if (s_pendingCtrl[0])
        {
            handle = {s_pendingCtrl[0], s_pendingCtrl[1]};
            s_pendingCtrl[0] = s_pendingCtrl[1] = 0;
        }
    }
    if (handle[0])
    {
        const uint64_t hash = Fnv1a64(aControllerName, strlen(aControllerName));
        auto* type = Red::CRTTISystem::Get()->GetClass(Red::CName(hash));
        auto* q = reinterpret_cast<uint64_t*>(aInstance);
        reinterpret_cast<std::atomic<uint32_t>*>(handle[1])->fetch_add(1); // the owner's own reference
        char b[260];
        if (type && type->IsA(s_logicControllerType))
        {
            if (s_logicOffset > 0 && q[7])
            {
                auto* widget = reinterpret_cast<uint64_t*>(q[7]);
                widget[s_logicOffset / 8] = handle[0];
                widget[s_logicOffset / 8 + 1] = handle[1];
                snprintf(b, sizeof(b), "script LOGIC controller %s set in widget %#llx at +%#x", aControllerName, (unsigned long long)q[7], s_logicOffset);
            }
            else
                snprintf(b, sizeof(b), "logic controller %s NOT set (offset unknown=%d, widget=%#llx)", aControllerName, s_logicOffset, (unsigned long long)q[7]);
        }
        else
        {
            q[9] = handle[0];
            q[10] = handle[1];
            snprintf(b, sizeof(b), "script GAME controller %s set in instance: %#llx/%#llx", aControllerName, (unsigned long long)handle[0], (unsigned long long)handle[1]);
        }
        App::MacLogLine("N", b);
        return;
    }
    App::MacLogLine("N", "no script controller pending: nothing injected");
    return;
    const uint64_t hash = Fnv1a64(aControllerName, strlen(aControllerName));
    auto* controllerType = Red::CRTTISystem::Get()->GetClass(Red::CName(hash));
    if (!controllerType)
    {
        if (s_inkLogFd >= 0)
        {
            char b[300];
            int n = snprintf(b, sizeof(b), "controller class not found: %s\n", aControllerName);
            write(s_inkLogFd, b, static_cast<size_t>(n));
        }
        return;
    }

    if (controllerType->IsA(s_gameControllerType))
    {
        auto* controllerInstance = reinterpret_cast<Red::ink::IWidgetController*>(controllerType->CreateInstance(true));
        {
            // Diagnostics: compare the game's own controller (request +0x70) with the one created here.
            auto describe = [](const char* tag, uint64_t* obj) {
                char b[900];
                if (!obj) { snprintf(b, sizeof(b), "%s: null", tag); App::MacLogLine("N", b); return; }
                const char* cls = "?";
                if (auto* t = reinterpret_cast<Red::ISerializable*>(obj)->GetType())
                    cls = RED4ext::CNamePool::Get(t->GetName()) ? RED4ext::CNamePool::Get(t->GetName()) : "?";
                snprintf(b, sizeof(b), "%s: obj=%#llx class=%s vt=%#llx", tag, (unsigned long long)reinterpret_cast<uintptr_t>(obj), cls, (unsigned long long)(obj[0] - s_slide));
                App::MacLogLine("N", b);
                for (int row = 0; row < 12; ++row)
                {
                    snprintf(b, sizeof(b), "   +%02x: %016llx %016llx", row * 16, (unsigned long long)obj[row * 2], (unsigned long long)obj[row * 2 + 1]);
                    App::MacLogLine("N", b);
                }
            };
            describe("NEW controller", reinterpret_cast<uint64_t*>(controllerInstance));
            if (aRequest)
                describe("OLD request controller", *reinterpret_cast<uint64_t**>(reinterpret_cast<uint8_t*>(aRequest) + 0x70));
            char b[200];
            snprintf(b, sizeof(b), "class size of target=%u", (unsigned)controllerType->GetSize());
            App::MacLogLine("N", b);
            if (auto* base = Red::CRTTISystem::Get()->GetClass(Red::CName(Fnv1a64("inkGameController", 17))))
            {
                snprintf(b, sizeof(b), "class size of inkGameController=%u", (unsigned)base->GetSize());
                App::MacLogLine("N", b);
            }
        }
        if (aRequest && access("/tmp/axl_req_swap", F_OK) == 0)
        {
            // The spawning request carries the game controller (+0x70) that the game later binds/initializes and copies to the
            // instance. Swap only that one. The holder is heap-allocated and never destroyed on purpose: releasing the old
            // controller's last reference calls Handle_DecWeakRef, which is unresolved on macOS.
            auto* requestController = reinterpret_cast<Red::Handle<Red::ink::IWidgetController>*>(reinterpret_cast<uint8_t*>(aRequest) + 0x70);
            auto* holder = new Red::Handle<Red::ink::IWidgetController>(controllerInstance);
            requestController->Swap(*holder);
            InkLog("request game controller replaced (old kept alive)\n");
        }
        else
        {
            Red::Handle<Red::ink::IWidgetController> controllerHandle(controllerInstance);
            aInstance->gameController.Swap(controllerHandle);
            InkLog("instance game controller replaced\n");
        }
    }
    else if (controllerType->IsA(s_logicControllerType))
    {
        auto* controllerInstance = reinterpret_cast<Red::ink::WidgetLogicController*>(controllerType->CreateInstance(true));
        Red::Handle<Red::ink::WidgetLogicController> controllerHandle(controllerInstance);
        aInstance->rootWidget->logicController.Swap(controllerHandle);
        if (controllerHandle.instance)
            InheritProperties(controllerInstance, controllerHandle.instance);
        InkLog("controller injected (logic controller)\n");
    }
    else
        InkLog("controller class is neither game nor logic controller\n");
}

void InstallGetClassProbe();

extern std::map<uint64_t, std::string> s_asyncRequestClass;
extern std::mutex s_asyncRequestMutex;

void PreInstantiate(void* aThis, void* aInOutHandle)
{
    {
        std::lock_guard<std::mutex> lock(s_asyncRequestMutex);
        auto it = s_asyncRequestClass.find(reinterpret_cast<uint64_t>(aThis));
        if (it != s_asyncRequestClass.end())
        {
            strncpy(t_pendingController, it->second.c_str(), sizeof(t_pendingController) - 1);
            App::MacLogLine("N", "PreInstantiate: controller class taken from the async request");
            s_asyncRequestClass.erase(it);
        }
    }
    {
        static bool s_logged = false;
        if (!s_logged && aThis)
        {
            s_logged = true;
            void** vtable = *reinterpret_cast<void***>(aThis);
            char b[300];
            snprintf(b, sizeof(b), "Instantiate this=%#llx vtable=%#llx slot0x168=%#llx (va without slide: %#llx)", (unsigned long long)reinterpret_cast<uintptr_t>(aThis),
                     (unsigned long long)reinterpret_cast<uintptr_t>(vtable), (unsigned long long)reinterpret_cast<uintptr_t>(vtable[0x168 / 8]),
                     (unsigned long long)(reinterpret_cast<uintptr_t>(vtable[0x168 / 8]) - s_slide));
            App::MacLogLine("N", b);
        }
    }
    // The game reads instance->gameController (+0x48) INSIDE this function (binds it and queues it for OnInitialize), so the
    // script-created controller must be in place BEFORE the original runs. On entry *aInOutHandle is already the instance.
    {
        static bool s_classProbe = false;
        if (!s_classProbe && t_pendingController[0])
        {
            s_classProbe = true;
            InstallGetClassProbe();
            const char* names[] = {"inkVirtualCompoundItemController", "inkVirtualGridController"};
            for (const char* n : names)
            {
                auto* c = Red::CRTTISystem::Get()->GetClass(Red::CName(Fnv1a64(n, strlen(n))));
                char b[200];
                snprintf(b, sizeof(b), "class probe %-60s -> %#llx", n, (unsigned long long)reinterpret_cast<uintptr_t>(c));
                App::MacLogLine("N", b);
            }
        }
    }
    if (t_pendingController[0] && aInOutHandle)
    {
        char name[sizeof(t_pendingController)];
        memcpy(name, t_pendingController, sizeof(name));
        t_pendingController[0] = 0;
        auto* handle = static_cast<uint64_t*>(aInOutHandle);
        if (handle[0])
        {
            auto* obj = reinterpret_cast<Red::ISerializable*>(handle[0]);
            const char* clsName = "?";
            if (auto* type = obj->GetType())
                clsName = RED4ext::CNamePool::Get(type->GetName()) ? RED4ext::CNamePool::Get(type->GetName()) : "?";
            char b[200];
            snprintf(b, sizeof(b), "Instantiate ENTRY obj=%#llx class=%s -> injecting before original", (unsigned long long)handle[0], clsName);
            App::MacLogLine("N", b);
            if (strcmp(clsName, "inkWidgetLibraryItemInstance") == 0)
                InjectController(reinterpret_cast<Red::ink::WidgetLibraryItemInstance*>(handle[0]), name, nullptr);
        }
    }
}

void InstantiateDetour(void* aThis, void* aInOutHandle, void* aOther)
{
    PreInstantiate(aThis, aInOutHandle);
    s_instantiateOriginal(aThis, aInOutHandle, aOther);
    if (t_pendingController[0] && aThis)
    {
        auto* r = reinterpret_cast<uint64_t*>(aThis);
        char b[600];
        snprintf(b, sizeof(b), "request fields: +48=%#llx +50=%#llx/%#llx +60=%#llx/%#llx +70=%#llx/%#llx +80=%#llx/%#llx +90=%#llx/%#llx +A0=%#llx +C8=%#llx",
                 (unsigned long long)r[9], (unsigned long long)r[10], (unsigned long long)r[11], (unsigned long long)r[12], (unsigned long long)r[13],
                 (unsigned long long)r[14], (unsigned long long)r[15], (unsigned long long)r[16], (unsigned long long)r[17], (unsigned long long)r[18],
                 (unsigned long long)r[19], (unsigned long long)r[20], (unsigned long long)r[25]);
        App::MacLogLine("N", b);
    }
    InkLog("Instantiate -> instance=%#llx pendingController=%llu\n", static_cast<uint64_t*>(aInOutHandle) ? static_cast<uint64_t*>(aInOutHandle)[0] : 0, t_pendingController[0] ? 1 : 0);
}

// Twin of the pair above used by another spawn path (probably the async/virtual-list items).
using Instantiate2Fn = void (*)(void* aThis, void* aInOutHandle, void* aOther);
Instantiate2Fn s_instantiate2Original = nullptr;

void Instantiate2Detour(void* aThis, void* aInOutHandle, void* aOther)
{
    App::MacLogLine("N", "Instantiate2 called");
    PreInstantiate(aThis, aInOutHandle);
    s_instantiate2Original(aThis, aInOutHandle, aOther);
}

// Top-level spawn (Spawn2): log what the script gets back.
using Spawn2Fn = Ret16 (*)(void* aParent, void* aLib, uint64_t aPath, uint64_t aItem);
Spawn2Fn s_spawn2Original = nullptr;

Ret16 Spawn2Detour(void* aParent, void* aLib, uint64_t aPath, uint64_t aItem)
{
    Ret16 r = s_spawn2Original(aParent, aLib, aPath, aItem);
    if (aPath == 0x5a413ee04843c418ull || aPath == 0xd6b2a2bceeee5160ull)
    {
        const char* clsName = "?";
        if (r.a)
        {
            auto* obj = reinterpret_cast<Red::ISerializable*>(r.a);
            if (auto* type = obj->GetType())
                clsName = RED4ext::CNamePool::Get(type->GetName()) ? RED4ext::CNamePool::Get(type->GetName()) : "?";
        }
        char b[300];
        snprintf(b, sizeof(b), "Spawn2 path=%#llx returns obj=%#llx class=%s", (unsigned long long)aPath, (unsigned long long)reinterpret_cast<uintptr_t>(r.a), clsName);
        App::MacLogLine("N", b);
    }
    return r;
}

// Diagnostic: log class names the game looks up through CRTTISystem::GetClass and does not find (unique names only).
using GetClassFn = void* (*)(void* aSystem, uint64_t aName);
GetClassFn s_getClassOriginal = nullptr;
std::unordered_set<uint64_t>* s_missingClasses = nullptr;
std::mutex s_missingMutex;

void* GetClassDetour(void* aSystem, uint64_t aName)
{
    void* result = s_getClassOriginal(aSystem, aName);
    {
        // Every lookup (hit or miss) of watched script classes, no dedupe: shows how the game refers to them.
        static const uint64_t watched[] = {Fnv1a64("ExampleModController", 20)};
        for (uint64_t w : watched)
            if (w == aName)
            {
                char b[200];
                snprintf(b, sizeof(b), "GetClass %s hash=%#llx -> %s", "watched-class", (unsigned long long)aName, result ? "FOUND" : "MISS");
                App::MacLogLine("N", b);
            }
    }
    if (!result)
    {
        std::lock_guard<std::mutex> lock(s_missingMutex);
        if (!s_missingClasses)
            s_missingClasses = new std::unordered_set<uint64_t>();
        if (s_missingClasses->size() < 400 && s_missingClasses->insert(aName).second)
        {
            const char* n = RED4ext::CNamePool::Get(RED4ext::CName{aName});
            char b[260];
            snprintf(b, sizeof(b), "GetClass MISS name=%s (hash %#llx)", n ? n : "?", (unsigned long long)aName);
            App::MacLogLine("N", b);
        }
    }
    return result;
}

void InstallGetClassProbe()
{
    static bool s_done = false;
    if (s_done)
        return;
    s_done = true;
    void** vtable = *reinterpret_cast<void***>(Red::CRTTISystem::Get());
    void* target = vtable[0x10 / 8];
    auto& driver = Core::HookingDriver::GetDefault();
    driver.HookAttach(reinterpret_cast<uintptr_t>(target), reinterpret_cast<void*>(&GetClassDetour), reinterpret_cast<void**>(&s_getClassOriginal));
    App::MacLogLine("N", "GetClass probe installed");
}

// Local spawn (library item by name inside the parent's own library): the same "Item:Class" names appear here (e.g. the
// templates of a virtual list). Log the requested name and strip the class suffix so the item can be found.
using SpawnLocalFn = Ret16 (*)(void* aThis, void* aLibHandle, void* aCtx, uint64_t aItem);
SpawnLocalFn s_spawnLocalOriginal = nullptr;

Ret16 SpawnLocalDetour(void* aThis, void* aLibHandle, void* aCtx, uint64_t aItem)
{
    const char* name = RED4ext::CNamePool::Get(RED4ext::CName{aItem});
    uint64_t item = aItem;
    if (name && name[0])
    {
        if (const char* sep = strchr(name, ':'))
        {
            item = Fnv1a64(name, static_cast<size_t>(sep - name));
            strncpy(t_pendingController, sep + 1, sizeof(t_pendingController) - 1);
            char b[300];
            snprintf(b, sizeof(b), "SpawnLocal item '%s' -> split controller '%s'", name, sep + 1);
            App::MacLogLine("N", b);
        }
        else
        {
            char b[300];
            snprintf(b, sizeof(b), "SpawnLocal item '%s'", name);
            App::MacLogLine("N", b);
        }
    }
    else
    {
        char b[100];
        snprintf(b, sizeof(b), "SpawnLocal item hash=%#llx (no name)", (unsigned long long)aItem);
        App::MacLogLine("N", b);
    }
    return s_spawnLocalOriginal(aThis, aLibHandle, aCtx, item);
}

using SpawnLocal2Fn = Ret16 (*)(void* aThis, void* aLibHandle, void* aCtx, uint64_t aItem);
SpawnLocal2Fn s_spawnLocal2Original = nullptr;

Ret16 SpawnLocal2Detour(void* aThis, void* aLibHandle, void* aCtx, uint64_t aItem)
{
    const char* name = RED4ext::CNamePool::Get(RED4ext::CName{aItem});
    uint64_t item = aItem;
    char b[300];
    if (name && name[0])
    {
        if (const char* sep = strchr(name, ':'))
        {
            item = Fnv1a64(name, static_cast<size_t>(sep - name));
            strncpy(t_pendingController, sep + 1, sizeof(t_pendingController) - 1);
            snprintf(b, sizeof(b), "SpawnLocal2 item '%s' -> split controller '%s'", name, sep + 1);
        }
        else
            snprintf(b, sizeof(b), "SpawnLocal2 item '%s'", name);
    }
    else
        snprintf(b, sizeof(b), "SpawnLocal2 item hash=%#llx (no name)", (unsigned long long)aItem);
    App::MacLogLine("N", b);
    return s_spawnLocal2Original(aThis, aLibHandle, aCtx, item);
}

// Async spawn (used by virtual lists for their real items): a = spawner, b = Handle<request>; the item name lives at
// *(*(request)+0x78)+0x40. Strip the ":Class" suffix there and remember the class for the request, because the instance
// is created later on another thread.
using AsyncSpawnFn = uint64_t (*)(void* aThis, void* aRequestHandle, void* aOther);
AsyncSpawnFn s_asyncSpawnOriginal = nullptr;
std::map<uint64_t, std::string> s_asyncRequestClass; // request pointer -> controller class
std::mutex s_asyncRequestMutex;

uint64_t AsyncSpawnDetour(void* aThis, void* aRequestHandle, void* aOther)
{
    auto* handle = static_cast<uint64_t*>(aRequestHandle);
    if (handle && handle[0])
    {
        auto* request = reinterpret_cast<uint64_t*>(handle[0]);
        auto* inner = reinterpret_cast<uint64_t*>(request[0x78 / 8]);
        if (inner)
        {
            const uint64_t nameHash = inner[0x40 / 8];
            const char* name = RED4ext::CNamePool::Get(RED4ext::CName{nameHash});
            char b[300];
            if (name && name[0])
            {
                if (const char* sep = strchr(name, ':'))
                {
                    inner[0x40 / 8] = Fnv1a64(name, static_cast<size_t>(sep - name));
                    {
                        std::lock_guard<std::mutex> lock(s_asyncRequestMutex);
                        s_asyncRequestClass[reinterpret_cast<uint64_t>(request)] = sep + 1;
                    }
                    snprintf(b, sizeof(b), "AsyncSpawn request=%#llx item '%s' -> split controller '%s'", (unsigned long long)reinterpret_cast<uint64_t>(request), name, sep + 1);
                }
                else
                    snprintf(b, sizeof(b), "AsyncSpawn request=%#llx item '%s'", (unsigned long long)reinterpret_cast<uint64_t>(request), name);
            }
            else
                snprintf(b, sizeof(b), "AsyncSpawn request=%#llx item hash=%#llx (no name)", (unsigned long long)reinterpret_cast<uint64_t>(request), (unsigned long long)nameHash);
            App::MacLogLine("N", b);
        }
    }
    return s_asyncSpawnOriginal(aThis, aRequestHandle, aOther);
}

Ret16 LookupDetour(void* aLibrary, uint64_t aPath, uint64_t aItem)
{
    InjectDependency(aLibrary, aPath);
    t_pendingController[0] = 0;

    const char* name = RED4ext::CNamePool::Get(RED4ext::CName{aItem});
    if (name)
    {
        if (const char* sep = strchr(name, ':'))
        {
            aItem = Fnv1a64(name, static_cast<size_t>(sep - name));
            strncpy(t_pendingController, sep + 1, sizeof(t_pendingController) - 1);
            InkLog("item '%s' -> controller split\n", 0, 0, 0);
            if (s_inkLogFd >= 0)
            {
                char b[300];
                int n = snprintf(b, sizeof(b), "   full name: %s\n", name);
                write(s_inkLogFd, b, static_cast<size_t>(n));
            }
        }
    }
    Ret16 found = s_lookupOriginal(aLibrary, aPath, aItem);
    InkLog("Lookup lib=%#llx path=%#llx -> item found=%llu\n", reinterpret_cast<uintptr_t>(aLibrary), aPath, found.a ? 1 : 0);
    return found;
}
}

namespace App { void MacLogLine(const char* aPrefix, const char* aText); }
namespace App
{
void MacDumpObject(uint64_t aPtr, const char* aTag)
{
    if (!aPtr)
        return;
    auto* q = reinterpret_cast<uint64_t*>(aPtr);
    char b[200];
    for (int row = 0; row < 0x30; ++row) // 0x300 bytes
    {
        snprintf(b, sizeof(b), "DUMP %s +%03x: %016llx %016llx", aTag, row * 16, (unsigned long long)q[row * 2], (unsigned long long)q[row * 2 + 1]);
        MacLogLine("N", b);
    }
}

void MacQueueController(uint64_t aPtr, uint64_t aCtrl, const char* aClass)
{
    if (!aPtr || !aCtrl || !aClass)
        return;
    std::lock_guard<std::mutex> lock(s_pendingCtrlMutex);
    reinterpret_cast<std::atomic<uint32_t>*>(aCtrl)->fetch_add(1); // keep alive until consumed
    s_controllerQueues[aClass].push_back({aPtr, aCtrl});
    char b[200];
    snprintf(b, sizeof(b), "controller queued for %s (queue size %zu)", aClass, s_controllerQueues[aClass].size());
    MacLogLine("N", b);
}

void MacProbeLogicOffset(uint64_t aWidget, uint64_t aCtrl)
{
    if (!aWidget || !aCtrl || s_logicOffset > 0)
        return;
    auto* w = reinterpret_cast<uint64_t*>(aWidget);
    for (int i = 4; i < 0x80; ++i)
        if (w[i] == aCtrl)
        {
            s_logicOffset = i * 8;
            char b[160];
            snprintf(b, sizeof(b), "logicController offset in inkWidget found: +%#x", s_logicOffset);
            MacLogLine("N", b);
            return;
        }
    MacLogLine("N", "logicController offset NOT found");
}

void MacSetPendingController(uint64_t aPtr, uint64_t aCtrl)
{
    if (!aPtr || !aCtrl)
        return;
    std::lock_guard<std::mutex> lock(s_pendingCtrlMutex);
    reinterpret_cast<std::atomic<uint32_t>*>(aCtrl)->fetch_add(1); // keep it alive until the spawn consumes it
    s_pendingCtrl[0] = aPtr;
    s_pendingCtrl[1] = aCtrl;
    MacLogLine("N", "pending script controller registered");
}

void MacLogLine(const char* aPrefix, const char* aText)
{
    if (s_inkLogFd < 0 || !aText)
        return;
    char buf[1200];
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % 100000000;
    int n = snprintf(buf, sizeof(buf), "%8lld [%s] %s\n", (long long)ms, aPrefix, aText);
    if (n > 0)
        write(s_inkLogFd, buf, static_cast<size_t>(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
    if (aPrefix[0] == 'S' && strstr(aText, "ClassifyItem"))
    {
        void* frames[24];
        int count = backtrace(frames, 24);
        char line[600];
        int off = snprintf(line, sizeof(line), "%8lld [N] BT:", (long long)ms);
        for (int i = 0; i < count && off < (int)sizeof(line) - 20; ++i)
            off += snprintf(line + off, sizeof(line) - off, " %#llx", (unsigned long long)(reinterpret_cast<uintptr_t>(frames[i]) - s_slide));
        line[off++] = '\n';
        write(s_inkLogFd, line, static_cast<size_t>(off));
    }
}

void StartMacCompat()
{
    auto& resolver = Core::AddressResolver::GetDefault();
    auto& driver = Core::HookingDriver::GetDefault();

    {
        s_callsFd = open("/tmp/axl_calls.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        s_seen = new std::unordered_set<uint64_t>();
        s_seen->reserve(4096);
        s_imgLo = reinterpret_cast<uintptr_t>(_NSGetMachExecuteHeader());
        s_imgHi = s_imgLo + 0x9000000;
        if (auto a = resolver.ResolveAddress(0x1817231Du))
            driver.HookAttach(a, reinterpret_cast<void*>(&ExecuteDetour), reinterpret_cast<void**>(&s_execOriginal));
    }
    if (auto a = resolver.ResolveAddress(1296154627u))
        driver.HookAttach(a, reinterpret_cast<void*>(&ValidatePropertyType), reinterpret_cast<void**>(&s_propOriginal));


    {
        const uintptr_t slide = reinterpret_cast<uintptr_t>(_NSGetMachExecuteHeader()) - 0x100000000ull;
        s_slide = slide;
        s_inkLogFd = open("/tmp/axl_va.log", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
        s_grow = reinterpret_cast<GrowFn>(slide + 0x1000280c8ull);
        s_tokenLoaded = reinterpret_cast<TokenLoadedFn>(slide + 0x104276974ull);
        s_requestByPath = reinterpret_cast<RequestByPathFn>(slide + 0x10426901cull);
        driver.HookAttach(slide + 0x1048cf68cull, reinterpret_cast<void*>(&SpawnLocalDetour), reinterpret_cast<void**>(&s_spawnLocalOriginal));
        driver.HookAttach(slide + 0x1048b9c10ull, reinterpret_cast<void*>(&AsyncSpawnDetour), reinterpret_cast<void**>(&s_asyncSpawnOriginal));
        driver.HookAttach(slide + 0x1048ca910ull, reinterpret_cast<void*>(&SpawnLocal2Detour), reinterpret_cast<void**>(&s_spawnLocal2Original));
        driver.HookAttach(slide + 0x1048caaf8ull, reinterpret_cast<void*>(&Instantiate2Detour), reinterpret_cast<void**>(&s_instantiate2Original));
        driver.HookAttach(slide + 0x1048258acull, reinterpret_cast<void*>(&Spawn2Detour), reinterpret_cast<void**>(&s_spawn2Original));
        driver.HookAttach(slide + 0x1048cf894ull, reinterpret_cast<void*>(&InstantiateDetour), reinterpret_cast<void**>(&s_instantiateOriginal));
        s_loaderGlobal = reinterpret_cast<void**>(slide + 0x10904a7b0ull);
        driver.HookAttach(slide + 0x10426651cull, reinterpret_cast<void*>(&LoaderDetour), reinterpret_cast<void**>(&s_loaderOriginal));
        driver.HookAttach(slide + 0x10495a9d8ull, reinterpret_cast<void*>(&LookupDetour), reinterpret_cast<void**>(&s_lookupOriginal));
    }

#define LENIENT(N, ID) if (auto a = resolver.ResolveAddress(ID)) driver.HookAttach(a, reinterpret_cast<void*>(&LenientValidator<N>::Detour), reinterpret_cast<void**>(&LenientValidator<N>::original));
    LENIENT(0, 1296154640u) LENIENT(1, 1296154641u) LENIENT(2, 1296154642u) LENIENT(3, 1296154643u) LENIENT(4, 1296154644u)
#undef LENIENT
}
}
#endif
