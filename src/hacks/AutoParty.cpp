/*
 * AutoParty.cpp
 *
 *  Created on: Nov 27, 2020
 *      Author: delimeats-ch
 */

#include "common.hpp"
#include "hack.hpp"
#include "ipc.hpp"

namespace hacks::autoparty
{
// Enable auto-party?
static settings::Boolean enabled{ "autoparty.enable", "false" };
// Max number of party members before locking the party (and kicking members if there are too many)
static settings::Int max_size{ "autoparty.max-party-size", "6" };
// Comma-separated list of Steam32 IDs that should accept party requests
static settings::String host_list{ "autoparty.party-hosts", "" };
// Whether to automatically kick raged players from the party
static settings::Boolean kick_rage{ "autoparty.kick-rage", "true" };
// Automatically leave the party when a member goes offline
static settings::Boolean auto_leave{ "autoparty.auto-leave", "false" };
// Automatically lock the party when max party size is reached
static settings::Boolean auto_lock{ "autoparty.auto-lock", "true" };
// Automatically unlock the party when max party size has not been reached
static settings::Boolean auto_unlock{ "autoparty.auto-unlock", "true" };
// Actions like leaving the party or kicking members
static settings::Boolean autoparty_log{ "autoparty.log", "true" };
// Log kick notifications to party chat
static settings::Boolean message_kicks{ "autoparty.message-kicks", "true" };
// Extra debugging information like locking/unlocking the party
static settings::Boolean autoparty_debug{ "autoparty.debug", "false" };
// Use the longest-running IPC members as party hosts
static settings::Boolean ipc_mode{ "autoparty.ipc-mode", "false" };
// How many IPC members to use as party hosts
static settings::Int ipc_count{ "autoparty.ipc-count", "0" };
// How often to run the autoparty routine, in seconds
static settings::Int timeout{ "autoparty.run-frequency", "60" };
// Only run the autoparty routine once every N seconds
static Timer routine_timer{};
// Populated by the routine when empty and by configuration changes
static std::vector<uint32> party_hosts = {};

// ha ha macros go brr
#define log(...)        \
    if (*autoparty_log) \
    logging::Info("AutoParty: " __VA_ARGS__)

#define log_debug(...)    \
    if (*autoparty_debug) \
    logging::Info("AutoParty (debug): " __VA_ARGS__)

// I can't think of a better way to do this right now
struct ipc_peer
{
    uint32 friendid;
    time_t ts_injected;
};

bool compare_ts(ipc_peer &a, ipc_peer &b)
{
    return a.ts_injected > b.ts_injected;
}

// Re-populates party_hosts from the current or new configuration
void repopulate(const std::string &str)
{
    // Empty previous values
    party_hosts.clear();

    // IPC autoparty mode
    if (*ipc_mode)
    {
        if (!ipc::peer)
        {
            // uh oh
            return;
        }
        auto &peer_data                           = ipc::peer->memory->peer_user_data;
        std::vector<struct ipc_peer> sorted_peers = {};
        for (auto &data : peer_data)
        {
            struct ipc_peer peer = { .friendid = data.friendid, .ts_injected = data.ts_injected };
            sorted_peers.push_back(peer);
        }

        std::sort(sorted_peers.begin(), sorted_peers.end(), compare_ts);
        for (int i = 0; i < *ipc_count; ++i)
            party_hosts.push_back(sorted_peers[i].friendid);

        return;
    }

    // Add Steam32 IDs to party_hosts
    std::stringstream ss(str);
    for (uint32 id; ss >> id;)
    {
        party_hosts.push_back(id);
        if (ss.peek() == ',' or ss.peek() == ' ')
            ss.ignore();
    }
}

// Is this bot a designated party host?
bool is_host()
{
    uint32 id = g_ISteamUser->GetSteamID().GetAccountID();
    if (std::ranges::any_of(party_hosts, [id](unsigned party_host) { return party_host == id; }))
        return true;

    return false;
}

// Tries to join every party host
void find_party()
{
    log_debug("No party members and not a party host; requesting to join with each party host");
    for (unsigned party_host : party_hosts)
        hack::ExecuteCommand("tf_party_request_join_user " + std::to_string(party_host));
}

// Locks the party, prevents more members from joining
void lock_party()
{
    // "Friends must be invited"
    hack::ExecuteCommand("tf_party_join_request_mode 2");
}

// Unlocks the party, yeah?
void unlock_party()
{
    // "Friends can freely join"
    hack::ExecuteCommand("tf_party_join_request_mode 0");
}

// Leaves the party, called when a member is offline
// If we were the party leader, also unlock the party
void leave_party(re::CTFPartyClient *client, bool was_leader)
{
    log("Leaving the party because %d/%d members are offline", client->GetNumMembers() - client->GetNumOnlineMembers(), client->GetNumMembers());
    hack::ExecuteCommand("tf_party_leave");
    if (was_leader && *auto_unlock)
        unlock_party();
}

// Automatically join/leave parties and kick bad members
void party_routine()
{
    // Is this thing on?
    if (!*enabled)
        return;

    // Only run every N seconds
    if (!routine_timer.test_and_set(*timeout * 1000))
        return;

    // Ignore bad settings
    if (*max_size < 1 or *max_size > 6)
    {
        log("Can't have %d members, max-party-size has been reset to 6", *max_size);
        max_size = 6;
    }

    // Populate party_hosts from the current configuration
    if (party_hosts.empty())
        repopulate(*host_list);

    re::CTFPartyClient *client = re::CTFPartyClient::GTFPartyClient();
    if (client)
    {
        int members = client->GetNumMembers();
        // Are we in a party?
        if (members == 1)
        {
            // We're the only player in this party, so not really
            // If we're a host, allow access, otherwise find a party to join
            if (is_host())
            {
                // We are a party host but have no members; allow access to the party
                if (*auto_unlock)
                {
                    log_debug("No members; unlocking the party");
                    unlock_party();
                }
            }
            else
            {
                find_party();
            }
        }
        else
        {
            // We are in a party!
            // Are we the *designated* leader of the current party?
            if (is_host())
            {
                // And are we actually the leader?
                // Get a list of party members, then check each one to determine the leader
                std::vector<unsigned> members = client->GetPartySteamIDs();
                uint32 leader_id;
                CSteamID id;
                client->GetCurrentPartyLeader(id);
                leader_id = id.GetAccountID();
                if (leader_id == g_ISteamUser->GetSteamID().GetAccountID())
                {
                    // Great, let's manage it
                    // If a member is offline, just leave the party and allow new join requests
                    if (*auto_leave && client->GetNumMembers() > client->GetNumOnlineMembers())
                    {
                        leave_party(client, true);
                        return;
                    }

                    // If enabled, check for any raged players who may have joined our party and kick them
                    // If there are any, return, so we don't kick other members in the event we're also over the set size limit
                    if (*kick_rage)
                    {
                        bool should_ret = false;
                        for (unsigned member : members)
                        {
                            auto &pl = playerlist::AccessData(member);
                            if (pl.state == playerlist::k_EState::RAGE)
                            {
                                std::string message = "Kicking Steam32 ID " + std::to_string(member) + " from the party because they are set to RAGE";
                                logging::Info("AutoParty: %s", message.c_str());
                                if (*message_kicks)
                                    client->SendPartyChat(message.c_str());
                                CSteamID id = CSteamID(member, EUniverse::k_EUniversePublic, EAccountType::k_EAccountTypeIndividual);
                                client->KickPlayer(id);
                                should_ret = true;
                            }
                        }
                        if (should_ret)
                            return;
                    }

                    // If we are at or over the specified limit, lock the party so we auto-reject future join requests
                    if (*auto_lock && members.size() >= *max_size)
                    {
                        log_debug("Locking the party because we have %d out of %d allowed members", members.size(), *max_size);
                        lock_party();
                    }

                    // Kick extra members from the party
                    if (members.size() > *max_size)
                    {
                        int num_to_kick     = members.size() - *max_size;
                        std::string message = "Kicking " + std::to_string(num_to_kick) + " party members because there are " + std::to_string(members.size()) + " out of " + std::to_string(*max_size) + " allowed members";
                        logging::Info("AutoParty: %s", message.c_str());
                        if (*message_kicks)
                            client->SendPartyChat(message.c_str());
                        for (int i = 0; i < num_to_kick; ++i)
                        {
                            CSteamID id = CSteamID(members[members.size() - (1 + i)], EUniverse::k_EUniversePublic, EAccountType::k_EAccountTypeIndividual);
                            client->KickPlayer(id);
                        }
                    }

                    // Unlock the party if it's not full
                    if (*auto_unlock && members.size() < *max_size)
                        unlock_party();
                }
                else
                {
                    // We are in someone else's party as a leader!
                    // Leave the party and unlock our join request mode
                    hack::ExecuteCommand("tf_party_leave");
                    if (*auto_unlock)
                        unlock_party();
                }
            }
            else
            {
                // In a party, but not the leader
                if (*auto_lock)
                {
                    log_debug("Locking our party join mode because we are not the leader of the current party");
                    lock_party();
                }

                // If a member is offline, leave the party
                if (*auto_leave && client->GetNumMembers() > client->GetNumOnlineMembers())
                    leave_party(client, false);
            }
        }
    }
}

static InitRoutine init(
    []()
    {
        host_list.installChangeCallback([](settings::VariableBase<std::string> &var, const std::string &after) { repopulate(after); });
        ipc_mode.installChangeCallback([](settings::VariableBase<bool> &var, bool after) { party_hosts.clear(); });
        EC::Register(EC::Paint, party_routine, "paint_autoparty", EC::average);
    });
} // namespace hacks::autoparty
