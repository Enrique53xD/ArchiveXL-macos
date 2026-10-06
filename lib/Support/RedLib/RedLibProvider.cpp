#include "RedLibProvider.hpp"
#include "Red/TypeInfo/Registrar.hpp"
#ifdef __APPLE__
#include <dlfcn.h>
#include <filesystem>
#endif

#ifdef __APPLE__
static int s_moduleAnchor = 0;
#endif

void Support::RedLibProvider::OnBootstrap()
{
#ifdef __APPLE__
    // macOS port (experimental): plugin RTTI registration is opt-in via <plugin dir>/rtti_experiment.
    Dl_info info{};
    if (!dladdr(&s_moduleAnchor, &info) || !info.dli_fname)
        return;
    std::filesystem::path flag = std::filesystem::path(info.dli_fname).parent_path() / "rtti_experiment";
    if (!flag.is_absolute() || !std::filesystem::exists(flag))
        return;
#endif
    Red::TypeInfoRegistrar::RegisterDiscovered();
}
