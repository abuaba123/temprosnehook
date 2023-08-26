/*
  Created by Jenny White on 29.04.18.
  Copyright (c) 2018 nullworks. All rights reserved.
*/

#include <hacks/hacklist.hpp>
#include <settings/Bool.hpp>
#include "HookedMethods.hpp"
#include "MiscTemporary.hpp"
#include "votelogger.hpp"

settings::Boolean random_name{ "misc.random-name", "false" };
extern std::string name_forced;

namespace hooked_methods
{

static TextFile randomnames_file;

DEFINE_HOOKED_METHOD(Shutdown, void, INetChannel *this_, const char *reason)
{
    g_Settings.bInvalid = true;
    logging::Info("Disconnect: %s", reason);
#if ENABLE_IPC
    ipc::UpdateServerAddress(true);
#endif
    ignoredc = false;
    hacks::autojoin::OnShutdown();
    std::string message = reason;
    votelogger::onShutdown(message);
}

} // namespace hooked_methods