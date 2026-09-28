#include "NetplayAndroid.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <typeinfo>

#include <android/log.h>
#include <sys/system_properties.h>
#include <enet/enet.h>
#include <zstd.h>

#include "MPInterface.h"
#include "NDSCart.h"
#include "Savestate.h"
#include "SPI.h"
#include "GPU.h"

#define NP_LOG(...) __android_log_print(ANDROID_LOG_INFO, "wmds-netplay", __VA_ARGS__)

using namespace melonDS;

namespace NetplayAndroid
{

namespace
{

constexpr u16 kPort = 8070;
constexpr u32 kMagic = 0x4E504457; // "WDPN"
constexpr u32 kLagFrames = 4;

enum MsgType : u8
{
    Msg_Hello = 1,
    Msg_Package = 2,
    Msg_Input = 3,
};

#pragma pack(push, 1)
struct Hello
{
    u8 Type;
    u32 Magic;
    u32 RomHash;
    u32 Lag;
};

struct InputFrame
{
    u8 Type;
    u32 Frame;
    u32 Keys;
    u8 Touching;
    u16 X;
    u16 Y;
};
#pragma pack(pop)

// The mirror's userdata: only its address matters, Platform callbacks compare
// against it to tell the mirror from the local MelonInstance.
struct MirrorTag { int Id = kMirrorInstanceId; } mirrorTag;

std::atomic<bool> active {false};
std::atomic<bool> running {false};
std::string lastRequest;

ENetHost* host = nullptr;
ENetPeer* peer = nullptr;

NDS* mirror = nullptr;
std::thread mirrorThread;
std::thread netThread;

// local console: its own inputs, applied LagFrames after they are read
std::deque<InputFrame> ownQueue;

// mirror console: the remote player's inputs, from the network thread
std::mutex mirrorLock;
std::condition_variable mirrorCond;
std::deque<InputFrame> mirrorQueue;

// bench: received inputs held back this long before the mirror may use them
u32 simDelayMs = 0;
std::deque<std::pair<u64, InputFrame>> delayed;

std::mutex outLock;
std::deque<InputFrame> outQueue;

u64 NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Property(const char* name)
{
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get(name, value);
    return value;
}

u32 HashBytes(const u8* data, u32 len, u32 step)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < len; i += step)
        h = (h ^ data[i]) * 16777619u;
    return h ^ len;
}

// frame each console started the session at, to hash every frame early on
u32 localStart = 0, mirrorStart = 0;

// Determinism diagnostics, off unless debug.wmds.diag=1 when the session
// starts: the section hashes take a full savestate of both consoles every
// 300 frames, a hitch you can hear.
bool diagnostics = false;

void LogCheck(const char* role, NDS* nds)
{
    if (!diagnostics || !nds->MainRAM)
        return;

    // Every frame for the first 600: the first frame where two sides disagree
    // is the one that says why. Full RAM, plus the CPU cycle counters.
    u32 start = (role[0] == 'm') ? mirrorStart : localStart;
    if (nds->NumFrames - start < 600)
    {
        const u32* ram = (const u32*)nds->MainRAM;
        u32 words = (nds->MainRAMMask + 1) / 4;
        u32 h = 2166136261u;
        for (u32 i = 0; i < words; i++)
            h = (h ^ ram[i]) * 16777619u;
        NP_LOG("[netplay] %s EARLY frame=%u ram=%08X sys=%llu a9=%llu a7=%llu pc9=%08X pc7=%08X",
               role, nds->NumFrames, h, (unsigned long long)nds->GetSysTimestamp(),
               (unsigned long long)nds->ARM9Timestamp, (unsigned long long)nds->ARM7Timestamp,
               nds->ARM9.R[15], nds->ARM7.R[15]);
    }

    if ((nds->NumFrames % 300) != 0)
        return;
    const u32* ram = (const u32*)nds->MainRAM;
    u32 words = (nds->MainRAMMask + 1) / 4;
    u32 hash = 2166136261u;
    for (u32 i = 0; i < words; i += 64)
        hash = (hash ^ ram[i]) * 16777619u;
    NP_LOG("[netplay] %s CHECK frame=%u ramhash=%08X", role, nds->NumFrames, hash);

    // the same, per savestate section: says in which part of the machine two
    // consoles disagree
    Savestate state(Savestate::DEFAULT_SIZE);
    if (state.Error || !nds->DoSavestate(&state)) return;
    const u8* buf = (const u8*)state.Buffer();
    u32 len = state.Length();
    std::string line;
    for (u32 off = 0x10; off + 16 <= len;)
    {
        u32 slen;
        memcpy(&slen, buf + off + 4, 4);
        if (slen < 16 || off + slen > len) break;
        u32 h = 2166136261u;
        for (u32 i = off + 16; i < off + slen; i++)
            h = (h ^ buf[i]) * 16777619u;
        char tmp[24];
        snprintf(tmp, sizeof(tmp), " %.4s=%08X", (const char*)(buf + off), h);
        line += tmp;
        off += slen;
    }
    NP_LOG("[netplay] %s SECT frame=%u%s", role, nds->NumFrames, line.c_str());
}

void ApplyInput(NDS* nds, const InputFrame& f)
{
    nds->SetKeyMask(f.Keys);
    if (f.Touching)
        nds->TouchScreen(f.X, f.Y);
    else
        nds->ReleaseScreen();
}

bool SendReliable(const void* data, size_t len)
{
    ENetPacket* pkt = enet_packet_create(data, len, ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(peer, 0, pkt) < 0)
        return false;
    enet_host_flush(host);
    return true;
}

// Waits for one packet of the given type, servicing the host meanwhile.
bool WaitFor(MsgType type, std::vector<u8>& out, u32 timeoutMs)
{
    u64 start = NowMs();
    while (NowMs() - start < timeoutMs)
    {
        ENetEvent evt;
        if (enet_host_service(host, &evt, 50) <= 0)
            continue;
        if (evt.type == ENET_EVENT_TYPE_DISCONNECT)
            return false;
        if (evt.type != ENET_EVENT_TYPE_RECEIVE)
            continue;
        bool match = evt.packet->dataLength > 0 && evt.packet->data[0] == type;
        if (match)
            out.assign(evt.packet->data, evt.packet->data + evt.packet->dataLength);
        enet_packet_destroy(evt.packet);
        if (match)
            return true;
    }
    return false;
}

u32 RomHash(NDS* nds)
{
    auto* cart = nds->NDSCartSlot.GetCart();
    if (!cart) return 0;
    return HashBytes(cart->GetROM(), cart->GetROMLength(), 4099);
}

void PutBlob(std::vector<u8>& out, const u8* data, u32 len)
{
    out.insert(out.end(), (const u8*)&len, (const u8*)&len + 4);
    out.insert(out.end(), data, data + len);
}

bool GetBlob(const u8*& p, const u8* end, const u8*& data, u32& len)
{
    if (end - p < 4) return false;
    memcpy(&len, p, 4);
    p += 4;
    if ((u32)(end - p) < len) return false;
    data = p;
    p += len;
    return true;
}

// Everything the other device needs to build a mirror of this console that is
// the same machine in the same state: the savestate does not carry the
// firmware (MAC address, player name), the BIOS, or the ROM. The ROM is not
// sent, both players have it; its hash was compared in the hello.
std::vector<u8> BuildPackage(NDS* nds, Savestate& state)
{

    std::vector<u8> raw;
    const Firmware& fw = nds->SPI.GetFirmware();
    PutBlob(raw, fw.Buffer(), fw.Length());
    PutBlob(raw, nds->GetARM9BIOS().data(), (u32)nds->GetARM9BIOS().size());
    PutBlob(raw, nds->GetARM7BIOS().data(), (u32)nds->GetARM7BIOS().size());
    PutBlob(raw, nds->GetNDSSave() ? nds->GetNDSSave() : (const u8*)"", nds->GetNDSSave() ? nds->GetNDSSaveLength() : 0);
    PutBlob(raw, (const u8*)state.Buffer(), state.Length());

    std::vector<u8> pkt(1 + 4 + ZSTD_compressBound(raw.size()));
    pkt[0] = Msg_Package;
    u32 rawLen = (u32)raw.size();
    memcpy(&pkt[1], &rawLen, 4);
    size_t clen = ZSTD_compress(&pkt[5], pkt.size() - 5, raw.data(), raw.size(), 3);
    if (ZSTD_isError(clen)) return {};
    pkt.resize(5 + clen);
    NP_LOG("[netplay] package: %u bytes, %zu compressed, frame %u", rawLen, clen, nds->NumFrames);
    return pkt;
}

NDS* BuildMirror(const std::vector<u8>& pkt, NDS* local, const ArgsFactory& mirrorArgs)
{
    if (pkt.size() < 5) return nullptr;
    u32 rawLen;
    memcpy(&rawLen, &pkt[1], 4);
    std::vector<u8> raw(rawLen);
    size_t got = ZSTD_decompress(raw.data(), rawLen, &pkt[5], pkt.size() - 5);
    if (ZSTD_isError(got) || got != rawLen) return nullptr;

    const u8* p = raw.data();
    const u8* end = p + raw.size();
    const u8 *fw, *bios9, *bios7, *save, *state;
    u32 fwLen, bios9Len, bios7Len, saveLen, stateLen;
    if (!GetBlob(p, end, fw, fwLen) || !GetBlob(p, end, bios9, bios9Len) || !GetBlob(p, end, bios7, bios7Len)
        || !GetBlob(p, end, save, saveLen) || !GetBlob(p, end, state, stateLen))
        return nullptr;
    if (bios9Len != ARM9BIOSSize || bios7Len != ARM7BIOSSize) return nullptr;

    auto args = mirrorArgs();
    if (!args) return nullptr;
    auto a9 = std::make_unique<ARM9BIOSImage>();
    memcpy(a9->data(), bios9, bios9Len);
    auto a7 = std::make_unique<ARM7BIOSImage>();
    memcpy(a7->data(), bios7, bios7Len);
    args->ARM9BIOS = std::move(a9);
    args->ARM7BIOS = std::move(a7);
    args->Firmware = Firmware(fw, fwLen);

    NDS* nds = new NDS(std::move(*args), &mirrorTag);
    nds->Reset();

    auto* localCart = local->NDSCartSlot.GetCart();
    if (!localCart) { delete nds; return nullptr; }
    NDSCart::NDSCartArgs cartArgs;
    if (saveLen)
    {
        cartArgs.SRAM = std::make_unique<u8[]>(saveLen);
        memcpy(cartArgs.SRAM.get(), save, saveLen);
        cartArgs.SRAMLength = saveLen;
    }
    auto cart = NDSCart::ParseROM(localCart->GetROM(), localCart->GetROMLength(), &mirrorTag, std::move(cartArgs));
    if (!cart) { delete nds; return nullptr; }
    nds->SetNDSCart(std::move(cart));
    if (saveLen)
        nds->SetNDSSave(save, saveLen);

    Savestate st((void*)state, stateLen, false);
    if (st.Error || !nds->DoSavestate(&st)) { delete nds; return nullptr; }

    // only RunningGame travels in a savestate, not Running
    nds->Start();
    return nds;
}

void MirrorLoop()
{
    NP_LOG("[netplay] mirror running from frame %u", mirror->NumFrames);
    mirror->JIT.ResetBlockCache();
    while (running.load())
    {
        InputFrame f;
        {
            std::unique_lock<std::mutex> lk(mirrorLock);
            mirrorCond.wait_for(lk, std::chrono::milliseconds(50), [] { return !mirrorQueue.empty() || !running.load(); });
            if (mirrorQueue.empty()) continue;
            f = mirrorQueue.front();
            mirrorQueue.pop_front();
        }
        if (f.Frame < mirror->NumFrames) continue;
        if (f.Frame > mirror->NumFrames)
            NP_LOG("[netplay] mirror: input for frame %u while at frame %u", f.Frame, mirror->NumFrames);

        ApplyInput(mirror, f);
        mirror->RunFrame();
        MPInterface::Get().SetNow(kMirrorInstanceId, mirror->GetSysTimestamp());
        LogCheck("mirror", mirror);
    }
    MPInterface::Get().Leave(kMirrorInstanceId);
}

void NetLoop()
{
    while (running.load())
    {
        {
            std::lock_guard<std::mutex> lk(outLock);
            bool any = false;
            while (!outQueue.empty())
            {
                InputFrame f = outQueue.front();
                outQueue.pop_front();
                ENetPacket* pkt = enet_packet_create(&f, sizeof(f), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(peer, 0, pkt);
                any = true;
            }
            if (any) enet_host_flush(host);
        }

        ENetEvent evt;
        while (enet_host_service(host, &evt, 1) > 0)
        {
            if (evt.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                NP_LOG("[netplay] the other player left");
                running = false;
                mirrorCond.notify_all();
                break;
            }
            if (evt.type == ENET_EVENT_TYPE_RECEIVE)
            {
                if (evt.packet->dataLength == sizeof(InputFrame) && evt.packet->data[0] == Msg_Input)
                {
                    InputFrame f;
                    memcpy(&f, evt.packet->data, sizeof(f));
                    if (simDelayMs)
                        delayed.emplace_back(NowMs() + simDelayMs, f);
                    else
                    {
                        std::lock_guard<std::mutex> lk(mirrorLock);
                        mirrorQueue.push_back(f);
                        mirrorCond.notify_all();
                    }
                }
                enet_packet_destroy(evt.packet);
            }
        }

        if (!delayed.empty())
        {
            u64 now = NowMs();
            std::lock_guard<std::mutex> lk(mirrorLock);
            while (!delayed.empty() && delayed.front().first <= now)
            {
                mirrorQueue.push_back(delayed.front().second);
                delayed.pop_front();
            }
            mirrorCond.notify_all();
        }
    }
}

void Teardown()
{
    if (peer) enet_peer_disconnect_now(peer, 0);
    peer = nullptr;
    if (host) enet_host_destroy(host);
    host = nullptr;
}

// Waiting for the other player never blocks the game: the connection is pumped
// once per frame until it comes up, and only the exchange that follows, a few
// seconds, holds the frame.
bool connecting = false;
bool connectAsHost = false;
std::string joinAddress;
u64 connectStart = 0, lastDial = 0;
constexpr u64 kConnectTimeoutMs = 180000;
constexpr u64 kRedialMs = 3000;

void Dial()
{
    ENetAddress addr {};
    enet_address_set_host(&addr, joinAddress.c_str());
    addr.port = kPort;
    if (peer) enet_peer_reset(peer);
    peer = nullptr;
    enet_host_connect(host, &addr, 1, 0);
    lastDial = NowMs();
}

bool StartConnect(const std::string& request)
{
    static bool enetReady = false;
    if (!enetReady) { enet_initialize(); enetReady = true; }
    Teardown();

    connectAsHost = request == "host";
    if (connectAsHost)
    {
        ENetAddress addr {ENET_HOST_ANY, kPort};
        host = enet_host_create(&addr, 4, 1, 0, 0);
        if (!host) { NP_LOG("[netplay] could not listen on %u", kPort); return false; }
        NP_LOG("[netplay] hosting on port %u, waiting for the other player", kPort);
    }
    else
    {
        joinAddress = request.substr(5);
        host = enet_host_create(nullptr, 1, 1, 0, 0);
        if (!host) return false;
        NP_LOG("[netplay] joining %s", joinAddress.c_str());
        Dial();
    }
    connecting = true;
    connectStart = NowMs();
    return true;
}

// true once the other player is connected
bool PumpConnect()
{
    ENetEvent evt;
    while (enet_host_service(host, &evt, 0) > 0)
    {
        if (evt.type == ENET_EVENT_TYPE_CONNECT) { peer = evt.peer; return true; }
        if (evt.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(evt.packet);
    }
    u64 now = NowMs();
    if (now - connectStart > kConnectTimeoutMs)
    {
        NP_LOG("[netplay] the other player never came");
        connecting = false;
        Teardown();
        return false;
    }
    // the host may not be listening yet: dial again until it is
    if (!connectAsHost && now - lastDial > kRedialMs)
        Dial();
    return false;
}

bool Exchange(NDS* local, int localId, const ArgsFactory& mirrorArgs)
{
    connecting = false;
    enet_peer_timeout(peer, 0, 10000, 30000);

    Hello hello {Msg_Hello, kMagic, RomHash(local), kLagFrames};
    SendReliable(&hello, sizeof(hello));
    std::vector<u8> in;
    if (!WaitFor(Msg_Hello, in, 15000) || in.size() != sizeof(Hello))
    { NP_LOG("[netplay] no hello"); Teardown(); return false; }
    Hello theirs;
    memcpy(&theirs, in.data(), sizeof(theirs));
    if (theirs.Magic != kMagic || theirs.RomHash != hello.RomHash || theirs.Lag != kLagFrames)
    { NP_LOG("[netplay] not the same game (%08X vs %08X)", theirs.RomHash, hello.RomHash); Teardown(); return false; }

    Savestate ownState(Savestate::DEFAULT_SIZE);
    if (ownState.Error || !local->DoSavestate(&ownState))
    { NP_LOG("[netplay] could not save our console"); Teardown(); return false; }
    std::vector<u8> pkg = BuildPackage(local, ownState);
    if (pkg.empty() || !SendReliable(pkg.data(), pkg.size()))
    { NP_LOG("[netplay] could not send our console"); Teardown(); return false; }
    if (!WaitFor(Msg_Package, in, 120000))
    { NP_LOG("[netplay] never got their console"); Teardown(); return false; }

    mirror = BuildMirror(in, local, mirrorArgs);
    if (!mirror) { NP_LOG("[netplay] could not build the mirror"); Teardown(); return false; }

    // The local console loads back the very state it sent, as the mirror of it
    // does on the other device. Loading recomputes things a savestate does not
    // carry (memory timings, code regions, JIT mappings); a console that just
    // kept running still had its own, and ran its first frame 8 to 18 cycles
    // apart from its mirror. Measured: every frame off from the first one.
    {
        Savestate reload(ownState.Buffer(), ownState.Length(), false);
        if (reload.Error || !local->DoSavestate(&reload))
            NP_LOG("[netplay] WARNING could not reload our own state, expect a desync");
    }

    // Both JIT block caches start empty, on both devices. Where blocks begin
    // decides where the JIT checks for interrupts, so a console with a warm
    // cache and a mirror with a cold one run the same state on different
    // timing. Measured: every checkpoint off, CPU registers and cycle counts
    // first, as soon as the JIT was on.
    local->JIT.ResetBlockCache();
    localStart = local->NumFrames;
    mirrorStart = mirror->NumFrames;

    // The air shared by the two consoles starts here, on both devices alike.
    MPInterface::Set(MPInterface_Deterministic);
    MPInterface::Get().ResetSession(localId, local->GetSysTimestamp());
    MPInterface::Get().ResetSession(kMirrorInstanceId, mirror->GetSysTimestamp());

    // The first LagFrames frames of each console have no input from anybody:
    // neutral on both devices.
    ownQueue.clear();
    for (u32 i = 0; i < kLagFrames; i++)
        ownQueue.push_back({Msg_Input, local->NumFrames + i, 0xFFF, 0, 0, 0});
    {
        std::lock_guard<std::mutex> lk(mirrorLock);
        mirrorQueue.clear();
        for (u32 i = 0; i < kLagFrames; i++)
            mirrorQueue.push_back({Msg_Input, mirror->NumFrames + i, 0xFFF, 0, 0, 0});
    }

    diagnostics = Property("debug.wmds.diag") == "1";
    std::string d = Property("debug.wmds.delay");
    simDelayMs = d.empty() ? 0 : (u32)atoi(d.c_str());

    running = true;
    netThread = std::thread(NetLoop);
    mirrorThread = std::thread(MirrorLoop);
    active = true;
    NP_LOG("[netplay] local: renderer %s, jit %d | mirror: renderer %s, jit %d",
           typeid(local->GPU.GetRenderer3D()).name(), local->IsJITEnabled() ? 1 : 0,
           typeid(mirror->GPU.GetRenderer3D()).name(), mirror->IsJITEnabled() ? 1 : 0);
    NP_LOG("[netplay] session running, local frame %u, mirror frame %u, delay %u ms",
           local->NumFrames, mirror->NumFrames, simDelayMs);
    return true;
}

}

std::mutex requestLock;
std::string pendingRequest;

void Request(const std::string& request)
{
    std::lock_guard<std::mutex> lk(requestLock);
    pendingRequest = request;
}

bool Poll(NDS* local, int localId, const ArgsFactory& mirrorArgs)
{
    if (active.load())
    {
        if (!running.load()) Stop();
        return active.load();
    }

    if (connecting)
    {
        if (PumpConnect())
            return Exchange(local, localId, mirrorArgs);
        return false;
    }

    std::string request;
    {
        std::lock_guard<std::mutex> lk(requestLock);
        request.swap(pendingRequest);
    }

    if (request.empty())
    {
        // bench: the system property, checked once a second
        static u32 tick = 0;
        if ((tick++ % 60) != 0) return false;
        std::string prop = Property("debug.wmds.netplay");
        // A request already set when the app starts is a leftover from a past
        // session, not a new one: acting on it held the game on a black screen
        // for up to two minutes, waiting for a player who was not coming.
        static bool seenFirst = false;
        if (!seenFirst) { seenFirst = true; lastRequest = prop; return false; }
        if (prop.empty() || prop == lastRequest) return false;
        lastRequest = prop;
        request = prop;
    }

    if (request != "host" && request.rfind("join:", 0) != 0) return false;
    StartConnect(request);
    return false;
}

bool Active()
{
    return active.load();
}

void BeforeLocalFrame(NDS* local, u32 liveKeys, bool liveTouching, u16 liveX, u16 liveY)
{
    if (!active.load()) return;

    u32 n = local->NumFrames;
    InputFrame live {Msg_Input, n + kLagFrames, liveKeys & 0xFFF, (u8)(liveTouching ? 1 : 0), liveX, liveY};
    ownQueue.push_back(live);
    {
        std::lock_guard<std::mutex> lk(outLock);
        outQueue.push_back(live);
    }

    while (!ownQueue.empty() && ownQueue.front().Frame < n)
        ownQueue.pop_front();
    if (!ownQueue.empty() && ownQueue.front().Frame == n)
    {
        ApplyInput(local, ownQueue.front());
        ownQueue.pop_front();
    }
}

void AfterLocalFrame(NDS* local, int localId)
{
    if (!active.load()) return;
    MPInterface::Get().SetNow(localId, local->GetSysTimestamp());
    LogCheck("local ", local);
}

bool IsMirror(void* userdata)
{
    return userdata == &mirrorTag;
}

NDS* MirrorNDS()
{
    return mirror;
}

void Stop()
{
    running = false;
    mirrorCond.notify_all();
    if (mirrorThread.joinable()) mirrorThread.join();
    if (netThread.joinable()) netThread.join();
    Teardown();
    delete mirror;
    mirror = nullptr;
    active = false;
    NP_LOG("[netplay] session over");
}

}
