#include "NetplayAndroid.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
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

constexpr u16 kDefaultPort = 8070;
u16 sessionPort = kDefaultPort; // set per request: "host:<n>:<port>" or "join:<address>:<port>"
constexpr u32 kMagic = 0x4E50445A; // v4: checks carry per-block memory hashes
constexpr u32 kLagFrames = 4;

enum MsgType : u8
{
    Msg_Hello = 1,    // guest -> host: rom hash, protocol
    Msg_Assign = 2,   // host -> guest: your player number
    Msg_Reject = 3,   // host -> guest: not the same game, or too late
    Msg_Start = 4,    // host -> all: the session starts with this many players
    Msg_Package = 5,  // anyone -> host -> others: a console to mirror
    Msg_Input = 6,    // anyone -> host -> others: one frame of one player's input
    Msg_Check = 7,    // anyone -> host -> others: fingerprint of one console at one frame
};

#pragma pack(push, 1)
struct Hello
{
    u8 Type;
    u32 Magic;
    u32 RomHash;
    u32 Lag;
};

struct Assign
{
    u8 Type;
    u8 Player;
};

struct Start
{
    u8 Type;
    u8 Players;
};

struct InputFrame
{
    u8 Type;
    u8 Player;
    u32 Frame;
    u32 Keys;
    u8 Touching;
    u16 X;
    u16 Y;
};

// Every device fingerprints every console it runs, its own and its mirrors,
// and sends them all: the same console fingerprinted on two devices at the
// same frame must match, and the first pair that does not says when and in
// which part the two copies parted. Cheap enough to stay on in every session.
struct Check
{
    u8 Type;
    u8 Player;   // the console
    u8 Origin;   // the device that fingerprinted it
    u32 Frame;
    u32 Keys;
    u32 Ram;
    u32 Wram;
    u32 Regs9;
    u32 Regs7;
    u32 Jit;
    u64 Sys;
    u64 A9;
    u64 A7;
    // Where the memory parted: main RAM in 16 KB blocks, shared WRAM and ARM7 WRAM in 4 KB.
    u32 RamBlocks[256];
    u32 SharedBlocks[8];
    u32 Arm7Blocks[16];
};
#pragma pack(pop)

// Every device runs one console per player: its own, and a mirror of each of
// the others. A player has the same number on every device, the host 0 and the
// guests 1 to 3 in the order they arrived, and it is also the instance number
// DetMP sees: two frames sent at the same emulated instant are ordered by that
// number, so it has to name the same console everywhere.
struct Mirror
{
    int Player = -1;
    NDS* Console = nullptr;
    std::thread Thread;
    std::mutex Lock;
    std::condition_variable Cond;
    std::deque<InputFrame> Queue;
    u32 StartFrame = 0;
};
Mirror mirrors[kMaxPlayers];

// A mirror's screens are never shown, yet by default it rasterised its 3D and
// drew both 2D screens every frame on the CPU: a second full DS per player.
// The geometry engine stays (games read its tests), only the pixels go.
// Display capture goes with the 2D renderer: a game reading its own capture
// back would part, and the per-frame check would say so at once.
// debug.wmds.mirrorrender=1 puts the renderers back, to measure the gain.
class NullRenderer3D : public Renderer3D
{
public:
    NullRenderer3D() : Renderer3D(false) {}
    void Reset(GPU&) override {}
    void RenderFrame(GPU&) override {}
    u32* GetLine(int) override { return Blank; }
private:
    u32 Blank[256 * 2] {};
};

class NullRenderer2D : public GPU2D::Renderer2D
{
public:
    void DrawScanline(u32, GPU2D::Unit*) override {}
    void DrawSprites(u32, GPU2D::Unit*) override {}
    void VBlankEnd(GPU2D::Unit*, GPU2D::Unit*) override {}
};

std::atomic<bool> active {false};
std::atomic<bool> running {false};
std::string lastRequest;

int myPlayer = 0;
int numPlayers = 0;

ENetHost* host = nullptr;
// host: one peer per guest, by player number; guest: peers[0] is the host
ENetPeer* peers[kMaxPlayers] = {};

std::thread netThread;

// local console: its own inputs, applied LagFrames after they are read
std::deque<InputFrame> ownQueue;
u32 localStart = 0;

// bench: received inputs held back this long before a mirror may use them
u32 simDelayMs = 0;
std::deque<std::pair<u64, InputFrame>> delayed;

std::mutex outLock;
std::deque<InputFrame> outQueue;
std::deque<Check> checkOut;

bool diagnostics = false;

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

// Determinism diagnostics, off unless debug.wmds.diag=1 when the session
// starts: the section hashes take a full savestate of every console every 300
// frames, a hitch you can hear. Logged with the player number, so the lines of
// one console on every device compare directly.
void LogCheck(int player, NDS* nds, u32 start)
{
    if (!diagnostics || !nds->MainRAM)
        return;

    // Every frame for the first 600: the first frame where two sides disagree
    // is the one that says why. Full RAM, plus the CPU cycle counters.
    if (nds->NumFrames - start < 600)
    {
        const u32* ram = (const u32*)nds->MainRAM;
        u32 words = (nds->MainRAMMask + 1) / 4;
        u32 h = 2166136261u;
        for (u32 i = 0; i < words; i++)
            h = (h ^ ram[i]) * 16777619u;
        NP_LOG("[netplay] p%d EARLY frame=%u ram=%08X sys=%llu a9=%llu a7=%llu",
               player, nds->NumFrames, h, (unsigned long long)nds->GetSysTimestamp(),
               (unsigned long long)nds->ARM9Timestamp, (unsigned long long)nds->ARM7Timestamp);
    }

    if ((nds->NumFrames % 300) != 0)
        return;

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
    NP_LOG("[netplay] p%d SECT frame=%u%s", player, nds->NumFrames, line.c_str());
}

constexpr u32 kCheckEvery = 60;

std::mutex checkLock;
std::map<u64, Check> checks; // (player << 32 | frame) -> this device's fingerprint
std::map<u64, Check> remoteChecks;
bool desyncLogged[kMaxPlayers] = {};
u32 desyncCount[kMaxPlayers] = {};

u32 HashWords(const u8* data, u32 len)
{
    const u32* w = (const u32*)data;
    u32 h = 2166136261u;
    for (u32 i = 0; i < len / 4; i++)
        h = (h ^ w[i]) * 16777619u;
    return h;
}

Check MakeCheck(int player, NDS* nds)
{
    Check c {};
    c.Type = Msg_Check;
    c.Player = (u8)player;
    c.Origin = (u8)myPlayer;
    c.Frame = nds->NumFrames;
    c.Keys = nds->KeyInput;
    c.Ram = HashWords(nds->MainRAM, nds->MainRAMMask + 1);
    c.Wram = HashWords(nds->SharedWRAM, nds->SharedWRAMSize) ^ (HashWords(nds->ARM7WRAM, nds->ARM7WRAMSize) * 31u);
    c.Regs9 = HashWords((const u8*)nds->ARM9.R, sizeof(nds->ARM9.R)) ^ nds->ARM9.CPSR;
    c.Regs7 = HashWords((const u8*)nds->ARM7.R, sizeof(nds->ARM7.R)) ^ nds->ARM7.CPSR;
#ifdef JIT_ENABLED
    c.Jit = (nds->IsJITEnabled() ? 1u : 0u) | ((u32)nds->JIT.GetMaxBlockSize() << 1)
        | (nds->JIT.LiteralOptimizationsEnabled() ? 1u << 16 : 0u)
        | (nds->JIT.BranchOptimizationsEnabled() ? 1u << 17 : 0u)
        | (nds->JIT.FastMemoryEnabled() ? 1u << 18 : 0u);
#else
    c.Jit = 0;
#endif
    const u32 ramLen = nds->MainRAMMask + 1;
    for (u32 i = 0; i < 256 && (i + 1) * 0x4000 <= ramLen; i++)
        c.RamBlocks[i] = HashWords(nds->MainRAM + i * 0x4000, 0x4000);
    for (u32 i = 0; i < 8 && (i + 1) * 0x1000 <= nds->SharedWRAMSize; i++)
        c.SharedBlocks[i] = HashWords(nds->SharedWRAM + i * 0x1000, 0x1000);
    for (u32 i = 0; i < 16 && (i + 1) * 0x1000 <= nds->ARM7WRAMSize; i++)
        c.Arm7Blocks[i] = HashWords(nds->ARM7WRAM + i * 0x1000, 0x1000);
    c.Sys = nds->GetSysTimestamp();
    c.A9 = nds->ARM9Timestamp;
    c.A7 = nds->ARM7Timestamp;
    return c;
}

// Caller holds checkLock.
void Compare(const Check& a, const Check& b)
{
    int p = a.Player;
    if (p >= kMaxPlayers) return;
    bool same = a.Keys == b.Keys && a.Ram == b.Ram && a.Wram == b.Wram && a.Regs9 == b.Regs9
        && a.Regs7 == b.Regs7 && a.Sys == b.Sys && a.A9 == b.A9 && a.A7 == b.A7;
    if (same) return;
    desyncCount[p]++;
    if (desyncLogged[p])
    {
        if (desyncCount[p] % 30 == 0)
            NP_LOG("[netplay] DESYNC p%d still apart at frame %u (%u checks)", p, a.Frame, desyncCount[p]);
        return;
    }
    desyncLogged[p] = true;
    NP_LOG("[netplay] DESYNC p%d first seen at frame %u, device %d vs device %d:%s%s%s%s%s%s%s%s%s",
           p, a.Frame, a.Origin, b.Origin,
           a.Keys != b.Keys ? " KEYS" : "", a.Ram != b.Ram ? " RAM" : "", a.Wram != b.Wram ? " WRAM" : "",
           a.Regs9 != b.Regs9 ? " ARM9-REGS" : "", a.Regs7 != b.Regs7 ? " ARM7-REGS" : "",
           a.Sys != b.Sys ? " SYS-CLOCK" : "", a.A9 != b.A9 ? " ARM9-CLOCK" : "", a.A7 != b.A7 ? " ARM7-CLOCK" : "",
           a.Jit != b.Jit ? " JIT-SETTINGS" : "");
    NP_LOG("[netplay] DESYNC p%d detail: keys %08X/%08X sys %llu/%llu a9 %llu/%llu a7 %llu/%llu jit %08X/%08X",
           p, a.Keys, b.Keys, (unsigned long long)a.Sys, (unsigned long long)b.Sys,
           (unsigned long long)a.A9, (unsigned long long)b.A9, (unsigned long long)a.A7, (unsigned long long)b.A7,
           a.Jit, b.Jit);
    // The blocks that differ, as DS addresses: main RAM at 0x02000000, shared WRAM
    // at 0x03000000, ARM7 WRAM at 0x03800000.
    std::string where;
    char buf[48];
    for (int i = 0; i < 256; i++)
        if (a.RamBlocks[i] != b.RamBlocks[i]) { snprintf(buf, sizeof(buf), " %08X", 0x02000000 + i * 0x4000); where += buf; }
    for (int i = 0; i < 8; i++)
        if (a.SharedBlocks[i] != b.SharedBlocks[i]) { snprintf(buf, sizeof(buf), " W%08X", 0x03000000 + i * 0x1000); where += buf; }
    for (int i = 0; i < 16; i++)
        if (a.Arm7Blocks[i] != b.Arm7Blocks[i]) { snprintf(buf, sizeof(buf), " A%08X", 0x03800000 + i * 0x1000); where += buf; }
    NP_LOG("[netplay] DESYNC p%d blocks:%s", p, where.c_str());
}

void Prune(std::map<u64, Check>& m, int player, u32 frame)
{
    if (frame < kCheckEvery * 20) return;
    u64 lo = (u64)player << 32;
    u64 hi = lo | (frame - kCheckEvery * 20);
    m.erase(m.lower_bound(lo), m.upper_bound(hi));
}

void QueueCheck(const Check& c);

// A console just finished a frame on this device.
void Fingerprint(int player, NDS* nds)
{
    // Every frame of the first second, then once a second: the first frame apart is the lead.
    if ((nds->NumFrames > kCheckEvery && (nds->NumFrames % kCheckEvery) != 0) || !nds->MainRAM) return;
    Check c = MakeCheck(player, nds);
    u64 key = ((u64)player << 32) | c.Frame;
    {
        std::lock_guard<std::mutex> lk(checkLock);
        checks[key] = c;
        auto it = remoteChecks.find(key);
        if (it != remoteChecks.end()) { Compare(c, it->second); remoteChecks.erase(it); }
        Prune(checks, player, c.Frame);
    }
    QueueCheck(c);
}

void OnRemoteCheck(const Check& c)
{
    if (c.Player >= kMaxPlayers) return;
    u64 key = ((u64)c.Player << 32) | c.Frame;
    std::lock_guard<std::mutex> lk(checkLock);
    auto it = checks.find(key);
    if (it != checks.end()) Compare(it->second, c);
    else remoteChecks[key] = c;
    Prune(remoteChecks, c.Player, c.Frame);
}

void ApplyInput(NDS* nds, const InputFrame& f)
{
    nds->SetKeyMask(f.Keys);
    if (f.Touching)
        nds->TouchScreen(f.X, f.Y);
    else
        nds->ReleaseScreen();
}

bool IsHost()
{
    return myPlayer == 0;
}

void SendTo(ENetPeer* peer, const void* data, size_t len)
{
    if (!peer) return;
    ENetPacket* pkt = enet_packet_create(data, len, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, pkt);
}

// A guest talks to the host only. The host sends to every guest but one: the
// one a relayed packet came from, which already has it.
void SendAll(const void* data, size_t len, int except = -1)
{
    if (IsHost())
    {
        for (int p = 1; p < kMaxPlayers; p++)
            if (p != except) SendTo(peers[p], data, len);
    }
    else
        SendTo(peers[0], data, len);
    enet_host_flush(host);
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

#pragma pack(push, 1)
struct JitSettings
{
    u8 Enabled;
    u32 MaxBlockSize;
    u8 Literal;
    u8 Branch;
    u8 FastMemory;
    u8 HgEngineFix;
};
#pragma pack(pop)

JitSettings JitOf(NDS* nds)
{
    JitSettings j {};
    j.Enabled = nds->IsJITEnabled() ? 1 : 0;
#ifdef JIT_ENABLED
    j.MaxBlockSize = (u32)nds->JIT.GetMaxBlockSize();
    j.Literal = nds->JIT.LiteralOptimizationsEnabled() ? 1 : 0;
    j.Branch = nds->JIT.BranchOptimizationsEnabled() ? 1 : 0;
    j.FastMemory = nds->JIT.FastMemoryEnabled() ? 1 : 0;
    j.HgEngineFix = nds->JIT.HgEngineFixEnabled() ? 1 : 0;
#endif
    return j;
}

// Everything another device needs to build a mirror of this console that is
// the same machine in the same state: the savestate does not carry the
// firmware (MAC address, player name), the BIOS, or the ROM. The ROM is not
// sent, every player has it; its hash was compared in the hello.
// Layout: type, player, raw length, zstd data.
std::vector<u8> BuildPackage(NDS* nds, Savestate& state)
{
    std::vector<u8> raw;
    const Firmware& fw = nds->SPI.GetFirmware();
    PutBlob(raw, fw.Buffer(), fw.Length());
    PutBlob(raw, nds->GetARM9BIOS().data(), (u32)nds->GetARM9BIOS().size());
    PutBlob(raw, nds->GetARM7BIOS().data(), (u32)nds->GetARM7BIOS().size());
    PutBlob(raw, nds->GetNDSSave() ? nds->GetNDSSave() : (const u8*)"", nds->GetNDSSave() ? nds->GetNDSSaveLength() : 0);
    PutBlob(raw, (const u8*)state.Buffer(), state.Length());
    // The JIT settings are the player's own, and they are part of the machine: where
    // blocks end decides when interrupts are seen. A mirror built with the receiving
    // player's settings runs the same state on other timing and drifts away.
    JitSettings jit = JitOf(nds);
    PutBlob(raw, (const u8*)&jit, sizeof(jit));

    std::vector<u8> pkt(2 + 4 + ZSTD_compressBound(raw.size()));
    pkt[0] = Msg_Package;
    pkt[1] = (u8)myPlayer;
    u32 rawLen = (u32)raw.size();
    memcpy(&pkt[2], &rawLen, 4);
    size_t clen = ZSTD_compress(&pkt[6], pkt.size() - 6, raw.data(), raw.size(), 3);
    if (ZSTD_isError(clen)) return {};
    pkt.resize(6 + clen);
    NP_LOG("[netplay] package: %u bytes, %zu compressed, frame %u", rawLen, clen, nds->NumFrames);
    return pkt;
}

NDS* BuildMirror(const std::vector<u8>& pkt, NDS* local, const ArgsFactory& mirrorArgs, Mirror& m)
{
    if (pkt.size() < 6) return nullptr;
    u32 rawLen;
    memcpy(&rawLen, &pkt[2], 4);
    std::vector<u8> raw(rawLen);
    size_t got = ZSTD_decompress(raw.data(), rawLen, &pkt[6], pkt.size() - 6);
    if (ZSTD_isError(got) || got != rawLen) return nullptr;

    const u8* p = raw.data();
    const u8* end = p + raw.size();
    const u8 *fw, *bios9, *bios7, *save, *state;
    u32 fwLen, bios9Len, bios7Len, saveLen, stateLen;
    if (!GetBlob(p, end, fw, fwLen) || !GetBlob(p, end, bios9, bios9Len) || !GetBlob(p, end, bios7, bios7Len)
        || !GetBlob(p, end, save, saveLen) || !GetBlob(p, end, state, stateLen))
        return nullptr;
    const u8* jitBlob;
    u32 jitLen;
    if (!GetBlob(p, end, jitBlob, jitLen) || jitLen != sizeof(JitSettings))
        return nullptr;
    JitSettings jit;
    memcpy(&jit, jitBlob, sizeof(jit));
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
    // The sender's JIT, never this device's: see BuildPackage.
    if (jit.Enabled)
    {
        JITArgs j;
        j.MaxBlockSize = jit.MaxBlockSize;
        j.LiteralOptimizations = jit.Literal != 0;
        j.BranchOptimizations = jit.Branch != 0;
        j.FastMemory = jit.FastMemory != 0;
        j.HgEngineFix = jit.HgEngineFix != 0;
        args->JIT = j;
    }
    else
    {
        args->JIT = std::nullopt;
    }
    NP_LOG("[netplay] mirror JIT: enabled=%d block=%u lit=%d br=%d fast=%d hg=%d",
           jit.Enabled, jit.MaxBlockSize, jit.Literal, jit.Branch, jit.FastMemory, jit.HgEngineFix);

    // the mirror's userdata is its slot: Platform callbacks find it back by address
    NDS* nds = new NDS(std::move(*args), &m);
    if (Property("debug.wmds.mirrorrender") != "1")
    {
        nds->GPU.SetRenderer3D(std::make_unique<NullRenderer3D>());
        nds->GPU.SetRenderer2D(std::make_unique<NullRenderer2D>());
        NP_LOG("[netplay] mirror renders nothing");
    }
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
    auto cart = NDSCart::ParseROM(localCart->GetROM(), localCart->GetROMLength(), &m, std::move(cartArgs));
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

void MirrorLoop(Mirror* m)
{
    u64 mirrorUs = 0;
    u32 mirrorFrames = 0;
    NP_LOG("[netplay] mirror of p%d running from frame %u", m->Player, m->Console->NumFrames);
    m->Console->JIT.ResetBlockCache();
    while (running.load())
    {
        InputFrame f;
        {
            std::unique_lock<std::mutex> lk(m->Lock);
            m->Cond.wait_for(lk, std::chrono::milliseconds(50), [m] { return !m->Queue.empty() || !running.load(); });
            if (m->Queue.empty()) continue;
            f = m->Queue.front();
            m->Queue.pop_front();
        }
        NDS* nds = m->Console;
        if (f.Frame < nds->NumFrames) continue;
        if (f.Frame > nds->NumFrames)
            NP_LOG("[netplay] mirror of p%d: input for frame %u while at frame %u", m->Player, f.Frame, nds->NumFrames);

        ApplyInput(nds, f);
        auto t0 = std::chrono::steady_clock::now();
        nds->RunFrame();
        // What a mirror costs per frame: under 16.7 ms it keeps up with a 60 fps game.
        mirrorUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        if (++mirrorFrames == 300)
        {
            NP_LOG("[netplay] mirror of p%d: %.2f ms per frame", m->Player, mirrorUs / 300000.0);
            mirrorUs = 0;
            mirrorFrames = 0;
        }
        MPInterface::Get().SetNow(m->Player, nds->GetSysTimestamp());
        LogCheck(m->Player, nds, m->StartFrame);
        Fingerprint(m->Player, nds);
    }
    MPInterface::Get().Leave(m->Player);
}

void QueueCheck(const Check& c)
{
    std::lock_guard<std::mutex> lk(outLock);
    checkOut.push_back(c);
}

void Deliver(const InputFrame& f)
{
    if (f.Player >= kMaxPlayers || f.Player == myPlayer) return;
    Mirror& m = mirrors[f.Player];
    if (!m.Console) return;
    std::lock_guard<std::mutex> lk(m.Lock);
    m.Queue.push_back(f);
    m.Cond.notify_all();
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
                if (IsHost())
                {
                    for (int p = 1; p < kMaxPlayers; p++) SendTo(peers[p], &f, sizeof(f));
                }
                else
                    SendTo(peers[0], &f, sizeof(f));
                any = true;
            }
            while (!checkOut.empty())
            {
                Check c = checkOut.front();
                checkOut.pop_front();
                if (IsHost())
                {
                    for (int p = 1; p < kMaxPlayers; p++) SendTo(peers[p], &c, sizeof(c));
                }
                else
                    SendTo(peers[0], &c, sizeof(c));
                any = true;
            }
            if (any) enet_host_flush(host);
        }

        ENetEvent evt;
        while (enet_host_service(host, &evt, 1) > 0)
        {
            if (evt.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                // a console nobody drives any more would hold everybody's DetMP
                NP_LOG("[netplay] a player left, session over");
                running = false;
                for (auto& m : mirrors) m.Cond.notify_all();
                break;
            }
            if (evt.type != ENET_EVENT_TYPE_RECEIVE)
                continue;
            if (evt.packet->dataLength == sizeof(Check) && evt.packet->data[0] == Msg_Check)
            {
                Check c;
                memcpy(&c, evt.packet->data, sizeof(c));
                if (IsHost())
                {
                    bool relayed = false;
                    for (int p = 1; p < kMaxPlayers; p++)
                        if (p != c.Origin && peers[p]) { SendTo(peers[p], &c, sizeof(c)); relayed = true; }
                    if (relayed) enet_host_flush(host);
                }
                OnRemoteCheck(c);
            }
            else if (evt.packet->dataLength == sizeof(InputFrame) && evt.packet->data[0] == Msg_Input)
            {
                InputFrame f;
                memcpy(&f, evt.packet->data, sizeof(f));
                // the host relays one guest's input to the other guests
                if (IsHost())
                {
                    bool relayed = false;
                    for (int p = 1; p < kMaxPlayers; p++)
                        if (p != f.Player && peers[p]) { SendTo(peers[p], &f, sizeof(f)); relayed = true; }
                    if (relayed) enet_host_flush(host);
                }
                if (simDelayMs)
                    delayed.emplace_back(NowMs() + simDelayMs, f);
                else
                    Deliver(f);
            }
            enet_packet_destroy(evt.packet);
        }

        if (!delayed.empty())
        {
            u64 now = NowMs();
            while (!delayed.empty() && delayed.front().first <= now)
            {
                Deliver(delayed.front().second);
                delayed.pop_front();
            }
        }
    }
}

void Teardown()
{
    for (auto& p : peers)
    {
        if (p) enet_peer_disconnect_now(p, 0);
        p = nullptr;
    }
    if (host) enet_host_destroy(host);
    host = nullptr;
}

// Waiting for the other players never blocks the game: the connection is
// pumped once per frame, and only the exchange that follows, a few seconds,
// holds the frame.
bool connecting = false;
std::string joinAddress;
u64 connectStart = 0, lastDial = 0, lastArrival = 0;
int expectedPlayers = 0;     // 0: unknown, start once nobody new came for a while
bool assigned = false;       // guest: the host gave us a number
u32 ownRomHash = 0;
constexpr u64 kConnectTimeoutMs = 180000;
constexpr u64 kRedialMs = 3000;
constexpr u64 kSettleMs = 8000;

void Dial()
{
    ENetAddress addr {};
    enet_address_set_host(&addr, joinAddress.c_str());
    addr.port = sessionPort;
    if (peers[0]) enet_peer_reset(peers[0]);
    peers[0] = nullptr;
    enet_host_connect(host, &addr, 1, 0);
    lastDial = NowMs();
}

u16 ParsePort(const std::string& text)
{
    int port = atoi(text.c_str());
    return port > 0 && port <= 65535 ? (u16)port : kDefaultPort;
}

bool StartConnect(const std::string& request, NDS* local)
{
    static bool enetReady = false;
    if (!enetReady) { enet_initialize(); enetReady = true; }
    Teardown();
    ownRomHash = RomHash(local);
    assigned = false;
    numPlayers = 0;

    if (request.rfind("host", 0) == 0)
    {
        myPlayer = 0;
        // "host:<n>": the launching app knows how many are coming
        expectedPlayers = request.size() > 5 ? atoi(request.c_str() + 5) : 0;
        size_t portSep = request.find(':', 5);
        sessionPort = portSep != std::string::npos ? ParsePort(request.substr(portSep + 1)) : kDefaultPort;
        if (expectedPlayers > kMaxPlayers) expectedPlayers = kMaxPlayers;
        ENetAddress addr {ENET_HOST_ANY, sessionPort};
        host = enet_host_create(&addr, kMaxPlayers + 2, 1, 0, 0);
        if (!host) { NP_LOG("[netplay] could not listen on %u", sessionPort); return false; }
        NP_LOG("[netplay] hosting on port %u, expecting %d players", sessionPort, expectedPlayers);
    }
    else
    {
        myPlayer = -1;
        joinAddress = request.substr(5);
        // "address:port" (a single colon, so a bare IPv6 address is left alone)
        sessionPort = kDefaultPort;
        size_t portSep = joinAddress.rfind(':');
        if (portSep != std::string::npos && joinAddress.find(':') == portSep)
        {
            sessionPort = ParsePort(joinAddress.substr(portSep + 1));
            joinAddress = joinAddress.substr(0, portSep);
        }
        host = enet_host_create(nullptr, 1, 1, 0, 0);
        if (!host) return false;
        NP_LOG("[netplay] joining %s", joinAddress.c_str());
        Dial();
    }
    connecting = true;
    connectStart = NowMs();
    lastArrival = 0;
    return true;
}

int GuestCount()
{
    int n = 0;
    for (int p = 1; p < kMaxPlayers; p++) if (peers[p]) n++;
    return n;
}

// Host side of the wait. Returns true when the session should start.
bool PumpHost()
{
    ENetEvent evt;
    while (enet_host_service(host, &evt, 0) > 0)
    {
        if (evt.type == ENET_EVENT_TYPE_DISCONNECT)
        {
            for (int p = 1; p < kMaxPlayers; p++)
                if (peers[p] == evt.peer) { peers[p] = nullptr; NP_LOG("[netplay] p%d left before the start", p); }
            continue;
        }
        if (evt.type != ENET_EVENT_TYPE_RECEIVE) continue;
        bool hello = evt.packet->dataLength == sizeof(Hello) && evt.packet->data[0] == Msg_Hello;
        Hello h {};
        if (hello) memcpy(&h, evt.packet->data, sizeof(h));
        enet_packet_destroy(evt.packet);
        if (!hello) continue;

        int slot = -1;
        for (int p = 1; p < kMaxPlayers && slot < 0; p++) if (!peers[p]) slot = p;
        if (h.Magic != kMagic || h.RomHash != ownRomHash || h.Lag != kLagFrames || slot < 0)
        {
            NP_LOG("[netplay] turned a player away (%s)", slot < 0 ? "full" : "not the same game");
            u8 no = Msg_Reject;
            SendTo(evt.peer, &no, 1);
            enet_host_flush(host);
            enet_peer_disconnect_later(evt.peer, 0);
            continue;
        }
        peers[slot] = evt.peer;
        enet_peer_timeout(evt.peer, 0, 10000, 30000);
        Assign a {Msg_Assign, (u8)slot};
        SendTo(evt.peer, &a, sizeof(a));
        enet_host_flush(host);
        lastArrival = NowMs();
        NP_LOG("[netplay] p%d joined", slot);
    }

    int players = 1 + GuestCount();
    if (players >= 2)
    {
        if (expectedPlayers > 0 ? players >= expectedPlayers : NowMs() - lastArrival > kSettleMs)
            return true;
    }
    if (NowMs() - connectStart > kConnectTimeoutMs && players < 2)
    {
        NP_LOG("[netplay] nobody came");
        connecting = false;
        Teardown();
    }
    return false;
}

// Guest side of the wait. Returns true once the host says start.
bool PumpGuest()
{
    ENetEvent evt;
    while (enet_host_service(host, &evt, 0) > 0)
    {
        if (evt.type == ENET_EVENT_TYPE_CONNECT)
        {
            peers[0] = evt.peer;
            enet_peer_timeout(evt.peer, 0, 10000, 30000);
            Hello h {Msg_Hello, kMagic, ownRomHash, kLagFrames};
            SendTo(peers[0], &h, sizeof(h));
            enet_host_flush(host);
            continue;
        }
        if (evt.type == ENET_EVENT_TYPE_DISCONNECT)
        {
            peers[0] = nullptr;
            assigned = false;
            continue;
        }
        if (evt.type != ENET_EVENT_TYPE_RECEIVE) continue;
        const u8* d = evt.packet->data;
        size_t len = evt.packet->dataLength;
        bool start = false;
        if (len == sizeof(Assign) && d[0] == Msg_Assign)
        {
            myPlayer = d[1];
            assigned = true;
            NP_LOG("[netplay] we are p%d", myPlayer);
        }
        else if (len == 1 && d[0] == Msg_Reject)
        {
            NP_LOG("[netplay] the host turned us away (not the same game, or full)");
            connecting = false;
        }
        else if (len == sizeof(Start) && d[0] == Msg_Start && assigned)
        {
            numPlayers = d[1];
            start = true;
        }
        enet_packet_destroy(evt.packet);
        if (!connecting) { Teardown(); return false; }
        if (start) return true;
    }

    u64 now = NowMs();
    if (now - connectStart > kConnectTimeoutMs)
    {
        NP_LOG("[netplay] never got in");
        connecting = false;
        Teardown();
        return false;
    }
    // the host may not be listening yet: dial again until it is
    if (!peers[0] && now - lastDial > kRedialMs)
        Dial();
    return false;
}

// Everybody sends their console, everybody waits for all the others. The host
// relays each guest's package to the other guests.
bool Exchange(NDS* local, const ArgsFactory& mirrorArgs)
{
    connecting = false;

    if (IsHost())
    {
        numPlayers = 1 + GuestCount();
        // renumber densely: a guest who left before the start leaves a hole
        int next = 1;
        for (int p = 1; p < kMaxPlayers; p++)
        {
            if (!peers[p]) continue;
            if (p != next)
            {
                peers[next] = peers[p];
                peers[p] = nullptr;
                Assign a {Msg_Assign, (u8)next};
                SendTo(peers[next], &a, sizeof(a));
            }
            next++;
        }
        Start s {Msg_Start, (u8)numPlayers};
        SendAll(&s, sizeof(s));
        NP_LOG("[netplay] starting with %d players", numPlayers);
    }

    Savestate ownState(Savestate::DEFAULT_SIZE);
    if (ownState.Error || !local->DoSavestate(&ownState))
    { NP_LOG("[netplay] could not save our console"); Teardown(); return false; }
    std::vector<u8> pkg = BuildPackage(local, ownState);
    if (pkg.empty()) { Teardown(); return false; }
    SendAll(pkg.data(), pkg.size());

    std::vector<u8> packages[kMaxPlayers];
    int have = 0;
    u64 start = NowMs();
    while (have < numPlayers - 1 && NowMs() - start < 120000)
    {
        ENetEvent evt;
        if (enet_host_service(host, &evt, 50) <= 0) continue;
        if (evt.type == ENET_EVENT_TYPE_DISCONNECT)
        { NP_LOG("[netplay] a player left during the exchange"); Teardown(); return false; }
        if (evt.type != ENET_EVENT_TYPE_RECEIVE) continue;
        const u8* d = evt.packet->data;
        size_t len = evt.packet->dataLength;
        if (len > 6 && d[0] == Msg_Package && d[1] < numPlayers && d[1] != myPlayer && packages[d[1]].empty())
        {
            packages[d[1]].assign(d, d + len);
            have++;
            if (IsHost()) SendAll(d, len, d[1]);
        }
        else if (len == sizeof(Assign) && d[0] == Msg_Assign)
            myPlayer = d[1];
        enet_packet_destroy(evt.packet);
    }
    if (have < numPlayers - 1)
    { NP_LOG("[netplay] never got every console (%d of %d)", have, numPlayers - 1); Teardown(); return false; }

    for (int p = 0; p < numPlayers; p++)
    {
        if (p == myPlayer) continue;
        Mirror& m = mirrors[p];
        m.Player = p;
        m.Console = BuildMirror(packages[p], local, mirrorArgs, m);
        if (!m.Console)
        {
            NP_LOG("[netplay] could not build the mirror of p%d", p);
            for (auto& mm : mirrors) { delete mm.Console; mm.Console = nullptr; }
            Teardown();
            return false;
        }
        m.StartFrame = m.Console->NumFrames;
    }

    // The local console loads back the very state it sent, as its mirrors do
    // on the other devices. Loading recomputes things a savestate does not
    // carry (memory timings, code regions, JIT mappings); a console that just
    // kept running still had its own, and ran its first frame 8 to 18 cycles
    // apart from its mirror. Measured: every frame off from the first one.
    {
        // Reset first, as the mirrors are built: a savestate leaves some fields as they
        // were (CyclesToRun among them), and a console that had been running kept its own.
        // Measured with the interpreter: the first frame one cycle apart, then memory.
        local->Reset();
        Savestate reload(ownState.Buffer(), ownState.Length(), false);
        if (reload.Error || !local->DoSavestate(&reload))
            NP_LOG("[netplay] WARNING could not reload our own state, expect a desync");
    }

    // Every JIT block cache starts empty, on every device. Where blocks begin
    // decides where the JIT checks for interrupts, so a console with a warm
    // cache and a mirror with a cold one run the same state on different
    // timing. Measured: every checkpoint off as soon as the JIT was on.
    local->JIT.ResetBlockCache();
    localStart = local->NumFrames;

    // The air shared by the consoles starts here, on every device alike.
    MPInterface::Set(MPInterface_Deterministic);
    MPInterface::Get().ResetSession(myPlayer, local->GetSysTimestamp());
    for (int p = 0; p < numPlayers; p++)
        if (p != myPlayer)
            MPInterface::Get().ResetSession(p, mirrors[p].Console->GetSysTimestamp());

    // The first LagFrames frames of each console have no input from anybody:
    // neutral on every device.
    {
        std::lock_guard<std::mutex> lk(checkLock);
        checks.clear();
        remoteChecks.clear();
        for (int p = 0; p < kMaxPlayers; p++) { desyncLogged[p] = false; desyncCount[p] = 0; }
    }
    {
        std::lock_guard<std::mutex> lk(outLock);
        checkOut.clear();
    }
    ownQueue.clear();
    for (u32 i = 0; i < kLagFrames; i++)
        ownQueue.push_back({Msg_Input, (u8)myPlayer, local->NumFrames + i, 0xFFF, 0, 0, 0});
    for (int p = 0; p < numPlayers; p++)
    {
        if (p == myPlayer) continue;
        Mirror& m = mirrors[p];
        std::lock_guard<std::mutex> lk(m.Lock);
        m.Queue.clear();
        for (u32 i = 0; i < kLagFrames; i++)
            m.Queue.push_back({Msg_Input, (u8)p, m.Console->NumFrames + i, 0xFFF, 0, 0, 0});
    }

    diagnostics = Property("debug.wmds.diag") == "1";
    std::string d = Property("debug.wmds.delay");
    simDelayMs = d.empty() ? 0 : (u32)atoi(d.c_str());

    running = true;
    // before the threads: a mirror's first wifi call asks who is local
    active = true;
    netThread = std::thread(NetLoop);
    for (int p = 0; p < numPlayers; p++)
        if (p != myPlayer)
            mirrors[p].Thread = std::thread(MirrorLoop, &mirrors[p]);
    NP_LOG("[netplay] session running: %d players, we are p%d, local frame %u, jit %d, delay %u ms",
           numPlayers, myPlayer, local->NumFrames, local->IsJITEnabled() ? 1 : 0, simDelayMs);
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
        bool go = (myPlayer == 0) ? PumpHost() : PumpGuest();
        if (go)
            return Exchange(local, mirrorArgs);
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

    if (request != "host" && request.rfind("host:", 0) != 0 && request.rfind("join:", 0) != 0)
        return false;
    StartConnect(request, local);
    return false;
}

bool Active()
{
    return active.load();
}

int LocalMpId(int instanceId)
{
    return active.load() ? myPlayer : instanceId;
}

void BeforeLocalFrame(NDS* local, u32 liveKeys, bool liveTouching, u16 liveX, u16 liveY)
{
    if (!active.load()) return;

    u32 n = local->NumFrames;
    InputFrame live {Msg_Input, (u8)myPlayer, n + kLagFrames, liveKeys & 0xFFF, (u8)(liveTouching ? 1 : 0), liveX, liveY};
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
    MPInterface::Get().SetNow(myPlayer, local->GetSysTimestamp());
    LogCheck(myPlayer, local, localStart);
    Fingerprint(myPlayer, local);
}

bool IsMirror(void* userdata)
{
    auto* p = (Mirror*)userdata;
    return p >= &mirrors[0] && p < &mirrors[kMaxPlayers];
}

NDS* MirrorNDS(void* userdata)
{
    return IsMirror(userdata) ? ((Mirror*)userdata)->Console : nullptr;
}

int MirrorId(void* userdata)
{
    return IsMirror(userdata) ? ((Mirror*)userdata)->Player : -1;
}

void Stop()
{
    running = false;
    for (auto& m : mirrors) m.Cond.notify_all();
    for (auto& m : mirrors) if (m.Thread.joinable()) m.Thread.join();
    if (netThread.joinable()) netThread.join();
    Teardown();
    for (auto& m : mirrors)
    {
        delete m.Console;
        m.Console = nullptr;
        m.Player = -1;
        m.Queue.clear();
    }
    active = false;
    NP_LOG("[netplay] session over");
}

}
