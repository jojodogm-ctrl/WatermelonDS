#ifndef NETPLAYANDROID_H
#define NETPLAYANDROID_H

#include <functional>
#include <memory>
#include <string>

#include "NDS.h"
#include "Args.h"

// Two-player DS netplay, in the shape melonDS's own netplay takes: every
// device emulates both consoles. The local console is the one the player
// sees; the other is a mirror of the remote player's console, fed with the
// inputs that player sends, running on its own thread, never shown, never
// heard. Local wireless between the two goes through DetMP, which resolves
// it on emulated time, so both devices compute exactly the same thing and
// only inputs have to cross the network.
namespace NetplayAndroid
{

// Up to four players: the host is player 0, the guests 1 to 3.
constexpr int kMaxPlayers = 4;

// Only picks the MAC address of the arguments a mirror is built from, which
// the other player's firmware then replaces.
constexpr int kMirrorInstanceId = 1;

using ArgsFactory = std::function<std::unique_ptr<melonDS::NDSArgs>()>;

// Called by the emulation loop before every frame of the local console. Looks
// for a session request (system property debug.wmds.netplay = "host" or
// "join:<address>") and, when one comes, runs the whole handshake right here,
// between two frames. Returns true once a session is running.
bool Poll(melonDS::NDS* local, int localId, const ArgsFactory& mirrorArgs);

bool Active();

// What happened to the session, for the player to see: a refusal used to leave
// the game running alone, with nothing on screen. Values shared with
// MelonEmulator.takeNetplayEvent.
enum Event
{
    Event_None = 0,
    Event_Started = 1,
    Event_Rejected = 2,        // guest: the host has another ROM file, another Edition, or is full
    Event_NeverGotIn = 3,      // guest: no answer from the host
    Event_NobodyCame = 4,      // host: nobody joined
    Event_TurnedAwayGame = 5,  // host: a player came with another ROM file or Edition
    Event_TurnedAwayFull = 6,  // host: a player came when the session was full
    Event_PlayerLeft = 7,
    Event_ExchangeFailed = 8,  // connected, but the consoles never all arrived
};

// The last event, cleared by the call.
int TakeEvent();

// A session asked for by the launching app: "host" or "join:<address>". Taken
// up by the next Poll, whatever the system property says.
void Request(const std::string& request);

// Around the local console's frame: applies the input queued for this frame
// (the live input goes out for a later one), then reports the emulated time.
void BeforeLocalFrame(melonDS::NDS* local, melonDS::u32 liveKeys, bool liveTouching, melonDS::u16 liveX, melonDS::u16 liveY);
void AfterLocalFrame(melonDS::NDS* local, int localId);

// Platform callbacks receive the mirrors' userdata too: these tell them apart.
bool IsMirror(void* userdata);
melonDS::NDS* MirrorNDS(void* userdata);
int MirrorId(void* userdata);

// The number the multiplayer interface knows the local console by: its player
// number during a session, the same on every device, else its own instance ID.
int LocalMpId(int instanceId);

void Stop();

}

#endif
