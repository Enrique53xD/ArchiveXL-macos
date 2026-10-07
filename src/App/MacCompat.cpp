#ifdef __APPLE__
// macOS port: fixes for game quirks that would otherwise stop scripts from loading.
#include "Core/Hooking/HookingDriver.hpp"
#include "Core/Memory/AddressResolver.hpp"

#include <algorithm>
#include <fstream>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <thread>
#include <vector>

#include "MacCounters.inc" // generated: g_countHits / g_countOrig / kCountDetours (naked trampolines that only count)

namespace App { void MacLogLine(const char* aPrefix, const char* aText); void DepotLog(char aKind, uint64_t aPath, int aOk); void WatchHit(const char* aWhere, uint64_t aValue); }

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
#include <signal.h>
#include <cstdarg>
#include <unordered_map>
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

// ---- Resource path aliases: /tmp/axl_alias.txt holds "<from hash> <to hash>" lines (hex). The engine never resolves the
// dynamic paths of ArchiveXL items (e.g. "*base\...\atomiic_urbansprinter_{gender}_{body}.mesh"), so requests for the literal
// path are redirected to the real resource. Applied wherever a path hash enters the loader or the depot. ----
std::unordered_map<uint64_t, uint64_t> s_pathAlias;
std::once_flag s_pathAliasOnce;

uint64_t AliasPath(uint64_t aPath)
{
    std::call_once(s_pathAliasOnce, []() {
        std::ifstream in("/tmp/axl_alias.txt");
        std::string from, to;
        while (in >> from >> to)
            s_pathAlias[std::strtoull(from.c_str(), nullptr, 16)] = std::strtoull(to.c_str(), nullptr, 16);
    });
    if (s_pathAlias.empty())
        return aPath;
    const auto it = s_pathAlias.find(aPath);
    if (it == s_pathAlias.end())
        return aPath;
    App::DepotLog('X', aPath, static_cast<int>(it->second & 0x7fffffff));
    return it->second;
}

Ret16 LoaderDetour(void* aLoader, uint64_t aPath, void* aParent, uint64_t aFlag)
{
    aPath = AliasPath(aPath);
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
    App::DepotLog('L', aPath, 0);
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
            const char* names[] = {"AtelierStoresListItemController", "VirtualAtelier.UI.AtelierStoresListItemController",
                                   "AtelierStoresListController", "VirtualAtelier.UI.AtelierStoresListController",
                                   "AtelierStoresTemplateClassifier", "VirtualAtelier.UI.AtelierStoresTemplateClassifier",
                                   "AtelierStoresDataView", "VirtualAtelier.UI.AtelierStoresDataView",
                                   "inkVirtualCompoundItemController", "inkVirtualGridController", "VirtualShop", "VirtualAtelier.Core.VirtualShop"};
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
        // Every lookup (hit or miss) of the VA script classes, no dedupe: shows how the game refers to them.
        static const uint64_t watched[] = {
            Fnv1a64("AtelierStoresListItemController", 31), Fnv1a64("VirtualAtelier.UI.AtelierStoresListItemController", 50),
            Fnv1a64("AtelierStoresListController", 27), Fnv1a64("VirtualAtelier.UI.AtelierStoresListController", 46),
            Fnv1a64("AtelierStoresTemplateClassifier", 31), Fnv1a64("VirtualAtelier.UI.AtelierStoresTemplateClassifier", 49)};
        for (uint64_t w : watched)
            if (w == aName)
            {
                char b[200];
                snprintf(b, sizeof(b), "GetClass %s hash=%#llx -> %s", "VA-class", (unsigned long long)aName, result ? "FOUND" : "MISS");
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

// Diagnostic: log every distinct resource path hash the game asks the depot about (ResourceDepot::ResourceExists) and the
// answer, to /tmp/axl_depot.log as "<hash> <0|1>". Lets us see whether an outfit's entity/factory is ever looked up.
int s_depotCheckFd = -1;
std::mutex s_depotCheckMutex;
std::unordered_set<uint64_t> s_depotLogSeen;

// Backtrace tracing, toggled with /tmp/axl_trace_on and /tmp/axl_trace_off: while on, the first request of every distinct
// path logs "B <kind> <hash> <return addresses as file VAs>" (frame-pointer walk) to the same log.
bool SafeRead(uint64_t aAddress, void* aOut, size_t aLength);
std::atomic<bool> s_traceOn{false};
std::unordered_set<uint64_t> s_traceSeen;

void TraceBacktrace(char aKind, uint64_t aPath)
{
    if (!s_traceOn.load(std::memory_order_relaxed) || s_depotCheckFd < 0)
        return;
    // Walk the frame-pointer chain first: when no path is given (aPath == 0) the call site itself is the dedupe key.
    uint64_t frames[16];
    int count = 0;
    uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    for (int depth = 0; depth < 16 && fp > 0x1000 && (fp & 7) == 0; ++depth)
    {
        uintptr_t frame[2];
        if (!SafeRead(fp, frame, sizeof(frame)))
            break;
        frames[count++] = frame[1] - s_slide;
        if (frame[0] <= fp)
            break;
        fp = frame[0];
    }
    const uint64_t key = (aPath ? aPath : (count ? frames[0] : 0)) ^ (static_cast<uint64_t>(static_cast<unsigned char>(aKind)) << 56);
    {
        std::lock_guard<std::mutex> lock(s_depotCheckMutex);
        if (s_traceSeen.size() > 200000 || !s_traceSeen.insert(key).second)
            return;
    }
    char line[512];
    int n = snprintf(line, sizeof(line), "B %c %016llx", aKind, static_cast<unsigned long long>(aPath));
    for (int i = 0; i < count; ++i)
        n += snprintf(line + n, sizeof(line) - n, " %llx", static_cast<unsigned long long>(frames[i]));
    line[n++] = '\n';
    std::lock_guard<std::mutex> lock(s_depotCheckMutex);
    write(s_depotCheckFd, line, static_cast<size_t>(n));
}

// One line per distinct (kind, path): "<kind> <hash> <ok>". kind E = depot Exists, R = depot Request, L = loader request.
void DepotLog(char aKind, uint64_t aPath, int aOk)
{
    if (s_depotCheckFd < 0)
        return;
    TraceBacktrace(aKind, aPath);
    {
        const char where[2] = {aKind, 0};
        WatchHit(where, aPath);
    }
    const uint64_t key = aPath ^ (static_cast<uint64_t>(static_cast<unsigned char>(aKind)) * 0x9E3779B97F4A7C15ull);
    std::lock_guard<std::mutex> lock(s_depotCheckMutex);
    if (s_depotLogSeen.size() < 400000 && s_depotLogSeen.insert(key).second)
    {
        char line[56];
        const int n = snprintf(line, sizeof(line), "%c %016llx %d\n", aKind, static_cast<unsigned long long>(aPath), aOk);
        write(s_depotCheckFd, line, static_cast<size_t>(n));
    }
}

// ---- Watchlist: /tmp/axl_watch.txt holds "<hex hash> <label>" lines (resource path hashes, CName hashes). Read once. ----
std::vector<std::pair<uint64_t, std::string>> s_watch; // sorted by hash
std::once_flag s_watchOnce;

void LoadWatch()
{
    std::call_once(s_watchOnce, []() {
        std::ifstream in("/tmp/axl_watch.txt");
        std::string h, label;
        while (in >> h >> label)
            s_watch.emplace_back(std::strtoull(h.c_str(), nullptr, 16), label);
        std::sort(s_watch.begin(), s_watch.end());
        s_watch.shrink_to_fit();
    });
}

const std::string* WatchLabel(uint64_t aValue)
{
    LoadWatch();
    auto it = std::lower_bound(s_watch.begin(), s_watch.end(), std::make_pair(aValue, std::string()));
    return (it != s_watch.end() && it->first == aValue) ? &it->second : nullptr;
}

std::unordered_set<uint64_t> s_watchReported;

void WatchHit(const char* aWhere, uint64_t aValue)
{
    if (const auto* label = WatchLabel(aValue))
    {
        std::lock_guard<std::mutex> lock(s_depotCheckMutex);
        if (s_watchReported.insert(aValue ^ std::hash<std::string>()(aWhere)).second && s_depotCheckFd >= 0)
        {
            char line[200];
            const int n = snprintf(line, sizeof(line), "HIT %s %016llx %s\n", aWhere, static_cast<unsigned long long>(aValue), label->c_str());
            write(s_depotCheckFd, line, static_cast<size_t>(n));
        }
    }
}

bool SafeRead(uint64_t aAddress, void* aOut, size_t aLength)
{
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), static_cast<vm_address_t>(aAddress), aLength, reinterpret_cast<vm_address_t>(aOut), &got) == KERN_SUCCESS && got == aLength;
}

// Checks 8 raw register arguments of a probed function: the value itself and, when it looks like a pointer, the first 0x120
// bytes it points to (resource paths usually travel inside request structures).
void ProbeScan(const char* aName, const uint64_t* aArgs)
{
    LoadWatch();
    if (s_watch.empty())
        return;
    for (int i = 0; i < 8; ++i)
    {
        const uint64_t v = aArgs[i];
        char where[64];
        snprintf(where, sizeof(where), "%s.arg%d", aName, i);
        WatchHit(where, v);
        if (v > 0x100000000ull && v < 0x7fffffffffffull && (v & 7) == 0)
        {
            uint64_t buf[0x120 / 8];
            if (SafeRead(v, buf, sizeof(buf)))
                for (size_t k = 0; k < 0x120 / 8; ++k)
                {
                    // Request structures carry the path hash inline: redirect aliased paths in place, before the call proceeds.
                    if (const uint64_t to = AliasPath(buf[k]); to != buf[k])
                    {
                        *reinterpret_cast<volatile uint64_t*>(v + k * 8) = to;
                        buf[k] = to;
                    }
                    if (WatchLabel(buf[k]))
                    {
                        snprintf(where, sizeof(where), "%s.arg%d+%#zx", aName, i, k * 8);
                        WatchHit(where, buf[k]);
                    }
                    // Second level: the request usually points at the resource token, which keeps its own copy of the path
                    // (token+0x48). The loader reads the path from the token, so redirect it there too.
                    if (!s_pathAlias.empty() && buf[k] > 0x100000000ull && buf[k] < 0x7fffffffffffull && (buf[k] & 7) == 0 && buf[k] != v)
                    {
                        uint64_t inner[0x80 / 8];
                        if (SafeRead(buf[k], inner, sizeof(inner)))
                            for (size_t m = 0; m < 0x80 / 8; ++m)
                                if (const uint64_t to = AliasPath(inner[m]); to != inner[m])
                                {
                                    *reinterpret_cast<volatile uint64_t*>(buf[k] + m * 8) = to;
                                    char line[160];
                                    snprintf(line, sizeof(line), "alias rewrite in %s.arg%d+%#zx -> +%#zx: %016llx -> %016llx", aName, i, k * 8, m * 8,
                                             static_cast<unsigned long long>(inner[m]), static_cast<unsigned long long>(to));
                                    MacLogLine("A", line);
                                }
                    }
                }
        }
    }
}

// Generic probes: forward all 8 integer args untouched (and x8 for indirect results) and scan them.
using ProbeX0Fn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using ProbeSretFn = Ret16 (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
void* s_probeOriginal[8] = {};
char s_probeName[8][24] = {};
std::atomic<uint32_t> s_probeCalls[8];
constexpr char kProbeKind[8] = {'Q', 'J', 'S', 'T', 'A', '5', '6', '7'}; // reqByPath, loadReq, submit, tokenInit, loadAsync

template<int I>
uint64_t ProbeX0(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    a0 = AliasPath(a0); a1 = AliasPath(a1); a2 = AliasPath(a2); a3 = AliasPath(a3);
    a4 = AliasPath(a4); a5 = AliasPath(a5); a6 = AliasPath(a6); a7 = AliasPath(a7);
    const uint64_t args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    if (s_probeCalls[I].fetch_add(1, std::memory_order_relaxed) < 400000)
        ProbeScan(s_probeName[I], args);
    TraceBacktrace(kProbeKind[I], 0);
    return reinterpret_cast<ProbeX0Fn>(s_probeOriginal[I])(a0, a1, a2, a3, a4, a5, a6, a7);
}

template<int I>
Ret16 ProbeSret(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    a0 = AliasPath(a0); a1 = AliasPath(a1); a2 = AliasPath(a2); a3 = AliasPath(a3); // a path hash may arrive as a plain argument
    a4 = AliasPath(a4); a5 = AliasPath(a5); a6 = AliasPath(a6); a7 = AliasPath(a7);
    const uint64_t args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    if (s_probeCalls[I].fetch_add(1, std::memory_order_relaxed) < 400000)
        ProbeScan(s_probeName[I], args);
    TraceBacktrace(kProbeKind[I], a1);
    return reinterpret_cast<ProbeSretFn>(s_probeOriginal[I])(a0, a1, a2, a3, a4, a5, a6, a7);
}

template<int I>
void AttachProbe(uintptr_t aAddress, bool aSret, const char* aName)
{
    snprintf(s_probeName[I], sizeof(s_probeName[I]), "%s", aName);
    auto& driver = Core::HookingDriver::GetDefault();
    if (aSret)
        driver.HookAttach(aAddress, reinterpret_cast<void*>(&ProbeSret<I>), &s_probeOriginal[I]);
    else
        driver.HookAttach(aAddress, reinterpret_cast<void*>(&ProbeX0<I>), &s_probeOriginal[I]);
}

// ---- Dynamic appearance names ("base!variant+attr=value%hash", ArchiveXL's convention for outfit colour variants) ----
// The game's `Entity/Spawn/LoadAppearance` scheduler (0x103d798b0: x0 = entity template, x3 = request) looks the requested
// appearance name (request +0x28) up in template->appearances and silently does nothing when it is not there. Names with
// '!' never exist in the template, so strip the dynamic part before the lookup. Every distinct name is logged as
// "N <hash> <before> -> <after>" for diagnostics.
using SpawnLoadAppearanceFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
SpawnLoadAppearanceFn s_spawnLoadAppearanceOriginal = nullptr;
std::unordered_set<uint64_t> s_dynNameSeen;

uint64_t SpawnLoadAppearanceDetour(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    uint64_t name = 0;
    if (a3 > 0x100000000ull && a3 < 0x7fffffffffffull && SafeRead(a3 + 0x28, &name, 8) && name)
    {
        const char* text = Red::CNamePool::Get(Red::CName(name));
        if (text)
        {
            const char* bang = strchr(text, '!');
            uint64_t after = name;
            std::string base;
            if (bang && bang != text)
            {
                base.assign(text, static_cast<size_t>(bang - text));
                after = Red::CNamePool::Add(base.c_str()).hash;
                const uint64_t newValue = after;
                memcpy(reinterpret_cast<void*>(a3 + 0x28), &newValue, 8);
            }
            bool fresh;
            {
                std::lock_guard<std::mutex> lock(s_depotCheckMutex);
                fresh = s_dynNameSeen.insert(name).second;
            }
            if (fresh && s_depotCheckFd >= 0)
            {
                char line[400];
                const int n = snprintf(line, sizeof(line), "N %016llx %s -> %s\n", static_cast<unsigned long long>(name), text, bang && bang != text ? base.c_str() : "(unchanged)");
                std::lock_guard<std::mutex> lock(s_depotCheckMutex);
                write(s_depotCheckFd, line, static_cast<size_t>(n));
            }
        }
    }
    return s_spawnLoadAppearanceOriginal(a0, a1, a2, a3, a4, a5, a6, a7);
}

// `EntityTemplate::FindAppearance` (0x103d79e90: x0 = entity template, x1 = CName; returns the 0x18-byte entry of
// template->appearances or null). The item factory (state 3 of the request machine at 0x100d39bc4, handler 0x100d3a5ec)
// calls it with the item's appearance name; ArchiveXL's dynamic names ("base!variant") never exist in the template, so
// the request stalls. When the exact name is missing, retry with the part before '!'. Each distinct name is logged as
// "F <hash> <name> -> <base> found|missing".
using FindAppearanceFn = uint64_t (*)(uint64_t, uint64_t);
FindAppearanceFn s_findAppearanceOriginal = nullptr;
std::unordered_set<uint64_t> s_findSeen;

// Variant ("brown" in "base_!brown+attr=value%hash") of the item whose appearance was resolved last. The garment mesh
// component asks the mesh for the appearance "*{variant}", which only ArchiveXL's own CMesh hook can expand; until that
// per-entity state is ported, the most recent item variant stands in for it (correct while one dynamic item is being equipped).
std::atomic<uint64_t> s_lastVariant{0};

uint64_t FindAppearanceDetour(uint64_t aTemplate, uint64_t aName)
{
    const uint64_t found = s_findAppearanceOriginal(aTemplate, aName);
    if (found || !aName)
        return found;

    const char* text = Red::CNamePool::Get(Red::CName(aName));
    const char* bang = text ? strchr(text, '!') : nullptr;
    if (!bang || bang == text)
        return found;

    {
        std::string variant(bang + 1);
        variant.resize(variant.find_first_of("+%") == std::string::npos ? variant.size() : variant.find_first_of("+%"));
        if (!variant.empty())
            s_lastVariant.store(Red::CNamePool::Add(variant.c_str()).hash);
    }
    const std::string base(text, static_cast<size_t>(bang - text));
    const uint64_t baseHash = Red::CNamePool::Add(base.c_str()).hash;
    const uint64_t retry = s_findAppearanceOriginal(aTemplate, baseHash);

    bool fresh;
    {
        std::lock_guard<std::mutex> lock(s_depotCheckMutex);
        fresh = s_findSeen.insert(aName).second;
    }
    if (fresh && s_depotCheckFd >= 0)
    {
        char line[400];
        const int n = snprintf(line, sizeof(line), "F %016llx %s -> %s %s\n", static_cast<unsigned long long>(aName), text, base.c_str(), retry ? "found" : "missing");
        std::lock_guard<std::mutex> lock(s_depotCheckMutex);
        write(s_depotCheckFd, line, static_cast<size_t>(n));
    }
    return retry;
}

// ---- Mesh job diagnostics. 0x103bbe49c is the callback that runs after a garment mesh token finishes (x3 = &job.token,
// x4 = &job.component); it hands the resource to the CMesh appearance lookup 0x103b62134 and crashes when the resource is
// null. The detour dumps the token, its first words and the component's appearance name ("M ..." lines in axl_va.log);
// the guard on 0x103b62134 returns an empty slot instead of dereferencing a null mesh. ----
using MeshJobFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
MeshJobFn s_meshJobOriginal = nullptr;
std::atomic<int> s_meshJobCalls{0};

void MeshJobLog(const char* aFmt, ...) __attribute__((format(printf, 1, 2)));
void MeshJobLog(const char* aFmt, ...)
{
    char text[400];
    va_list args;
    va_start(args, aFmt);
    vsnprintf(text, sizeof(text), aFmt, args);
    va_end(args);
    MacLogLine("M", text);
}

uint64_t MeshJobDetour(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    const int call = s_meshJobCalls.fetch_add(1);
    if (call < 20000)
    {
        uint64_t token = 0;
        SafeRead(a3, &token, 8);
        uint64_t component = 0, appearance = 0;
        const char* appearanceText = nullptr;
        if (SafeRead(a4, &component, 8) && component && SafeRead(component + 0x228, &appearance, 8) && appearance)
            appearanceText = Red::CNamePool::Get(Red::CName(appearance));
        const bool dynamic = appearanceText && (strchr(appearanceText, '{') || appearanceText[0] == '*');
        MeshJobLog("meshjob #%d ctx=%#llx index=%u token=%#llx appearance='%s'%s", call, static_cast<unsigned long long>(a0), static_cast<unsigned>(a1),
                   static_cast<unsigned long long>(token), appearanceText ? appearanceText : "?", dynamic ? " DYNAMIC" : "");
        if ((call < 2 || dynamic) && token > 0x100000000ull && token < 0x7fffffffffffull)
        {
            uint64_t w[16] = {};
            SafeRead(token, w, sizeof(w));
            for (int k = 0; k < 16; k += 4)
                MeshJobLog("  token+%#x: %016llx %016llx %016llx %016llx", k * 8, static_cast<unsigned long long>(w[k]), static_cast<unsigned long long>(w[k + 1]),
                           static_cast<unsigned long long>(w[k + 2]), static_cast<unsigned long long>(w[k + 3]));
            for (int k = 0; k < 16; ++k)
            {
                if (w[k] > 0x100000000ull && w[k] < 0x7fffffffffffull && (w[k] & 7) == 0)
                {
                    uint64_t inner[6] = {};
                    if (SafeRead(w[k], inner, sizeof(inner)))
                        MeshJobLog("  *(token+%#x)=%#llx -> %016llx %016llx %016llx %016llx %016llx %016llx", k * 8, static_cast<unsigned long long>(w[k]),
                                   static_cast<unsigned long long>(inner[0]), static_cast<unsigned long long>(inner[1]), static_cast<unsigned long long>(inner[2]),
                                   static_cast<unsigned long long>(inner[3]), static_cast<unsigned long long>(inner[4]), static_cast<unsigned long long>(inner[5]));
                }
            }
        }
    }
    return s_meshJobOriginal(a0, a1, a2, a3, a4, a5, a6, a7);
}

using MeshFindFn = uint64_t (*)(uint64_t, uint64_t);
MeshFindFn s_meshFindOriginal = nullptr;
alignas(16) uint64_t s_emptyMeshSlot[4] = {};

uint64_t MeshFindGuard(uint64_t aMesh, uint64_t aName)
{
    if (!aMesh)
    {
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1) < 8)
            MeshJobLog("CMesh::FindAppearance called with null mesh, name=%016llx", static_cast<unsigned long long>(aName));
        return reinterpret_cast<uint64_t>(&s_emptyMeshSlot[0]);
    }
    static const uint64_t dynamicName = Red::CNamePool::Add("*{variant}").hash;
    if (aName != dynamicName)
        return s_meshFindOriginal(aMesh, aName);

    // "*{variant}": try the item's variant, then "default", then "black"; keep the first the mesh actually has.
    const uint64_t candidates[3] = {s_lastVariant.load(), Red::CNamePool::Add("default").hash, Red::CNamePool::Add("black").hash};
    uint64_t result = 0;
    for (const uint64_t candidate : candidates)
    {
        if (!candidate)
            continue;
        result = s_meshFindOriginal(aMesh, candidate);
        uint64_t handle = 0;
        if (result && SafeRead(result, &handle, 8) && handle)
        {
            static std::atomic<int> reported{0};
            if (reported.fetch_add(1) < 16)
                MeshJobLog("*{variant} -> '%s'", Red::CNamePool::Get(Red::CName(candidate)));
            return result;
        }
    }
    static std::atomic<int> missed{0};
    if (missed.fetch_add(1) < 8)
        MeshJobLog("*{variant} not resolved (variant=%016llx)", static_cast<unsigned long long>(s_lastVariant.load()));
    return result ? result : reinterpret_cast<uint64_t>(&s_emptyMeshSlot[0]);
}

// ---- Call counters: /tmp/axl_count_targets.txt lists file VAs (hex, one per line); each gets a counting trampoline.
// `touch /tmp/axl_counts_reset` zeroes them, `touch /tmp/axl_counts_dump` writes "<va> <hits>" lines to /tmp/axl_counts.txt. ----
uint64_t s_countVa[kCountSlots];
int s_countN = 0;

void AttachCounters(uintptr_t aSlide)
{
    std::ifstream in("/tmp/axl_count_targets.txt");
    std::string token;
    auto& driver = Core::HookingDriver::GetDefault();
    while (s_countN < kCountSlots && (in >> token))
    {
        const uint64_t va = std::strtoull(token.c_str(), nullptr, 16);
        if (!va || va == 0x103d79e90ull)
            continue; // FindAppearanceDetour owns this address
        s_countVa[s_countN] = va;
        driver.HookAttach(aSlide + va, reinterpret_cast<void*>(kCountDetours[s_countN]), &g_countOrig[s_countN]);
        ++s_countN;
    }
    char b[64];
    snprintf(b, sizeof(b), "call counters attached: %d", s_countN);
    MacLogLine("N", b);
}

void DumpCounters()
{
    FILE* out = fopen("/tmp/axl_counts.txt", "w");
    if (!out)
        return;
    for (int i = 0; i < s_countN; ++i)
        if (g_countHits[i])
            fprintf(out, "%llx %llu\n", static_cast<unsigned long long>(s_countVa[i]), static_cast<unsigned long long>(g_countHits[i]));
    fclose(out);
}

void ResetCounters()
{
    for (int i = 0; i < kCountSlots; ++i)
        g_countHits[i] = 0;
}

// ---- On-demand scan of the process' writable memory for the watched hashes: `touch /tmp/axl_scan_now` ----
void MemoryScan()
{
    LoadWatch();
    std::vector<uint8_t> chunk((4u << 20) + 8);
    struct Found { size_t count = 0; std::vector<uint64_t> where; };
    std::vector<Found> found(s_watch.size());
    const uint64_t watchLo = reinterpret_cast<uint64_t>(s_watch.data());
    const uint64_t watchHi = watchLo + s_watch.size() * sizeof(s_watch[0]);
    const uint64_t chunkLo = reinterpret_cast<uint64_t>(chunk.data());
    const uint64_t chunkHi = chunkLo + chunk.size();
    const uint64_t foundLo = reinterpret_cast<uint64_t>(found.data());
    const uint64_t foundHi = foundLo + found.size() * sizeof(Found);
    size_t scannedBytes = 0, regions = 0;
    mach_vm_address_t addr = 0;
    for (;;)
    {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = 0;
        if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS)
            break;
        if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE) && size <= (1ull << 30))
        {
            ++regions;
            for (uint64_t off = 0; off < size; off += (4u << 20))
            {
                const size_t len = static_cast<size_t>(std::min<uint64_t>(4u << 20, size - off));
                if (!SafeRead(addr + off, chunk.data(), len))
                    continue;
                scannedBytes += len;
                for (size_t i = 0; i + 8 <= len; i += 8)
                {
                    uint64_t v;
                    memcpy(&v, chunk.data() + i, 8);
                    if (v == 0)
                        continue;
                    auto it = std::lower_bound(s_watch.begin(), s_watch.end(), std::make_pair(v, std::string()));
                    if (it == s_watch.end() || it->first != v)
                        continue;
                    const uint64_t at = addr + off + i;
                    if ((at >= watchLo && at < watchHi) || (at >= chunkLo && at < chunkHi) || (at >= foundLo && at < foundHi))
                        continue;
                    auto& f = found[static_cast<size_t>(it - s_watch.begin())];
                    ++f.count;
                    if (f.where.size() < 3)
                        f.where.push_back(at);
                }
            }
        }
        addr += size;
    }
    FILE* out = fopen("/tmp/axl_scan.log", "w");
    if (!out)
        return;
    fprintf(out, "scan: %zu regions, %zu MB\n", regions, scannedBytes >> 20);
    size_t any = 0;
    for (size_t i = 0; i < s_watch.size(); ++i)
        if (found[i].count)
        {
            ++any;
            fprintf(out, "%016llx %-60s x%zu", static_cast<unsigned long long>(s_watch[i].first), s_watch[i].second.c_str(), found[i].count);
            for (auto w : found[i].where)
                fprintf(out, " @%#llx", static_cast<unsigned long long>(w));
            fprintf(out, "\n");
        }
    fprintf(out, "watched %zu, present in memory: %zu\n", s_watch.size(), any);
    fclose(out);
}

// ---- Crash log: on a fatal signal write "C sig=.. addr=.. pc=.. lr=.. sp=.." plus the frame-pointer chain (all as file VAs,
// i.e. minus the slide) to /tmp/axl_crash.log, then restore the previous handler and return so the fault re-raises. The game
// may install its own handlers later, so the scan thread re-installs ours whenever it finds another one in place. ----
struct sigaction s_prevAction[32];
bool s_crashInstalled[32];

void CrashHandler(int aSig, siginfo_t* aInfo, void* aCtx)
{
    char buf[1400];
    int off = 0;
    const auto* uc = static_cast<ucontext_t*>(aCtx);
    const uint64_t pc = uc->uc_mcontext->__ss.__pc;
    const uint64_t lr = uc->uc_mcontext->__ss.__lr;
    const uint64_t sp = uc->uc_mcontext->__ss.__sp;
    uint64_t fp = uc->uc_mcontext->__ss.__fp;
    off += snprintf(buf + off, sizeof(buf) - off, "C sig=%d addr=%#llx pc=%#llx lr=%#llx sp=%#llx\nC frames:", aSig,
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(aInfo->si_addr)), static_cast<unsigned long long>(pc - s_slide),
                    static_cast<unsigned long long>(lr - s_slide), static_cast<unsigned long long>(sp));
    for (int i = 0; i < 24 && fp > 0x100000000ull && fp < 0x7fffffffffffull && (fp & 7) == 0 && off < static_cast<int>(sizeof(buf)) - 40; ++i)
    {
        uint64_t frame[2];
        if (!SafeRead(fp, frame, 16))
            break;
        off += snprintf(buf + off, sizeof(buf) - off, " %#llx", static_cast<unsigned long long>(frame[1] - s_slide));
        if (frame[0] <= fp)
            break;
        fp = frame[0];
    }
    off += snprintf(buf + off, sizeof(buf) - off, "\n");
    const int fd = open("/tmp/axl_crash.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0)
    {
        write(fd, buf, static_cast<size_t>(off));
        close(fd);
    }
    static std::atomic<bool> dumped{false};
    if (!dumped.exchange(true))
        DumpCounters(); // /tmp/axl_counts.txt: what ran since the last reset, up to the crash
    sigaction(aSig, &s_prevAction[aSig], nullptr);
}

void EnsureCrashHandlers()
{
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT})
    {
        struct sigaction current;
        if (sigaction(sig, nullptr, &current) != 0)
            continue;
        if (s_crashInstalled[sig] && (current.sa_flags & SA_SIGINFO) && current.sa_sigaction == &CrashHandler)
            continue;
        struct sigaction mine;
        memset(&mine, 0, sizeof(mine));
        mine.sa_sigaction = &CrashHandler;
        mine.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&mine.sa_mask);
        s_prevAction[sig] = current;
        sigaction(sig, &mine, nullptr);
        s_crashInstalled[sig] = true;
    }
}

void StartScanThread()
{
    std::thread([]() {
        for (;;)
        {
            sleep(1);
            EnsureCrashHandlers();
            if (access("/tmp/axl_trace_on", F_OK) == 0)
            {
                unlink("/tmp/axl_trace_on");
                s_traceOn.store(true);
                MacLogLine("N", "backtrace tracing ON");
            }
            if (access("/tmp/axl_trace_off", F_OK) == 0)
            {
                unlink("/tmp/axl_trace_off");
                s_traceOn.store(false);
                MacLogLine("N", "backtrace tracing OFF");
            }
            if (access("/tmp/axl_counts_reset", F_OK) == 0)
            {
                unlink("/tmp/axl_counts_reset");
                ResetCounters();
            }
            if (access("/tmp/axl_counts_dump", F_OK) == 0)
            {
                unlink("/tmp/axl_counts_dump");
                DumpCounters();
            }
            if (access("/tmp/axl_scan_now", F_OK) == 0)
            {
                unlink("/tmp/axl_scan_now");
                MemoryScan();
            }
        }
    }).detach();
}

using DepotCheckFn = bool (*)(void* aDepot, uint64_t aPath);
DepotCheckFn s_depotCheckOriginal = nullptr;

bool DepotCheckDetour(void* aDepot, uint64_t aPath)
{
    aPath = AliasPath(aPath);
    const bool result = s_depotCheckOriginal(aDepot, aPath);
    DepotLog('E', aPath, result ? 1 : 0);
    return result;
}

// Same idea for ResourceDepot::Request (depot vtable slot 3, result returned through x8): logs "R <hash> <ok>" for every
// distinct path the game actually loads through the depot.
using DepotRequestFn = Ret16 (*)(void* aDepot, void* aArg1, uint64_t aPath);
DepotRequestFn s_depotRequestOriginal = nullptr;

Ret16 DepotRequestDetour(void* aDepot, void* aArg1, uint64_t aPath)
{
    Ret16 result = s_depotRequestOriginal(aDepot, aArg1, aPath);
    DepotLog('R', aPath, result.a ? 1 : 0);
    return result;
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
        s_depotCheckFd = open("/tmp/axl_depot.log", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
        driver.HookAttach(slide + 0x10407eb88ull, reinterpret_cast<void*>(&DepotCheckDetour), reinterpret_cast<void**>(&s_depotCheckOriginal));
        driver.HookAttach(slide + 0x10407e880ull, reinterpret_cast<void*>(&DepotRequestDetour), reinterpret_cast<void**>(&s_depotRequestOriginal));
        // Cast the net wide: every loader/token function that could carry an outfit resource path.
        AttachProbe<0>(slide + 0x10426901cull, true, "reqByPath");
        AttachProbe<1>(slide + 0x10426bf00ull, false, "loadReq");
        AttachProbe<2>(slide + 0x10426b200ull, false, "submit");
        AttachProbe<3>(slide + 0x104276af8ull, false, "tokenInit");
        AttachProbe<4>(slide + 0x10427b5fcull, false, "loadAsync");
        driver.HookAttach(slide + 0x103d798b0ull, reinterpret_cast<void*>(&SpawnLoadAppearanceDetour), reinterpret_cast<void**>(&s_spawnLoadAppearanceOriginal));
        driver.HookAttach(slide + 0x103d79e90ull, reinterpret_cast<void*>(&FindAppearanceDetour), reinterpret_cast<void**>(&s_findAppearanceOriginal));
        driver.HookAttach(slide + 0x103bbe49cull, reinterpret_cast<void*>(&MeshJobDetour), reinterpret_cast<void**>(&s_meshJobOriginal));
        driver.HookAttach(slide + 0x103b62134ull, reinterpret_cast<void*>(&MeshFindGuard), reinterpret_cast<void**>(&s_meshFindOriginal));
        AttachCounters(slide);
        StartScanThread();
    }

#define LENIENT(N, ID) if (auto a = resolver.ResolveAddress(ID)) driver.HookAttach(a, reinterpret_cast<void*>(&LenientValidator<N>::Detour), reinterpret_cast<void**>(&LenientValidator<N>::original));
    LENIENT(0, 1296154640u) LENIENT(1, 1296154641u) LENIENT(2, 1296154642u) LENIENT(3, 1296154643u) LENIENT(4, 1296154644u)
#undef LENIENT
}
}
#endif
