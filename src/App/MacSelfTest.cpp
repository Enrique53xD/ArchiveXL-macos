#ifdef __APPLE__
// Development-only diagnostics for the macOS port: verifies at runtime that the located game functions and the
// SDK struct layouts behave as expected. Results go to /tmp/axl_selftest.log.
#include <cstdio>
#include <thread>
#include <unistd.h>

namespace App
{
void StartMacSelfTest()
{
    std::thread([]() {
        sleep(35);
        FILE* f = fopen("/tmp/axl_selftest.log", "w");
        if (!f) return;
        auto log = [&](const char* aFmt, auto... aArgs) { fprintf(f, aFmt, aArgs...); fflush(f); };

        const auto name = RED4ext::CNamePool::Add("IScriptable");
        log("CNamePool::Add(\"IScriptable\") = %#llx (expected 0x2c494a1da412f26b) %s\n", static_cast<unsigned long long>(name.hash),
            name.hash == 0x2c494a1da412f26bULL ? "OK" : "MISMATCH");
        log("CNamePool::Get(hash) = '%s'\n", RED4ext::CNamePool::Get(name));

        auto* rtti = RED4ext::CRTTISystem::Get();
        log("CRTTISystem::Get() = %p\n", static_cast<void*>(rtti));
        if (rtti)
        {
            for (const char* cls : {"IScriptable", "ISerializable", "entEntity", "gameObject", "CMesh", "ResourceDepot"})
            {
                auto* c = rtti->GetClass(RED4ext::CName(cls));
                log("GetClass(%s) = %p %s\n", cls, static_cast<void*>(c), c ? RED4ext::CNamePool::Get(c->name) : "");
            }
            log("GetType(Int32) = %p\n", static_cast<void*>(rtti->GetType(RED4ext::CName("Int32"))));
        }
        log("selftest done\n");
        fclose(f);
    }).detach();
}
} // namespace App
#endif
