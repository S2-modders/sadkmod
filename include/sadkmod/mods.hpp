// The host's mods (host.hpp), the same in a mod (through sadkmod_api) and in the host: for a mod that manages others,
// such as one that downloads a match's server mods.
#pragma once
#include <string>
#include <vector>

#include "host.hpp"

namespace sadk::mods {

std::vector<host::ModInfo> list();                 // every mod the host knows, in load order
bool add(const std::string &dir);                  // a folder outside the scan (<game>\mods\.temp_x): listed, inactive
bool forget(const std::string &folder);            // an inactive mod leaves the list
bool activate(const std::string &folder);
host::Unload deactivate(const std::string &folder);
int free_pending();                                // retries freeing unloaded mod.dlls; how many still wait
std::string content_hash(const std::string &folder);
void redirect_server_mods(bool on);                // off on this thread: server mods' files are not redirected

}  // namespace sadk::mods
