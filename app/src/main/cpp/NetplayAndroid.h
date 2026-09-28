#ifndef NETPLAYANDROID_H
#define NETPLAYANDROID_H

#include <functional>
#include <memory>

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

// The instance ID of the mirror console, next to the local one (0).
constexpr int kMirrorInstanceId = 1;

using ArgsFactory = std::function<std::unique_ptr<melonDS::NDSArgs>()>;

// Called by the emulation loop before every frame of the local console. Looks
// for a session request (system property debug.wmds.netplay = "host" or
// "join:<address>") and, when one comes, runs the whole handshake right here,
// between two frames. Returns true once a session is running.
bool Poll(melonDS::NDS* local, int localId, const ArgsFactory& mirrorArgs);

bool Active();

// Around the local console's frame: applies the input queued for this frame
// (the live input goes out for a later one), then reports the emulated time.
void BeforeLocalFrame(melonDS::NDS* local, melonDS::u32 liveKeys, bool liveTouching, melonDS::u16 liveX, melonDS::u16 liveY);
void AfterLocalFrame(melonDS::NDS* local, int localId);

// Platform callbacks receive the mirror's userdata too: these tell it apart.
bool IsMirror(void* userdata);
melonDS::NDS* MirrorNDS();

void Stop();

}

#endif
