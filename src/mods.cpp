#include <sadkmod/mod.hpp>
#include <sadkmod/mods.hpp>

namespace sadk::mods {

// In a mod everything goes through the host's API; in the host straight to host.hpp.
std::vector<host::ModInfo> list()
{
    const sadkmod_api *h = host_api();
    if (!h) return host::mods();
    std::vector<host::ModInfo> out;
    h->list_mods(
        [](const sadkmod_modinfo *i, void *ctx) {
            host::ModInfo m;
            m.folder = i->folder;
            m.name = i->name;
            m.dir = i->dir;
            m.version = i->version;
            m.flags = i->flags;
            m.active = i->active;
            m.pending_free = i->pending_free;
            static_cast<std::vector<host::ModInfo> *>(ctx)->push_back(m);
        },
        &out);
    return out;
}

bool add(const std::string &dir) { return host_api() ? host_api()->add_mod(dir.c_str()) : host::add_mod(dir.c_str()); }
bool forget(const std::string &f) { return host_api() ? host_api()->forget_mod(f.c_str()) : host::forget_mod(f.c_str()); }
bool activate(const std::string &f) { return host_api() ? host_api()->activate_mod(f.c_str()) : host::activate(f.c_str()); }
host::Unload deactivate(const std::string &f)
{
    return host_api() ? static_cast<host::Unload>(host_api()->deactivate_mod(f.c_str())) : host::deactivate(f.c_str());
}
int free_pending() { return host_api() ? host_api()->free_pending() : host::free_pending(); }
std::string content_hash(const std::string &f)
{
    if (!host_api()) return host::content_hash(f.c_str());
    char h[33] = {};
    return host_api()->mod_hash(f.c_str(), h) ? std::string(h) : std::string();
}
void redirect_server_mods(bool on)
{
    if (host_api())
        host_api()->redirect_server_mods(on);
    else
        host::redirect_server_mods(on);
}

}  // namespace sadk::mods
