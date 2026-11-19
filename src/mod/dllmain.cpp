// ewcoop.dll: hooks idGameLocal::RunFrame through its vtable so our code runs on the game
// thread. Spawns Joseph next to the player and lets a second "player" drive him (puppet).
//   F9        = spawn Joseph next to you (or bring the existing one to you)
//   F10       = puppet mode on/off (blocks his own AI decisions while on)
//   F11       = cycle puppet movement: AI walk (default) / hybrid (v2) / slide (v0)
//   Numpad 8/5/7/9 = move forward/back/left/right relative to the puppet camera yaw
//   Numpad 4/6 = turn the puppet camera yaw, hold Numpad 0 to run
//   Ctrl+F8   = spawn a SECOND Sebastian in front of you (correct rig; Ctrl+Alt+F8 = whole-model Joseph)
//   Ctrl+F9   = player 2 wears Joseph's mesh; blanks Sebastian's body (Ctrl+Shift+F9 cycles head kit)
//   Ctrl+F7   = switch the controlled character: P2 becomes the local player (camera + input + HUD)
//   Ctrl+H    = toggle the on-screen debug HUD (session state + peer counts)
//   Ctrl+F10  = split enemy aggro between both players by proximity (default on)
//   Ctrl+F11  = run console commands from ewcmd.txt next to the DLL (';'-separated) — drives MP start/join
//   Ctrl+Shift+F12 = host (party lobby + match, direct backend, SP map); Ctrl+Shift+S = start
//   Ctrl+Shift+I = print host IP(s) + net_port so you can hand the friend a connect address
//   Ctrl+Shift+J = join as a client (direct backend + `connect <EWCOOP_HOST|ewhost.txt|127.0.0.1>`)
//   Ctrl+Shift+N = toggle net status logging (session state + party/game peer connectionState)
//   Ctrl+Shift+P = let the ENGINE spawn a 2nd real player (SetupPlayer clamp off + numPlayers=2; reload map)
//   Ctrl+End  = remove the hooks (the DLL stays loaded but does nothing)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "game.h"
#include "actor_vtables.h"

using namespace game;

static uintptr_t g_base, g_exeEnd;
static char g_dir[MAX_PATH];  // folder of the loaded DLL (build\release)
static FILE* g_log;
static volatile bool g_unhook;

struct VHook {
    void** slot;
    void* orig;
};
static VHook g_runFrameHook, g_actionHook, g_damageHook, g_thinkHook, g_isLocalHook, g_setHealthHook,
    g_nataThinkHook, g_restartHook;
static IsLocal_t g_origIsLocal;
static Think_t g_origThink;
static void* volatile g_ctxPlayer;  // while set, GetPlayer(0) returns this player
static volatile DWORD g_ctxThread;  // ...but only on this thread
static bool g_getPlayerHooked;
static PlayerDamage_t g_origDamage;
static void* g_player1;  // player 0, refreshed every frame
static RunFrame_t g_origRunFrame;
static HandleAction_t g_origHandleAction;

static void* g_game;     // gameLocal seen in the last RunFrame
static void* g_joseph;   // entity we spawned
static void* g_player2;  // second idPlayer we spawned
static bool g_josephBody;  // player 2 wears Joseph's mesh (alt body on); Sebastian's body blanked each frame
static Vec3 g_playerPos[2];        // player 1 / 2 origin, refreshed once a frame in hkRunFrame (safe to read anywhere)
static bool g_playerPosValid[2];
static bool g_player1Alive;
static bool g_diedFromRemote;  // our player was taken down because the other player died (don't echo it back)
static bool g_levelRestarted;  // a checkpoint restart wiped the level during this frame: skip the rest of it
static bool g_applyingGive;    // we are giving items ourselves (don't share them as pickups)
static void* g_p2SyncOwner;    // enemy player 2 is attached to here (replayed grab)
static void* g_p1SyncOwner;    // enemy our player is attached to (to send the end)
static bool g_meJoseph, g_meJosephOn;  // Ctrl+Shift+X: WE play as Joseph (choice / applied to our player)
static bool g_meJosephChosen;          // the player picked (Ctrl+Shift+X) - otherwise a joining player is Joseph
static bool g_remoteIsJoseph;          // the partner plays as Joseph (packet state bit6)
static bool g_wantJoseph;  // Ctrl+F9 choice, kept across respawns and game restarts (ewjoseph.txt next to the DLL)
static void saveJosephChoice();
static void loadJosephChoice();
static const char* entityName(void* e);  // shared enemies section
static void takeSnapshot(void* gameLocal, const char* id);  // snapshot section
static void* g_aiThis;     // the enemy whose target-select last ran (feeds the AI player getter)
static bool g_aiAggro = true;  // split enemy aggro between the two players by proximity (Ctrl+F10)
static AiTargetContext_t g_origAiContext;
static void* g_sebastian;  // the real player 1 while we are "playing as" player 2
static bool g_playAsP2;    // Ctrl+F7: the game drives player 2 (Joseph) with the real input + camera
static void* g_origPresentableLocal;  // the presentable player's IsLocal (vtable +0x1C8) we bypass for P2
enum Mode { ModeAiWalk, ModeHybrid, ModeSlide, ModeCount };
static const char* kModeNames[] = {"AI walk (orders his AI to walk/run to a point ahead)",
                                   "hybrid (we place him, animation kept playing)",
                                   "slide (teleport, no animation)"};
static int g_mode = ModeAiWalk;
static bool g_puppet, g_moving, g_run;
static constexpr float kOurAction = 31337.f;  // editValue tag on actions we issue
static float g_orderTimer;                    // seconds until the walk order is refreshed
static float g_yaw;      // puppet "camera" yaw, degrees
static float g_bodyYaw;  // the way Joseph's body faces
static float g_resend;   // seconds until the Move request is re-sent
static Vec3 g_pos;       // puppet position we drive (x, y used; z comes from his physics)
static float g_logTimer;
static Vec3 g_lastPos;
static float g_reloadT;  // seconds since the last "game instance changed" (checkpoint reload settle timer)
static LARGE_INTEGER g_freq, g_last;

static void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    if (g_log) {
        fputs(buf, g_log);
        fflush(g_log);
    }
}

// GetAsyncKeyState sees the keyboard globally, so with two game copies open both would react to every key.
// Only count keys while this process owns the foreground window.
static bool weAreFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static bool pressed(int vk) {
    static bool down[256];
    bool now = (GetAsyncKeyState(vk) & 0x8000) != 0 && weAreFocused();
    bool edge = now && !down[vk];
    down[vk] = now;
    return edge;
}
static bool held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0 && weAreFocused(); }

template <class T> static T vfn(void* obj, int slot) {
    return reinterpret_cast<T>((*reinterpret_cast<void***>(obj))[slot]);
}
template <class T> static T fn(uintptr_t rva) { return reinterpret_cast<T>(g_base + rva); }
template <class T> static T& field(void* obj, uintptr_t off) {
    return *reinterpret_cast<T*>(static_cast<char*>(obj) + off);
}
static bool isA(void* obj, uintptr_t vtableRva) {
    return obj && *reinterpret_cast<uintptr_t*>(obj) == g_base + vtableRva;
}

static void writeSlot(void** slot, void* value) {
    DWORD old;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = value;
        VirtualProtect(slot, sizeof(void*), old, &old);
    }
}
// Patch vtable slot `index` of the class whose vtable is at `vtableRva`. The slot must still
// hold the game's function at `expectRva`, or point outside the exe (an older ewcoop copy we
// take over from). Our hook always chains to the game's own function.
static bool hookSlot(VHook& h, uintptr_t vtableRva, int index, uintptr_t expectRva, void* hook) {
    h.slot = reinterpret_cast<void**>(g_base + vtableRva) + index;
    auto cur = reinterpret_cast<uintptr_t>(*h.slot);
    bool original = cur == g_base + expectRva;
    bool oldHook = cur < g_base || cur >= g_exeEnd;
    if (!original && !oldHook) {
        logf("[ewcoop] vtable %#llx slot %d = %p, expected %#llx. Not hooking.\n",
             (unsigned long long)vtableRva, index, *h.slot, (unsigned long long)expectRva);
        return false;
    }
    if (oldHook) logf("[ewcoop] taking over vtable %#llx slot %d from an older ewcoop\n", (unsigned long long)vtableRva, index);
    h.orig = reinterpret_cast<void*>(g_base + expectRva);
    writeSlot(h.slot, hook);
    return true;
}

static void* localPlayer(void* gameLocal) {
    auto mgr = field<char*>(gameLocal, off_gameLocal_entMgr);
    if (!mgr) return nullptr;
    void* p = *reinterpret_cast<void**>(mgr + off_entMgr_players);
    return isA(p, rva_idPlayer_vtable) ? p : nullptr;
}
// The player the engine holds in slot n: *(entMgr + 0x28 + n*8). SetupPlayer writes here per index.
static void* playerAtSlot(void* gameLocal, int slot) {
    auto mgr = field<char*>(gameLocal, off_gameLocal_entMgr);
    if (!mgr) return nullptr;
    void* p = *reinterpret_cast<void**>(mgr + off_entMgr_players + slot * 8);
    return isA(p, rva_idPlayer_vtable) ? p : nullptr;
}
// The game's player slot points at the entity whose view/HUD/input RunFrame drives; swapping it is the
// actual "control switch" (crashes the HUD until the presentable gate below is forced).
static void setPlayerSlot(void* gameLocal, void* ent) {
    auto mgr = field<char*>(gameLocal, off_gameLocal_entMgr);
    if (mgr) *reinterpret_cast<void**>(mgr + off_entMgr_players) = ent;
}
// The idPresentablePlayer behind a player entity (player+0x2A8 -> vfunc[12]); owns the HUD and model.
static void* presentableOf(void* ent) {
    void* anim = ent ? field<void*>(ent, off_entity_anim) : nullptr;
    return anim ? vfn<void* (*)(void*)>(anim, slot_anim_GetController)(anim) : nullptr;
}
// Run one console command through the engine (tokenize + dispatch). Used to drive MP (startServer/connect)
// and cvars that the retail console no longer lets us type.
static void runConsole(const char* text) {
    void* cs = reinterpret_cast<void*>(g_base + rva_cmdSystem);
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(cs);
    if (vt < g_base || vt >= g_exeEnd) {
        logf("[ewcoop] cmdSystem looks wrong (vtable %p); not running '%s'\n", reinterpret_cast<void*>(vt), text);
        return;
    }
    logf("[ewcoop] console: %s\n", text);
    fn<ExecuteString_t>(rva_cmdExecuteString)(cs, text, 1);
}
// Dump every registered console command (name + description) by walking the command list the executor uses
// (cmdSystem+8). Reveals MP commands (session/lobby/bot) the retail console doesn't advertise.
static bool readable(const void* p, size_t n);  // defined below
static Vec3 originOf(void* ent);                  // defined below

// Host announcement: print net_port (the direct backend's UDP port) and the machine's LAN IPv4 addresses,
// so the user has everything to hand the friend a `connect <ip>[:port]` address.
static void hostInfo() {
    const char* port = "27015";
    auto vp = reinterpret_cast<char**>(g_base + rva_cvarNetPort + off_cvar_valueString);
    if (readable(vp, 8) && *vp) port = *vp;
    const char* bindIp = "localhost";
    auto bi = reinterpret_cast<char**>(g_base + rva_netIpString);
    if (readable(bi, 8) && *bi) bindIp = *bi;
    logf("[ewcoop] ========== HOST INFO (give this to your friend) ==========\n");
    logf("[ewcoop] UDP port = %s  (net_port) | bind address (net_ip) = '%s'%s\n", port, bindIp,
         strcmp(bindIp, "localhost") ? " (specific NIC!)" : " (all interfaces -> virtual LAN OK)");
    char buf[65536];
    ULONG size = sizeof buf;
    if (GetAdaptersInfo(reinterpret_cast<IP_ADAPTER_INFO*>(buf), &size) == NO_ERROR) {
        for (auto* a = reinterpret_cast<IP_ADAPTER_INFO*>(buf); a; a = a->Next) {
            if (a->Type == MIB_IF_TYPE_LOOPBACK) continue;
            for (auto* ip = &a->IpAddressList; ip; ip = ip->Next)
                if (ip->IpAddress.String[0] && strcmp(ip->IpAddress.String, "0.0.0.0"))
                    logf("[ewcoop] LAN IP %-15s  -> same network: connect %s\n",
                         ip->IpAddress.String, ip->IpAddress.String);
        }
    } else {
        logf("[ewcoop] (could not enumerate adapters)\n");
    }
    logf("[ewcoop] LAN: friend presses Ctrl+Shift+J with EWCOOP_HOST=<LAN IP>\n");
    logf("[ewcoop] remote: port-forward UDP %s on the router, friend does connect <your-public-ip>\n", port);
    logf("[ewcoop] host flow: Ctrl+Shift+F12 (create) then Ctrl+Shift+S (start)\n");
    logf("[ewcoop] firewall: allow EvilWithin.exe inbound UDP %s (Radmin/ZeroTier nets count as Public)\n", port);
    logf("[ewcoop] ==========================================================\n");
}

// cmdSystem is frequently dead (vtable 0) so runConsole can't change cvars. Write net_usePlatformBackend's
// int value directly so the DIRECT UDP backend is selected -- a Steam-backend host can't be reached by the
// friend's direct `connect <ip>`.
static void forceDirectBackend() {
    if (!readable(reinterpret_cast<void*>(g_base + rva_cvarNetUsePlatformBackend + off_cvar_valueInt), 4)) return;
    int* v = reinterpret_cast<int*>(g_base + rva_cvarNetUsePlatformBackend + off_cvar_valueInt);
    if (*v != 0) {
        logf("[ewcoop] net_usePlatformBackend %d -> 0 (direct backend)\n", *v);
        *v = 0;
    }
}

// Diagnostic: dump the MP session object and locate CreatePartyLobby in its vtable (no calls).
static void mpDiag() {
    void* session = reinterpret_cast<void*>(g_base + rva_session);
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(session);
    logf("[ewcoop] session obj %p vtable %p (rva %#llx)\n", session, reinterpret_cast<void*>(vt),
         (unsigned long long)(vt - g_base));
    if (vt < g_base || vt >= g_exeEnd) { logf("[ewcoop] session vtable not in exe; wrong object?\n"); return; }
    auto slots = reinterpret_cast<void**>(vt);
    int found = -1;
    for (int i = 0; i < 400 && readable(slots + i, 8); ++i)
        if (slots[i] == reinterpret_cast<void*>(g_base + rva_CreatePartyLobby)) { found = i; break; }
    logf("[ewcoop] CreatePartyLobby (rva %#llx) in session vtable slot: %d\n",
         (unsigned long long)rva_CreatePartyLobby, found);
    logf("[ewcoop] session state=%d, lobbyStr1='%s' lobbyStr2='%s'\n",
         *reinterpret_cast<int*>(static_cast<char*>(session) + 40),
         *reinterpret_cast<char**>(g_base + rva_cvarLobbyStr1), *reinterpret_cast<char**>(g_base + rva_cvarLobbyStr2));
    // party lobby member list: ptr at +0x4D0, count at +0x4D8, 496-byte entries, entry[0] = state (2 = joined)
    char* pl = static_cast<char*>(session) + off_session_partyLobby;
    void* members = *reinterpret_cast<void**>(pl + 0x4D0);
    int mcount = *reinterpret_cast<int*>(pl + 0x4D8);
    void* users = *reinterpret_cast<void**>(pl + 984);
    int ucount = *reinterpret_cast<int*>(pl + 992);
    logf("[ewcoop] party lobby %p: users(ptr %p cnt %d) members(ptr %p cnt %d)\n", pl, users, ucount, members, mcount);
    void* backend = *reinterpret_cast<void**>(pl + 72);
    void* user0 = (ucount > 0 && users) ? *reinterpret_cast<void**>(users) : nullptr;
    logf("[ewcoop] party lobby backend(+72)=%p user0=%p\n", backend, user0);
    if (backend) {
        void** bvt = *reinterpret_cast<void***>(backend);
        uintptr_t brr = reinterpret_cast<uintptr_t>(bvt);
        logf("[ewcoop] backend vtable=%p (rva %#llx)%s\n", bvt,
             brr >= g_base ? (unsigned long long)(brr - g_base) : 0ull, brr >= g_base ? "" : " OUTSIDE");
        if (brr >= g_base && readable(bvt + 25, 8)) {
            uintptr_t f = reinterpret_cast<uintptr_t>(bvt[25]);
            logf("[ewcoop] backend vtbl[25] (addUser) = %p (rva %#llx)\n", bvt[25],
                 f >= g_base ? (unsigned long long)(f - g_base) : 0ull);
        }
    }
    if (members && mcount > 0 && readable(members, 4))
        logf("[ewcoop]   member[0] state=%d\n", *reinterpret_cast<int*>(members));
    logf("[ewcoop] session +40=%d +48=%d | backend state(+8)=%d\n",
         *reinterpret_cast<int*>(static_cast<char*>(session) + 40),
         *reinterpret_cast<int*>(static_cast<char*>(session) + 48),
         backend ? *reinterpret_cast<int*>(static_cast<char*>(backend) + 8) : -1);
    char* gl = static_cast<char*>(session) + 5760;  // game lobby
    void* gmem = *reinterpret_cast<void**>(gl + 0x4D0);
    int gcnt = *reinterpret_cast<int*>(gl + 0x4D8);
    logf("[ewcoop] game lobby %p: members(ptr %p cnt %d) idx(+36)=%d state\n", gl, gmem, gcnt,
         *reinterpret_cast<int*>(gl + 36));
    // level-load data set by SetLevelLoadData (0x324780): buffer at +5656, len at +5664
    void* ld = *reinterpret_cast<void**>(gl + 5656);
    int ldl = *reinterpret_cast<int*>(gl + 5664);
    logf("[ewcoop] game lobby levelData buf=%p len=%d '%.48s'\n", ld, ldl,
         (ld && ldl > 0 && readable(ld, 1)) ? static_cast<const char*>(ld) : "");
}

// Periodic net status: how far a join/host got. Reads the session state and the party/game lobby peer
// lists (idStaticList<peer_t> at lobby+0x4D0: ptr, count at +0x4D8, entries stride 496, +0 connectionState,
// 2 = ESTABLISHED). Enabled by Ctrl+Shift+N (or automatically for ~30s after a client join).
static bool g_netWatch;
static bool g_captureNet;                 // install the engine print listener + net_verbose (opt-in)
static bool g_printListenerOn;            // listener installed once
static float g_netTimer;
static void netStatus() {
    void* session = reinterpret_cast<void*>(g_base + rva_session);
    if (!readable(session, 8)) return;
    int state = *reinterpret_cast<int*>(static_cast<char*>(session) + 40);
    char* pl = static_cast<char*>(session) + off_session_partyLobby;
    char* gl = static_cast<char*>(session) + 5760;
    void* peers = *reinterpret_cast<void**>(pl + 0x4D0);
    int pcount = *reinterpret_cast<int*>(pl + 0x4D8);
    void* gpeers = *reinterpret_cast<void**>(gl + 0x4D0);
    int gcount = *reinterpret_cast<int*>(gl + 0x4D8);
    logf("[ewcoop] net: session state=%d | party peers=%d game peers=%d | P1=%d P2=%d\n", state, pcount, gcount,
         g_player1 ? 1 : 0, g_player2 ? 1 : 0);
    if (peers && pcount > 0 && readable(peers, 16)) {
        for (int i = 0; i < pcount && i < 8; ++i)
            logf("[ewcoop] net:   party peer[%d] connectionState=%d\n", i,
                 *reinterpret_cast<int*>(static_cast<char*>(peers) + i * 496));
    }
    if (gpeers && gcount > 0 && readable(gpeers, 16)) {
        for (int i = 0; i < gcount && i < 8; ++i)
            logf("[ewcoop] net:   game peer[%d] connectionState=%d\n", i,
                 *reinterpret_cast<int*>(static_cast<char*>(gpeers) + i * 496));
    }
}

// ---------------- capture the engine's own console/NET output into our log ----------------
// The engine's print impl (sub_CC1680) walks a listener list (head 0x9AFB410) calling listener->vtbl[1]
// (listener, str) for each formatted message. We insert our own listener so the NET handshake (enabled by
// net_verbose) lands in ewcoop_<pid>.log -- no need to watch the in-game console or a debug viewer.
struct PrintListener {
    void** vtbl;               // +0
    PrintListener* next;       // +8
    unsigned char pass1;       // +16
    unsigned char alwaysPass;  // +17
    unsigned short pad;        // +18
    int minLevel;              // +20
};
static void listenerNoop(void*) {}
static void listenerPrint(void* self, const char* s) {
    (void)self;
    if (!s || !*s) return;
    if (strstr(s, "NET") || strstr(s, "Lobby") || strstr(s, "lobby") || strstr(s, "peer") ||
        strstr(s, "Peer") || strstr(s, "ession") || strstr(s, "napshot") || strstr(s, "sercmd") ||
        strstr(s, "socket") || strstr(s, "onnect") || strstr(s, "Hosting") || strstr(s, "SendMembers"))
        logf("[game] %s", s);
}
static void* g_printVtbl[2] = {reinterpret_cast<void*>(&listenerNoop), reinterpret_cast<void*>(&listenerPrint)};
static PrintListener g_listener = {g_printVtbl, nullptr, 1, 1, 0, 0};
static void installPrintListener() {
    fn<void (*)(void*)>(rva_PrintListenerAdd)(&g_listener);
    logf("[ewcoop] print listener installed (engine NET/Lobby/peer lines -> this log)\n");
}

// ---------------- on-screen debug HUD (GDI layered overlay, topmost + click-through) ----------------
// A WS_POPUP layered window (black = transparent) parked at the game client's top-left, drawn from a
// WM_TIMER the game's own message loop dispatches. Shows session state + peer counts live so a join can
// be watched in-game. Works when TEW runs windowed/borderless (an exclusive-fullscreen game can't be
// overdrawn). Ctrl+H toggles; text is refreshed each frame by the game thread (updateHud).
static HWND g_hudWnd, g_gameWnd;
static bool g_hudOn = false;  // off by default so injecting is inert; Ctrl+H shows it
static char g_hudText[768];
static bool g_netHavePeer, g_remoteValid, g_bot;  // netcode/bot state (defined in the net section below)

// ---- render-world capture -> world->screen (native-style world-anchored HUD) ----
extern "C" void* g_renderWorld = nullptr;  // set by the idRenderWorldLocal::Render hook
extern "C" void* g_renderWorldTramp = nullptr;
static bool g_renderHookOn;
static const unsigned char kRenderPrologue[12] = {0x48, 0x8B, 0xC4, 0x44, 0x89, 0x40, 0x18,
                                                  0x89, 0x50, 0x10, 0x55, 0x53};
// Naked tail-jmp hook: record the render-world `this` (rcx) then jump to the trampoline WITHOUT pushing a
// return address -- the prologue writes args into the entry stack frame, so rsp must be untouched.
__attribute__((naked)) static void hkRenderWorldRender() {
    __asm__ __volatile__("movq %rcx, g_renderWorld(%rip)\n\t"
                         "movq g_renderWorldTramp(%rip), %rax\n\t"
                         "jmp *%rax\n\t");
}
static bool worldToScreen(const Vec3& w, float* sx, float* sy) {
    if (!g_renderWorld || !readable(static_cast<char*>(g_renderWorld) + off_render_MVP, 64)) return false;
    const float* m = reinterpret_cast<const float*>(static_cast<char*>(g_renderWorld) + off_render_MVP);
    float cx = w.x * m[0] + w.y * m[4] + w.z * m[8] + m[12];
    float cy = w.x * m[1] + w.y * m[5] + w.z * m[9] + m[13];
    float cw = w.x * m[3] + w.y * m[7] + w.z * m[11] + m[15];
    if (cw < 0.001f) return false;
    int vw = *reinterpret_cast<int16_t*>(static_cast<char*>(g_renderWorld) + off_render_viewportW);
    int vh = *reinterpret_cast<int16_t*>(static_cast<char*>(g_renderWorld) + off_render_viewportH);
    if (vw <= 0 || vh <= 0) return false;
    *sx = (cx / cw * 0.5f + 0.5f) * (float)vw;
    *sy = (1.0f - (cy / cw * 0.5f + 0.5f)) * (float)vh;
    return true;
}
// tag state shared from the game thread to the overlay's WM_TIMER draw
static volatile int g_tagX, g_tagY, g_tagHp;
static volatile bool g_tagVisible;
static char g_tagName[32];
static DWORD g_ourPid;

static const char* stateName(int s) {
    switch (s) {
        case 0: return "PRESS_START";        case 1: return "IDLE";
        case 2: return "PARTY_LOBBY_HOST";   case 3: return "PARTY_LOBBY_PEER";
        case 4: return "GAME_LOBBY_HOST";    case 5: return "GAME_LOBBY_PEER";
        case 6: return "GAMESTATE_LOBBY_HOST"; case 7: return "GAMESTATE_LOBBY_PEER";
        case 8: return "CREATE_PARTY";       case 9: return "CREATE_GAMELOBBY";
        case 10: return "CREATE_GAMESTATELOBBY"; case 11: return "FIND_OR_CREATE";
        case 12: return "CONNECT_PARTY";     case 13: return "CONNECT_GAMELOBBY";
        case 14: return "CONNECT_GAMESTATELOBBY"; case 15: return "BUSY";
        case 16: return "LOADING";           case 17: return "INGAME";
        default: return "?";
    }
}

static BOOL CALLBACK hudEnumProc(HWND h, LPARAM out) {
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    if (wpid != g_ourPid || h == g_hudWnd || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r;
    GetClientRect(h, &r);
    if (r.right - r.left >= 300 && r.bottom - r.top >= 250) { *reinterpret_cast<HWND*>(out) = h; return FALSE; }
    return TRUE;
}
static HWND findGameWindow() {
    g_ourPid = GetCurrentProcessId();
    HWND out = nullptr;
    EnumWindows(hudEnumProc, reinterpret_cast<LPARAM>(&out));
    return out;
}

// ---- keep running when unfocused ----
// The game window proc (0x3557E0) treats WM_ACTIVATE(inactive) / WM_KILLFOCUS as "app inactive"
// (byte_9A52D20 = 0) and opens the pause menu. While a net session runs we swallow those so the other copy
// (or a friend's game in the background) keeps simulating and sending.
static WNDPROC g_origWndProc;
static HWND g_subclassedWnd;
static bool g_keepRunning;  // set while a net session is active
static LRESULT CALLBACK keepRunningProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_keepRunning && !g_unhook) {
        if (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) return 0;
        if (m == WM_KILLFOCUS) return 0;
        if (m == WM_ACTIVATEAPP && !w) return 0;
    }
    return CallWindowProcA(g_origWndProc, h, m, w, l);
}
static void installKeepRunning() {
    if (g_subclassedWnd) return;
    HWND w = findGameWindow();
    if (!w) return;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&keepRunningProc)));
    if (g_origWndProc) {
        g_subclassedWnd = w;
        logf("[ewcoop] window %p subclassed: no pause on focus loss during net sessions\n", w);
    }
}

// ---- block keyboard/mouse in the unfocused copy ----
// The game reads keyboard/mouse with DirectInput in foreground mode (keyboard coop 0x16, mouse 5; 0x343FE0 /
// 0x3447B0). Foreground devices are released when the window deactivates, but we swallow those messages
// (no pause), so the background copy kept getting keys. All DirectInput devices share dinput8.dll's device
// vtable, so we hook GetDeviceState (slot 9) / GetDeviceData (slot 10) there and hand the game "nothing
// pressed / no events" while a net session runs and this copy isn't focused.
typedef HRESULT(__stdcall* DIGetState_t)(void* self, DWORD cb, void* data);
typedef HRESULT(__stdcall* DIGetData_t)(void* self, DWORD cbObj, void* rgdod, DWORD* inOut, DWORD flags);
static DIGetState_t g_origDIGetState;
static DIGetData_t g_origDIGetData;
static bool blockInput() { return g_keepRunning && !g_unhook && !weAreFocused(); }
static HRESULT __stdcall hkDIGetState(void* self, DWORD cb, void* data) {
    HRESULT hr = g_origDIGetState(self, cb, data);
    if (blockInput() && data) memset(data, 0, cb);
    return hr;
}
static HRESULT __stdcall hkDIGetData(void* self, DWORD cbObj, void* rgdod, DWORD* inOut, DWORD flags) {
    HRESULT hr = g_origDIGetData(self, cbObj, rgdod, inOut, flags);
    if (blockInput() && inOut) *inOut = 0;  // drained, but nothing delivered
    return hr;
}
static void hookDirectInput() {
    IDirectInput8A* di = nullptr;
    if (FAILED(DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                                  reinterpret_cast<void**>(&di), nullptr)) || !di) {
        logf("[ewcoop] DirectInput hook: DirectInput8Create failed\n");
        return;
    }
    IDirectInputDevice8A* dev = nullptr;
    if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) && dev) {
        void** vt = *reinterpret_cast<void***>(dev);
        // Chain to whatever is there now (the real function, or an older ewcoop's hook).
        g_origDIGetState = reinterpret_cast<DIGetState_t>(vt[9]);
        g_origDIGetData = reinterpret_cast<DIGetData_t>(vt[10]);
        writeSlot(&vt[9], reinterpret_cast<void*>(&hkDIGetState));
        writeSlot(&vt[10], reinterpret_cast<void*>(&hkDIGetData));
        logf("[ewcoop] DirectInput hooked: unfocused copy gets no keyboard/mouse during net sessions\n");
        dev->Release();
    }
    di->Release();
}

static LRESULT CALLBACK hudProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m != WM_TIMER && m != WM_PAINT) return DefWindowProcA(h, m, w, l);
    if (!g_hudOn) { ShowWindow(h, SW_HIDE); return 0; }
    if (!g_gameWnd || !IsWindow(g_gameWnd)) g_gameWnd = findGameWindow();
    int cw = 640, ch = 480;
    if (g_gameWnd) {
        RECT cr;
        POINT pt{0, 0};
        GetClientRect(g_gameWnd, &cr);
        ClientToScreen(g_gameWnd, &pt);
        cw = cr.right - cr.left; ch = cr.bottom - cr.top;
        if (cw < 16 || ch < 16) { cw = 640; ch = 480; }
        SetWindowPos(h, HWND_TOPMOST, pt.x, pt.y, cw, ch, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    HDC dc = GetDC(h);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, cw, ch);
    HGDIOBJ oldbmp = SelectObject(mem, bmp);
    RECT r{0, 0, cw, ch};
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));  // black -> transparent (colorkey)
    FillRect(mem, &r, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    HFONT f = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH, "Consolas");
    HGDIOBJ oldf = SelectObject(mem, f);
    SetTextColor(mem, RGB(0, 255, 80));
    RECT cr2{8, 8, 8 + 620, 8 + 150};
    DrawTextA(mem, g_hudText, -1, &cr2, DT_LEFT | DT_TOP | DT_NOPREFIX);
    if (g_tagVisible) {  // world-anchored nameplate over the remote player
        SetTextColor(mem, RGB(255, 255, 255));
        RECT tr{g_tagX - 70, g_tagY - 34, g_tagX + 70, g_tagY - 34 + 20};
        DrawTextA(mem, g_tagName, -1, &tr, DT_CENTER | DT_TOP | DT_NOPREFIX);
        int bx = g_tagX - 26, by = g_tagY - 12, bw = 52, bh = 5;
        RECT bgr{bx, by, bx + bw, by + bh};
        HBRUSH eb = CreateSolidBrush(RGB(30, 30, 30));
        FillRect(mem, &bgr, eb);
        DeleteObject(eb);
        int fill = bw * g_tagHp / 100;
        if (fill < 0) fill = 0; if (fill > bw) fill = bw;
        RECT fgr{bx, by, bx + fill, by + bh};
        HBRUSH fb = CreateSolidBrush(RGB(235, 235, 235));
        FillRect(mem, &fgr, fb);
        DeleteObject(fb);
    }
    SelectObject(mem, oldf);
    DeleteObject(f);
    BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(h, dc);
    return 0;
}

static void ensureHud() {
    if (g_hudWnd) return;
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = hudProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ewcoopHud";
    RegisterClassExA(&wc);
    g_hudWnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                               "ewcoopHud", "ewcoop", WS_POPUP, 0, 0, 620, 150, nullptr, nullptr,
                               wc.hInstance, nullptr);
    if (g_hudWnd) {
        SetLayeredWindowAttributes(g_hudWnd, RGB(0, 0, 0), 0, LWA_COLORKEY);  // black -> transparent
        SetTimer(g_hudWnd, 1, 100, nullptr);
    }
}

// Fill the HUD text from the live session (game thread).
static void updateHud() {
    void* session = reinterpret_cast<void*>(g_base + rva_session);
    if (!readable(session, 8)) { snprintf(g_hudText, sizeof g_hudText, "ewcoop  (no session yet)"); return; }
    int state = *reinterpret_cast<int*>(static_cast<char*>(session) + 40);
    char* pl = static_cast<char*>(session) + off_session_partyLobby;
    char* gl = static_cast<char*>(session) + 5760;
    int pcount = *reinterpret_cast<int*>(pl + 0x4D8);
    int gcount = *reinterpret_cast<int*>(gl + 0x4D8);
    void* peers = *reinterpret_cast<void**>(pl + 0x4D0);
    void* gpeers = *reinterpret_cast<void**>(gl + 0x4D0);
    int pc0 = (peers && pcount > 0 && readable(peers, 4)) ? *reinterpret_cast<int*>(peers) : -1;
    int gc0 = (gpeers && gcount > 0 && readable(gpeers, 4)) ? *reinterpret_cast<int*>(gpeers) : -1;
    const char* port = "?";
    auto vp = reinterpret_cast<char**>(g_base + rva_cvarNetPort + off_cvar_valueString);
    if (readable(vp, 8) && *vp) port = *vp;
    // Remote/bot player name + health (idPlayer::GetHealth = vtable slot 88).
    const char* p2name = g_bot ? "Bot01" : "Player 2";
    float hp = -1.f;
    if (g_player2 && isA(g_player2, rva_idPlayer_vtable)) hp = vfn<float (*)(void*)>(g_player2, 88)(g_player2);
    int hpi = (hp < 0.f) ? -1 : (hp > 100.f ? 100 : (int)hp);
    // World-anchored nameplate: project the remote player's head to the screen.
    g_tagVisible = false;
    if (g_player2 && isA(g_player2, rva_idPlayer_vtable) && g_renderWorld) {
        Vec3 head = originOf(g_player2);
        head.z += 70.f;
        float sx, sy;
        if (worldToScreen(head, &sx, &sy) && sx > 0 && sy > 0) {
            g_tagX = (int)sx;
            g_tagY = (int)sy;
            g_tagVisible = true;
            strncpy(g_tagName, p2name, sizeof g_tagName - 1);
            g_tagName[sizeof g_tagName - 1] = 0;
            g_tagHp = (hpi < 0) ? 100 : hpi;
        }
    }
    char bar[12] = "..........";
    for (int i = 0; i < 10 && hpi > 0; ++i) bar[i] = (i * 10 < hpi) ? '#' : '.';
    snprintf(g_hudText, sizeof g_hudText,
             "ewcoop   net_port %s   [Ctrl+H hide]\n"
             "session: %s (%d)\n"
             "net: peer=%s remote=%s   bot=%s\n"
             "P1: %s\n"
             "%s: [%s] %d%%",
             port, stateName(state), state, g_netHavePeer ? "y" : "n", g_remoteValid ? "y" : "n", g_bot ? "y" : "n",
             g_player1 ? "ok" : "--", p2name, bar, hpi);
}

// idSessionLocal::StartMatch - what the "Start Game" button calls. Requires STATE_GAME_LOBBY_HOST (4).
// The front-end also passes a level-load blob (used to pick the map's vtexture); we send the map name.
static const char* g_mpMap = "root/chapter/an_emergency_call";
static void mpStartMatch() {
    void* session = reinterpret_cast<void*>(g_base + rva_session);
    int state = *reinterpret_cast<int*>(static_cast<char*>(session) + 40);
    logf("[ewcoop] StartMatch: session state=%d (need 4), map='%s'\n", state, g_mpMap);
    fn<void (*)(void*, void*, int)>(rva_StartMatch)(session, const_cast<char*>(g_mpMap),
                                                    static_cast<int>(strlen(g_mpMap) + 1));
    logf("[ewcoop] StartMatch returned, state now=%d\n", *reinterpret_cast<int*>(static_cast<char*>(session) + 40));
    char* gl = static_cast<char*>(session) + 5760;
    void* ld = *reinterpret_cast<void**>(gl + 5656);
    int ldl = *reinterpret_cast<int*>(gl + 5664);
    logf("[ewcoop] post-StartMatch game lobby levelData buf=%p len=%d '%.64s'\n", ld, ldl,
         (ld && readable(ld, 1)) ? static_cast<const char*>(ld) : "");
}

// Force idGameLocal::LoadMap for our map by setting its map idStr (via idStr::operator=, which heap-allocates),
// then calling LoadMap. gameLocal's map name uses the "maps/" form (e.g. 'maps/root/title/title').
static void mpLoadMap() {
    void* game = *reinterpret_cast<void**>(g_base + rva_gameLocal);
    if (!game) { logf("[ewcoop] LoadMap: no gameLocal\n"); return; }
    static const char* glMap = "maps/root/chapter/an_emergency_call";
    char* mi = static_cast<char*>(game) + off_game_mapIdStr;
    char* curptr = *reinterpret_cast<char**>(mi + 8);
    logf("[ewcoop] gameLocal map idStr before: len=%d ptr=%p '%s'\n", *reinterpret_cast<int*>(mi), curptr,
         (curptr && readable(curptr, 1)) ? curptr : "");
    struct IdStrSrc { int len; int pad; const char* data; };
    IdStrSrc src{ static_cast<int>(strlen(glMap)), 0, glMap };
    fn<void (*)(void*, void*)>(0x11D0)(mi, &src);  // idStr::operator= (heap-allocs the dest)
    logf("[ewcoop] set gameLocal map -> '%s', calling LoadMap\n", glMap);
    fn<void (*)(void*)>(rva_LoadMap)(game);
    logf("[ewcoop] LoadMap returned\n");
}

// Experiment: try to force the local user into the party lobby's member list. sub_345900 (backend addUser)
// only does its real work when the backend is in hosting state (+8 == 1); our hand-driven flow leaves it 0, so
// the user is queued but never becomes a member. Set +8 = 1 and re-run the notify (sub_32B880) and watch.
static void forceMember() {
    void* session = reinterpret_cast<void*>(g_base + rva_session);
    char* pl = static_cast<char*>(session) + off_session_partyLobby;
    void* backend = *reinterpret_cast<void**>(pl + 72);
    void* users = *reinterpret_cast<void**>(pl + 984);
    int ucount = *reinterpret_cast<int*>(pl + 992);
    void* user0 = (ucount > 0 && users) ? *reinterpret_cast<void**>(users) : nullptr;
    logf("[ewcoop] forceMember: backend=%p user0=%p partyMembers=%d\n", backend, user0,
         *reinterpret_cast<int*>(pl + 0x4D8));
    if (!backend || !user0) { logf("[ewcoop] forceMember: missing backend/user\n"); return; }
    int before = *reinterpret_cast<int*>(static_cast<char*>(backend) + 8);
    *reinterpret_cast<int*>(static_cast<char*>(backend) + 8) = 1;
    fn<void (*)(void*, void*)>(0x32B880)(pl, user0);
    logf("[ewcoop] forceMember: backend+8 %d -> 1; partyMembers now %d\n", before,
         *reinterpret_cast<int*>(pl + 0x4D8));
}

// Kick off the multiplayer party lobby: build the lobby params then call idSessionLocal::CreatePartyLobby
// directly (session vtable slot 10). That sets the session to the create-lobby state; the state machine then
// drives WaitOnLobbyCreate -> backend creation -> AddLocalUsersToLobby, which is what startServer's member
// send needs. (Going via CreateMatch was ambiguous about which branch it took.)
static void createPartyLobby() {
    void* session = reinterpret_cast<void*>(g_base + rva_session);
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(session);
    if (vt < g_base || vt >= g_exeEnd) { logf("[ewcoop] session vtable bad; not calling CreatePartyLobby\n"); return; }
    // The friend reaches us with `connect <ip>` (direct backend); the Steam backend would make us
    // unreachable without the removed front-end. Set it before the session creates the lobby/backend.
    runConsole("net_usePlatformBackend 0");
    forceDirectBackend();  // cmdSystem may be dead; set the backend int directly too
    g_captureNet = true;  // host: capture the engine NET trace for the join
    alignas(16) unsigned char params[4096] = {};
    const char* s1 = *reinterpret_cast<const char**>(g_base + rva_cvarLobbyStr1);
    const char* s2 = *reinterpret_cast<const char**>(g_base + rva_cvarLobbyStr2);
    int c = *reinterpret_cast<int*>(g_base + rva_lobbyParamConst);
    if (!s1 || !readable(s1, 1)) s1 = "";
    if (!s2 || !readable(s2, 1)) s2 = "";
    const char* map = g_mpMap;
    // matchFlags = DefaultPartyFlags | DefaultPrivateGameFlags
    //   = MATCH_ONLINE(0x2) | MATCH_PRIVATE(0x8) | MATCH_REQUIRE_PARTY_LOBBY(0x20) | MATCH_JOIN_IN_PROGRESS(0x80)
    // This is what the game's own front-end passes for a joinable online (private) game, so common->IsMultiplayer()
    // is true AND a client may join after the host has started. (The old 0x42 had ONLY ONLINE + the party-
    // placeholder bit and lacked join-in-progress.)
    const int kMatchFlags = 0xAA;
    logf("[ewcoop] building lobby params (matchFlags %#x, map='%s') s1='%s' s2='%s' c=%d\n", kMatchFlags, map, s1, s2, c);
    // a8/a9/a10 must be idStr (idStr::operator= reads [0]=len, [8]=data), not char*
    struct IdStrSrc { int len; int pad; const char* data; };
    IdStrSrc m{(int)strlen(map), 0, map}, a{(int)strlen(s1), 0, s1}, b{(int)strlen(s2), 0, s2};
    fn<void (*)(void*, int, int, int, int, int, int, void*, void*, void*, int, int, char, char)>(
        rva_InitLobbyParams)(params, 0, -1, -2, 0, 16, kMatchFlags, &m, &a, &b, 0, c, 0, 0);  // gameMap = GAME_MAP_SINGLEPLAYER (-2)
    logf("[ewcoop] calling CreatePartyLobby(session=%p, params=%p)\n", session, params);
    fn<void (*)(void*, void*)>(rva_CreatePartyLobby)(session, params);
    logf("[ewcoop] CreatePartyLobby returned (state=%d)\n", *reinterpret_cast<int*>(static_cast<char*>(session) + 40));
    fn<void (*)(void*, void*)>(rva_CreateMatch)(session, params);  // game lobby, same SP match parms (gameMap=-2)
    logf("[ewcoop] CreateMatch returned (state=%d)\n", *reinterpret_cast<int*>(static_cast<char*>(session) + 40));
    fn<void (*)(void*)>(rva_DestroyLobbyParams)(params);
}

static void dumpCommands() {
    char* n = *reinterpret_cast<char**>(g_base + rva_cmdList);
    int count = 0;
    for (; n && count < 5000; ++count) {
        if (!readable(n, 32)) { logf("[ewcoop] command list broke at %p\n", n); break; }
        const char* name = *reinterpret_cast<const char**>(n + 8);
        const char* desc = *reinterpret_cast<const char**>(n + 24);
        if (name && *name) logf("[ewcoop] cmd: %s%s%s\n", name, (desc && *desc) ? " - " : "", (desc && *desc) ? desc : "");
        n = *reinterpret_cast<char**>(n);
    }
    logf("[ewcoop] %d commands\n", count);
}
static void runConsoleFile() {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\ewcmd.txt", g_dir);
    FILE* f = fopen(path, "rb");
    if (!f) { logf("[ewcoop] no %s to run\n", path); return; }
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    for (char* p = buf; p && *p;) {
        char* semi = strchr(p, ';');
        if (semi) *semi = 0;
        while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') ++p;
        char* end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\r' || end[-1] == '\n' || end[-1] == '\t')) *--end = 0;
        if (*p) runConsole(p);
        p = semi ? semi + 1 : nullptr;
    }
}
static void* physicsOf(void* ent) { return field<void*>(ent, off_entity_physics); }
static Vec3 originOf(void* ent) {
    void* phys = physicsOf(ent);
    return *vfn<GetVec_t>(phys, slot_phys_GetOrigin)(phys, 0);
}
static float yawOf(void* ent) {
    void* phys = physicsOf(ent);
    const Mat3* ax = vfn<GetAxis_t>(phys, slot_phys_GetAxis)(phys, 0);
    return std::atan2(ax->rows[0].y, ax->rows[0].x) * 57.2957795f;
}
static void teleport(void* partner, const Vec3& o, float yaw, bool toIdle) {
    Angles a{0.f, yaw, 0.f};
    vfn<Teleport_t>(partner, slot_partner_Teleport)(partner, &o, &a, toIdle, 0);
}
// Turn the body without moving it: the same two calls Teleport (0x9C15A0) makes for the axis.
static void face(void* partner, float yaw) {
    Angles a{0.f, yaw, 0.f};
    alignas(16) unsigned char mat[48], quat[16];
    void* phys = physicsOf(partner);
    vfn<SetAxis_t>(phys, slot_phys_SetAxis)(phys, fn<AnglesTo_t>(rva_AnglesToMat3)(&a, mat), -1);
    fn<SetFacing_t>(rva_SetFacing)(field<void*>(partner, off_partner_facing),
                                   fn<AnglesTo_t>(rva_AnglesToQuat)(&a, quat));
}
// Move the body without Teleport's animation reset: just the two position calls it makes.
static void place(void* partner, const Vec3& o) {
    void* phys = physicsOf(partner);
    vfn<SetOrigin_t>(phys, slot_phys_SetOrigin)(phys, &o, -1);
    fn<SetFacingOrigin_t>(rva_SetFacingOrigin)(field<void*>(partner, off_partner_facing), &o);
}
static float angleDiff(float a, float b) {  // shortest signed a - b in degrees
    float d = std::fmod(a - b + 540.f, 360.f) - 180.f;
    return d;
}
static void* morpheme(void* partner) { return static_cast<char*>(partner) + off_partner_morpheme; }
static void setMoveSpeed(void* partner, float v) {
    fn<SetFloatParam_t>(rva_SetFloatParam)(morpheme(partner), field<uint16_t>(partner, off_partner_MoveSpeed), v);
}
static bool request(void* partner, uintptr_t idOffset) {
    return fn<SendRequest_t>(rva_SendRequest)(morpheme(partner), field<int>(partner, idOffset), 1);
}

static void* spawnJoseph(void* gameLocal) {
    auto find = fn<FindEntityDef_t>(rva_FindEntityDef);
    void* decl = find(reinterpret_cast<void*>(g_base + rva_entityDefList), "ai/partner/joseph", 0, 0, 0, 0, 0, 0, 0);
    logf("[ewcoop] entityDef ai/partner/joseph = %p\n", decl);
    if (!decl) return nullptr;
    void* ent = vfn<SpawnEntityDef_t>(gameLocal, slot_SpawnEntityDef)(gameLocal, decl, -1);
    logf("[ewcoop] spawned entity %p (is idPartner01: %d)\n", ent, isA(ent, rva_idPartner01_vtable));
    return isA(ent, rva_idPartner01_vtable) ? ent : nullptr;
}

// The player's main model instance (idTreeAnimator) = 0x85E5E0(*(player+0x2A8)->vfunc[6]()); null if unset.
static void* mainModelInstance(void* player) {
    void* anim = field<void*>(player, off_entity_anim);
    if (!anim) return nullptr;
    void* model = vfn<void* (*)(void*)>(anim, 48 / 8)(anim);
    return model ? fn<ModelInstance_t>(rva_ModelInstance)(model) : nullptr;
}

// True if [p, p+n) is committed, readable memory (guards the diagnostic against bad pointers).
static bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// How many mesh components of a model instance are currently shown (sum of the +2560 visibility array).
static int visibleMeshCount(void* inst) {
    if (!inst || !readable(static_cast<char*>(inst) + off_model_Md6Decl, 8)) return -1;
    char* decl = field<char*>(inst, off_model_Md6Decl);
    if (!readable(decl, 400) || !readable(static_cast<char*>(inst) + off_model_meshVisible, 8)) return -1;
    char* vis = field<char*>(inst, off_model_meshVisible);
    char* skel = field<char*>(decl, 112);
    if (!readable(skel, 220)) return -1;
    int count = field<uint16_t>(skel, 216);
    if (count <= 0 || count > 4096 || !readable(vis, count)) return -1;
    int shown = 0;
    for (int i = 0; i < count; ++i) shown += vis[i] != 0;
    return shown;
}

// Log the meshkit entry names baked into the model's Md6Decl (heads + gear) with each entry's mesh-component
// count. This shows exactly which body parts "normal"/"headless"/"bodyless" cover and what is left over.
static void dumpMeshKits(void* player) {
    void* inst = mainModelInstance(player);
    char* decl = (inst && readable(static_cast<char*>(inst) + off_model_Md6Decl, 8))
                     ? field<char*>(inst, off_model_Md6Decl) : nullptr;
    logf("[ewcoop] meshkit dump: player %p, instance %p, Md6Decl %p\n", player, inst, decl);
    if (!readable(decl, 400)) return;
    for (int k = 0; k < 2; ++k) {
        const char* kit = k == 0 ? "heads" : "gear";
        int count = field<uint16_t>(decl, 16 * k + 360);
        char* list = field<char*>(decl, 16 * (k + 22));
        logf("[ewcoop] meshkit %s: %d entries, list %p\n", kit, count, list);
        if (count < 0 || count > 512 || !readable(list, 56 * static_cast<size_t>(count))) continue;
        for (int j = 0; j < count; ++j) {
            char* e = list + 56 * j;
            const char* name = field<const char*>(e, 8);
            logf("[ewcoop]   %s[%d] \"%s\" (%d mesh components)\n", kit, j,
                 (name && readable(name, 1)) ? name : "(bad)", field<uint16_t>(e, 48));
        }
    }
}

static bool isCharacterVtable(uintptr_t rva);  // defined with the shared-enemy code below

// ---- AI aggro: make enemies consider both players ----
static bool validEnt(void* e) {
    if (!e || !readable(e, 8)) return false;
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(e);
    return vt >= g_base && vt < g_exeEnd;
}
static bool entityOrigin(void* e, Vec3* out) {
    if (!validEnt(e)) return false;
    void* phys = field<void*>(e, off_entity_physics);
    if (!validEnt(phys)) return false;
    // An entity that is being destroyed has its vtables reset to abstract bases, where GetOrigin is the
    // _purecall stub (0x120B8F8): calling it is the R6025 "pure virtual function call" abort we hit twice
    // (stack: AI target select 0x7C9D9E -> hkAiPlayer -> here, after a synced kill removed the enemy).
    auto getOrigin = reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(phys))[slot_phys_GetOrigin]);
    if (getOrigin == g_base + 0x120B8F8 || getOrigin < g_base || getOrigin >= g_exeEnd) return false;
    *out = *vfn<GetVec_t>(phys, slot_phys_GetOrigin)(phys, 0);
    return true;
}
// Replace the first `n` bytes of a function with "mov rax, hook; jmp rax" (n >= 12), building an executable
// trampoline (the real leading bytes + jump back) that must be called to run the original. `orig` is the
// function's real leading bytes; if the target already holds an older ewcoop's jump we retake it. Returns the
// trampoline, or null if the code doesn't match.
static void* installInlineHook(uintptr_t rva, const unsigned char* orig, size_t n, void* hook) {
    auto* p = reinterpret_cast<unsigned char*>(g_base + rva);
    bool expected = memcmp(p, orig, n) == 0;
    bool oldHook = p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0;  // an older ewocop's jmp
    if (!expected && !oldHook) {
        logf("[ewcoop] inline hook %#llx: bytes differ\n", (unsigned long long)rva);
        return nullptr;
    }
    if (oldHook) logf("[ewcoop] taking over inline hook %#llx from an older ewcoop\n", (unsigned long long)rva);
    auto* tramp = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;
    memcpy(tramp, orig, n);
    auto back = reinterpret_cast<uintptr_t>(p + n);
    unsigned char* t = tramp + n;
    // jmp qword ptr [rip+0] ; <back>  -- indirect, so NO register is clobbered (some prologues set rax=rsp
    // and use it as the frame base; the old "mov rax,back; jmp rax" would corrupt that).
    t[0] = 0xFF; t[1] = 0x25; t[2] = 0; t[3] = 0; t[4] = 0; t[5] = 0;
    memcpy(t + 6, &back, 8);
    unsigned char patch[16];
    patch[0] = 0x48; patch[1] = 0xB8; memcpy(patch + 2, &hook, 8); patch[10] = 0xFF; patch[11] = 0xE0;
    for (size_t i = 12; i < n; ++i) patch[i] = 0x90;
    DWORD old;
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, patch, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return tramp;
}
// Notify hook: run `notify()` at a function's entry without knowing its arguments. A generated stub saves
// rcx/rdx/r8/r9 and xmm0-3, calls notify, restores them and jumps on to the trampoline (stack args untouched).
static bool installNotifyHook(uintptr_t rva, const unsigned char* orig, size_t n, void (*notify)()) {
    auto* s = static_cast<unsigned char*>(VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!s) return false;
    static const unsigned char pre[] = {
        0x51, 0x52, 0x41, 0x50, 0x41, 0x51,              // push rcx, rdx, r8, r9
        0x48, 0x83, 0xEC, 0x68,                          // sub rsp, 0x68 (keeps 16-byte alignment)
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,  // movdqu [rsp+20h/30h], xmm0/1
        0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,  // movdqu [rsp+40h/50h], xmm2/3
        0x48, 0xB8};                                     // mov rax, imm64 (notify)
    static const unsigned char post[] = {
        0xFF, 0xD0,                                      // call rax
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,
        0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
        0x48, 0x83, 0xC4, 0x68,                          // add rsp, 0x68
        0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,              // pop r9, r8, rdx, rcx
        0xFF, 0x25, 0, 0, 0, 0};                         // jmp [rip+0] -> trampoline (address follows)
    size_t k = 0;
    memcpy(s + k, pre, sizeof pre); k += sizeof pre;
    memcpy(s + k, &notify, 8); k += 8;
    memcpy(s + k, post, sizeof post); k += sizeof post;
    unsigned char* trampSlot = s + k;
    void* tramp = installInlineHook(rva, orig, n, s);
    if (!tramp) return false;
    memcpy(trampSlot, &tramp, 8);
    FlushInstructionCache(GetCurrentProcess(), s, 256);
    return true;
}
// Short inline hook for functions whose first 12 bytes can't be moved (a short jump inside them): a 5-byte
// `jmp rel32` to a stub allocated within 2 GB of the exe, which jumps on to the hook. The trampoline runs the
// first n (>= 5, position-independent) bytes, then jumps back.
static void* allocNear(uintptr_t target) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t g = si.dwAllocationGranularity;
    for (uintptr_t a = (target - 0x70000000) & ~(g - 1); a < target; a += g) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(a), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return nullptr;
}
static void* installShortHook(uintptr_t rva, const unsigned char* orig, size_t n, void* hook) {
    auto* p = reinterpret_cast<unsigned char*>(g_base + rva);
    bool expected = memcmp(p, orig, n) == 0;
    if (!expected && p[0] != 0xE9) { logf("[ewcoop] short hook %#llx: bytes differ\n", (unsigned long long)rva); return nullptr; }
    auto* page = static_cast<unsigned char*>(allocNear(g_base + rva));
    if (!page) { logf("[ewcoop] short hook %#llx: no memory near the exe\n", (unsigned long long)rva); return nullptr; }
    // page+0: jmp [rip+0] -> hook ; page+32: trampoline (orig bytes, jmp [rip+0] -> p+n)
    page[0] = 0xFF; page[1] = 0x25; memset(page + 2, 0, 4); memcpy(page + 6, &hook, 8);
    unsigned char* tramp = page + 32;
    memcpy(tramp, orig, n);
    uintptr_t back = reinterpret_cast<uintptr_t>(p + n);
    tramp[n] = 0xFF; tramp[n + 1] = 0x25; memset(tramp + n + 2, 0, 4); memcpy(tramp + n + 6, &back, 8);
    unsigned char patch[16];
    int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(page) - reinterpret_cast<intptr_t>(p + 5));
    patch[0] = 0xE9; memcpy(patch + 1, &rel, 4);
    for (size_t i = 5; i < n; ++i) patch[i] = 0x90;
    DWORD old;
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, patch, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return tramp;
}
// Arm the self-removing Render hook (captures the render world once). Re-armed on each level load.
static void armRenderHook() {
    if (g_renderHookOn) return;
    g_renderWorld = nullptr;
    g_renderWorldTramp = installInlineHook(rva_RenderWorld_Render, kRenderPrologue, 12,
                                           reinterpret_cast<void*>(&hkRenderWorldRender));
    g_renderHookOn = g_renderWorldTramp != nullptr;
}
// sub_8AB4F0(this = enemy): remember the enemy whose target-select is running.
static void* hkAiContext(void* self) {
    if (!g_unhook && validEnt(self)) g_aiThis = self;
    return g_origAiContext(self);
}
// Replacement for sub_8AB6D0 (= GetPlayer(0)): the player nearest the current enemy, else player 1.
static void* g_aiForceP2Owner;  // a replayed kill is running in this enemy's Think: his "player" is player 2
static bool g_aiForceP2;
static void* hkAiPlayer() {
    // Never hand the AI a deleted player 2 (a reload crashed the game in 0x7C904E reading one).
    if (g_player2 && !isA(g_player2, rva_idPlayer_vtable)) {
        void* game = *reinterpret_cast<void**>(g_base + rva_gameLocal);
        return game ? localPlayer(game) : nullptr;
    }
    if (!g_unhook && g_aiForceP2 && g_player2) return g_player2;
    void* game = *reinterpret_cast<void**>(g_base + rva_gameLocal);
    void* p1 = game ? localPlayer(game) : nullptr;
    if (g_unhook || !g_aiAggro || !g_player2 || !g_aiThis) return p1;
    // g_aiThis is remembered from the last target-select and may since have died or been removed: only use it
    // while it is still a live character (a concrete character vtable; a dying object's vtable is reset).
    if (!validEnt(g_aiThis) || !isCharacterVtable(*reinterpret_cast<uintptr_t*>(g_aiThis) - g_base)) {
        g_aiThis = nullptr;
        return p1;
    }
    // Player positions from the per-frame cache: a reload frees the players mid-frame while AI still runs.
    if (!g_playerPosValid[0] || !g_playerPosValid[1]) return p1;
    Vec3 e, a = g_playerPos[0], b = g_playerPos[1];
    if (entityOrigin(g_aiThis, &e)) {
        float da = (a.x - e.x) * (a.x - e.x) + (a.y - e.y) * (a.y - e.y) + (a.z - e.z) * (a.z - e.z);
        float db = (b.x - e.x) * (b.x - e.x) + (b.y - e.y) * (b.y - e.y) + (b.z - e.z) * (b.z - e.z);
        if (db < da) return g_player2;
    }
    return p1;
}
static bool patchAiPlayerGetter() {
    static const unsigned char orig[14] = {0x48, 0x8B, 0x0D, 0x19, 0x84, 0x20, 0x09, 0x33, 0xD2, 0xE9, 0x92, 0x19, 0xEF, 0xFF};
    auto* p = reinterpret_cast<unsigned char*>(g_base + rva_AI_PlayerGetter);
    bool expected = memcmp(p, orig, sizeof orig) == 0;
    bool oldHook = p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0;
    if (!expected && !oldHook) {
        logf("[ewcoop] AI player getter bytes differ, not hooking aggro\n");
        return false;
    }
    void* hook = reinterpret_cast<void*>(&hkAiPlayer);
    unsigned char patch[14] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0, 0x90, 0x90};
    memcpy(patch + 2, &hook, 8);
    DWORD old;
    VirtualProtect(p, 14, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, patch, 14);
    VirtualProtect(p, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 14);
    return true;
}
static bool hookAiAggro() {
    static const unsigned char kCtx[16] = {0x40, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x40};
    g_origAiContext = reinterpret_cast<AiTargetContext_t>(
        installInlineHook(rva_AI_TargetContext, kCtx, sizeof kCtx, reinterpret_cast<void*>(&hkAiContext)));
    if (!g_origAiContext) return false;
    return patchAiPlayerGetter();
}

// idPresentablePlayer's IsLocal (vtable slot 57, +0x1C8): false for player 2, so its HUD is never created and
// the HUD renderer null-derefs once P2 is the local player. Force it true for P2's presentable; chain otherwise.
// Compares against a pointer cached once a frame (hkRunFrame, after player 2 is confirmed alive): reading
// g_player2 here crashed after a death reload, which frees him mid-frame before our checks run.
static void* g_p2Presentable;
static void* hkPresentableLocal(void* self) {
    if (!g_unhook && g_p2Presentable && self == g_p2Presentable) return reinterpret_cast<void*>(1);
    return reinterpret_cast<void* (*)(void*)>(g_origPresentableLocal)(self);
}
static void hookPresentableLocal() {
    static bool done;
    if (done || !g_player1) return;
    void* pr = presentableOf(g_player1);
    if (!pr) return;
    done = true;
    auto slot = *reinterpret_cast<void***>(pr) + 57;
    auto cur = reinterpret_cast<uintptr_t>(*slot);
    if (cur < g_base || cur >= g_exeEnd) {
        logf("[ewcoop] presentable slot 57 = %#llx outside the exe; not hooking (HUD seems fine anyway)\n",
             (unsigned long long)cur);
        return;
    }
    g_origPresentableLocal = *slot;
    writeSlot(slot, reinterpret_cast<void*>(&hkPresentableLocal));
    logf("[ewcoop] hooked presentable IsLocal (slot 57, was rva %#llx)\n", (unsigned long long)(cur - g_base));
}

// The object that really positions a player (see game.h); null if not set up.
static void* playerBody(void* player) {
    void* anim = field<void*>(player, off_entity_anim);
    if (!anim) return nullptr;
    char* ctrl = vfn<char* (*)(void*)>(anim, slot_anim_GetController)(anim);
    return ctrl ? ctrl + off_controller_body : nullptr;
}
static Vec3 bodyOrigin(void* body) { return *vfn<GetVec_t>(body, slot_phys_GetOrigin)(body, 0); }
// The actor motion object at +1720 is the real authority: every frame 0x91F2C0 copies its position
// into the physics (found with the Ctrl+F6 watchpoint). Set it too, as partner Teleport does.
static void placePlayer(void* player, const Vec3& o) {
    if (void* motion = field<void*>(player, off_partner_facing))
        fn<SetFacingOrigin_t>(rva_SetFacingOrigin)(motion, &o);
    if (void* body = playerBody(player)) vfn<SetOrigin_t>(body, slot_phys_SetOrigin)(body, &o, -1);
    void* phys = physicsOf(player);
    if (phys) vfn<SetOrigin_t>(phys, slot_phys_SetOrigin)(phys, &o, -1);
    fn<void (*)(void*)>(rva_UpdateVisuals)(player);
}

// A temporary code patch: checks the original bytes, swaps in new ones, restores them.
struct CodePatch {
    uintptr_t rva;
    const unsigned char* orig;
    const unsigned char* repl;
    size_t len;         // bytes written
    size_t checkLen = 0;  // bytes verified (defaults to len)
    unsigned char* at() const { return reinterpret_cast<unsigned char*>(g_base + rva); }
    bool matches() const { return memcmp(at(), orig, checkLen ? checkLen : len) == 0; }
    void write(const unsigned char* bytes) const {
        DWORD old;
        VirtualProtect(at(), len, PAGE_EXECUTE_READWRITE, &old);
        memcpy(at(), bytes, len);
        VirtualProtect(at(), len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at(), len);
    }
    void apply() const { write(repl); }
    void restore() const { write(orig); }
};

// "R6025 pure virtual function call" aborts via __fastfail, which our exception logger never sees. The game's
// _purecall (0x120B8F8) first calls an optional handler stored EncodePointer'd at 0x9B97348 (MSVC
// _set_purecall_handler), so we install one that logs who made the call, then let the game abort as before.
static void purecallHandler() {
    void* frames[32];
    USHORT n = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&purecallHandler), &self);
    logf("[ewcoop] PURE VIRTUAL CALL (R6025). Stack (game = exe rva, mod = ewcoop offset):\n");
    for (USHORT i = 0; i < n; ++i) {
        auto a = reinterpret_cast<uintptr_t>(frames[i]);
        if (a >= g_base && a < g_exeEnd)
            logf("         game rva %#llx\n", (unsigned long long)(a - g_base));
        else if (self && a >= reinterpret_cast<uintptr_t>(self) && a < reinterpret_cast<uintptr_t>(self) + 0x40000)
            logf("         mod +%#llx\n", (unsigned long long)(a - reinterpret_cast<uintptr_t>(self)));
    }
    logf("[ewcoop] context: player2=%p joseph=%p ctxPlayer=%p\n", g_player2, g_joseph, g_ctxPlayer);
}
static void installPurecallHandler() {
    *reinterpret_cast<void**>(g_base + 0x9B97348) = EncodePointer(reinterpret_cast<void*>(&purecallHandler));
}

// Log access violations inside the game exe (first few), then let the game crash as normal.
static LONG CALLBACK crashLogger(EXCEPTION_POINTERS* ep) {
    static int logged;
    auto* rec = ep->ExceptionRecord;
    auto ip = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ip >= g_base && ip < g_exeEnd && logged < 5) {
        ++logged;
        auto* c = ep->ContextRecord;
        logf("[ewcoop] CRASH: access violation at rva %#llx (%s address %p)\n"
             "         rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx\n",
             (unsigned long long)(ip - g_base), rec->ExceptionInformation[0] ? "writing" : "reading",
             reinterpret_cast<void*>(rec->ExceptionInformation[1]),
             c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R8, c->R9);
        // Return addresses on the stack that point into the game, for a rough call stack.
        auto* sp = reinterpret_cast<uintptr_t*>(c->Rsp);
        int found = 0;
        for (int i = 0; i < 512 && found < 8; ++i) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            if (sp[i] > g_base + 0x1000 && sp[i] < g_base + 0x1477000) {
                logf("         stack: rva %#llx\n", (unsigned long long)(sp[i] - g_base));
                ++found;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- Hardware write watchpoints (debug registers) to find who writes a value ----
static uintptr_t g_watch[2];         // addresses watched by DR0/DR1
static volatile LONG g_watchHits;
static uintptr_t g_watchSeen[48];    // unique accessor rvas already logged
static bool g_watchReads;            // false: writes only; true: reads and writes
static bool g_watchExec;             // execute breakpoints on code addresses instead
static int g_watchSeenCount;

static void setDebugRegs(bool on) {
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        SuspendThread(t);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            c.Dr0 = on ? g_watch[0] : 0;
            c.Dr1 = on ? g_watch[1] : 0;
            // L0, L1 enabled; RW = 01 (write) or 11 (read/write); LEN = 11 (4 bytes)
            unsigned long long rw = g_watchExec ? 0ull : g_watchReads ? 3ull : 1ull;
            unsigned long long len = g_watchExec ? 0ull : 3ull;  // execute breakpoints need LEN = 00
            c.Dr7 = on ? (1 | 4 | (rw << 16) | (len << 18) | (rw << 20) | (len << 22)) : 0;
            if (SetThreadContext(t, &c)) ++n;
        }
        ResumeThread(t);
        CloseHandle(t);
    }
    CloseHandle(snap);
    logf("[ewcoop] watchpoints %s on %d threads (%p, %p)\n", on ? "set" : "cleared", n,
         reinterpret_cast<void*>(g_watch[0]), reinterpret_cast<void*>(g_watch[1]));
}
static DWORD WINAPI watchThread(LPVOID on) {  // runs off the game thread so it can set the game thread too
    setDebugRegs(on != nullptr);
    return 0;
}
static LONG CALLBACK watchHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    auto* c = ep->ContextRecord;
    if ((c->Dr6 & 3) == 0) return EXCEPTION_CONTINUE_SEARCH;
    if (g_watchExec) {  // at function entry [rsp] is the exact return address; rcx is `this`
        uintptr_t ret = *reinterpret_cast<uintptr_t*>(c->Rsp);
        uintptr_t key = ret ^ (c->Dr6 & 1 ? 0 : 1ull << 62) ^ (c->Rcx << 1);
        bool seen = false;
        for (int i = 0; i < g_watchSeenCount; ++i) seen |= g_watchSeen[i] == key;
        if (!seen && g_watchSeenCount < 48) {
            g_watchSeen[g_watchSeenCount++] = key;
            logf("[ewcoop] EXEC: %s entered, this=%p, called from rva %#llx\n",
                 c->Dr6 & 1 ? "DR0 fn" : "DR1 fn", reinterpret_cast<void*>(c->Rcx),
                 (unsigned long long)(ret >= g_base && ret < g_exeEnd ? ret - g_base : ret));
        }
        InterlockedIncrement(&g_watchHits);
        c->Dr6 = 0;
        c->EFlags |= 0x10000;  // resume flag: run the instruction instead of re-trapping
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    uintptr_t ip = c->Rip;
    uintptr_t rva = ip >= g_base && ip < g_exeEnd ? ip - g_base : ip;
    InterlockedIncrement(&g_watchHits);
    bool seen = false;
    for (int i = 0; i < g_watchSeenCount; ++i) seen |= g_watchSeen[i] == rva;
    if (!seen && g_watchSeenCount < 48) {
        g_watchSeen[g_watchSeenCount++] = rva;
        // Use the address from the CPU's own debug register: g_watch may be empty if an older ewcoop armed it.
        auto* at = reinterpret_cast<uint32_t*>(c->Dr6 & 1 ? c->Dr0 : c->Dr1);
        bool ok = at && readable(at, 4);
        logf("[ewcoop] WATCH: %s %s by code at rva %#llx (instruction before it), value now %.1f / %#x, thread %lu\n",
             c->Dr6 & 1 ? "DR0" : "DR1", g_watchReads ? "accessed" : "written", (unsigned long long)rva,
             ok ? *reinterpret_cast<float*>(at) : 0.f, ok ? *at : 0u, GetCurrentThreadId());
        auto* sp = reinterpret_cast<uintptr_t*>(c->Rsp);
        int found = 0;
        for (int i = 0; i < 256 && found < (g_watchReads ? 2 : 6); ++i)
            if (sp[i] > g_base + 0x1000 && sp[i] < g_base + 0x1477000) {
                logf("         stack: rva %#llx\n", (unsigned long long)(sp[i] - g_base));
                ++found;
            }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void sneakDiag(const char* who, void* p) {
    if (!p) return;
    auto node = [&](uintptr_t off) {
        return fn<IsNodeActive_t>(rva_IsNodeActive)(static_cast<char*>(p) + 1280, field<uint16_t>(p, off));
    };
    float v72 = vfn<float (*)(void*)>(p, slot_player_vfunc72)(p);
    bool can = vfn<bool (*)(void*)>(p, slot_player_CanSneak)(p);
    logf("[ewcoop] %s CanSneak=%d | 4791=%d 4780=%d node6872=%d(want 1) node6874=%d(want 0) vf72=%.1f(<35) "
         "3976=%d 4348=%d 3984=%d sneaking=%d\n",
         who, can, field<uint8_t>(p, 4791), field<uint8_t>(p, 4780), node(6872), node(6874), v72,
         field<int>(p, 3976), field<uint8_t>(p, 4348), field<int>(p, 3984), field<uint8_t>(p, off_player_sneaking));
}

// ---- Player 2 input from the numpad (local test before networking) ----
static float g_p2Yaw, g_p2Pitch;
static int32_t g_p2Frame = 1000000;
static short angleToShort(float deg) { return static_cast<short>(static_cast<int>(deg * 65536.f / 360.f) & 0xFFFF); }

static void dumpUsercmd(const char* who, const Usercmd& c) {
    logf("[ewcoop] %s usercmd: buttons=%x b2=%x fwd=%d right=%d up=%d inhib=%d camInhib=%d angles=(%d %d %d) "
         "frame=%d pos=(%.0f %.0f %.0f) pyr=(%d %d %d) predBits=%x throttle=%d svTime=%d fireCount=%d speed=%d\n",
         who, c.buttons, c.buttons2, c.forwardmove, c.rightmove, c.upmove, c.inhibited, c.cameraInhibited,
         c.angles[0], c.angles[1], c.angles[2], c.clientGameFrame, c.pos[0], c.pos[1], c.pos[2], c.pitch, c.yaw,
         c.roll, c.predictionStateBits, c.vehicleThrottle, c.serverGameTime, c.fireCount, c.speed);
}

// Feed a player an "idle" usercmd (keeps its animation/state machine ticking without moving it).
static void feedIdle(void* player, const Usercmd* base) {
    if (!player) return;
    Usercmd c{};
    if (base) c = *base;
    c.buttons = 0;
    c.buttons2 = 0;
    c.forwardmove = 0;
    c.rightmove = 0;
    c.upmove = 0;
    c.inhibited = false;
    c.cameraInhibited = false;
    c.clientGameFrame = ++g_p2Frame;
    fn<PlayerSetUsercmd_t>(rva_PlayerSetUsercmd)(player, &c, &c);
}

// Read the newest usercmd the user-cmd manager holds for a player slot. The network transport fills these
// per-client buffers (idUserCmdMgr::PutUserCmdForPlayer), so this is what a joining player's input lands in.
// Layout: cmdBuffer[player][frame] (player stride 0x2000, frame stride 0x40), writeFrame[MAX_PLAYERS]@0x4000,
// readFrame[MAX_PLAYERS]@0x4008.
static bool readUserCmdForPlayer(void* mgr, int slot, Usercmd* out) {
    if (!mgr || !readable(mgr, 0x4010)) return false;
    int wf = *reinterpret_cast<int*>(static_cast<char*>(mgr) + 0x4000 + slot * 4);
    if (wf <= 0) return false;
    int idx = (wf > 1 ? wf - 1 : 0) % 128;
    *out = *reinterpret_cast<Usercmd*>(static_cast<char*>(mgr) + slot * 0x2000 + idx * 0x40);
    return true;
}

static void feedPlayer2(float dt, const Usercmd* p1cmd) {
    Usercmd c{};
    if (p1cmd) {  // start from player 1's cmd so timing fields look normal, then replace the input
        c = *p1cmd;
    }
    c.buttons = 0;
    c.buttons2 = 0;
    int fwd = (held(VK_NUMPAD8) ? 127 : 0) - (held(VK_NUMPAD5) ? 127 : 0);
    int right = (held(VK_NUMPAD6) ? 127 : 0) - (held(VK_NUMPAD4) ? 127 : 0);
    c.forwardmove = static_cast<int8_t>(fwd);
    c.rightmove = static_cast<int8_t>(right);
    c.upmove = 0;
    g_p2Yaw += ((held(VK_NUMPAD7) ? 1.f : 0.f) - (held(VK_NUMPAD9) ? 1.f : 0.f)) * 150.f * dt;
    if (held(VK_NUMPAD0)) c.buttons |= BUTTON_SPRINT;
    static bool sneak;  // the game keeps sneak as an on/off state in the cmd (player 1 shows 0x200 held)
    if (pressed(VK_DECIMAL) || pressed(VK_DELETE)) {
        sneak = !sneak;
        logf("[ewcoop] player 2 sneak %s\n", sneak ? "on" : "off");
    }
    if (sneak) c.buttons |= BUTTON_SNEAK;
    static float diagT;
    diagT += dt;
    if (sneak && diagT >= 1.f) {  // what the sneak state machine 0x7F8910 sees for player 2
        diagT = 0.f;
        void* anim = field<void*>(g_player2, off_entity_anim);
        char* ctrl = anim ? vfn<char* (*)(void*)>(anim, slot_anim_GetController)(anim) : nullptr;
        void* item = vfn<void* (*)(void*)>(g_player2, 1568 / 8)(g_player2);
        logf("[ewcoop] p2 sneak diag: ctrl buttons A=%#x (old %#x) B=%#x inhibitedA=%d holdMode=%d item=%p item+60=%d "
             "blocked4351=%d 4799=%d sneaking=%d\n",
             ctrl ? *reinterpret_cast<uint32_t*>(ctrl + 20460) : 0, ctrl ? *reinterpret_cast<uint32_t*>(ctrl + 20396) : 0,
             ctrl ? *reinterpret_cast<uint32_t*>(ctrl + 20616) : 0, ctrl ? ctrl[20469] : -1,
             *reinterpret_cast<int*>(g_base + 0x9AC0558) == 0, item, item ? field<int>(item, 60) : -1,
             field<uint8_t>(g_player2, 4351), field<uint8_t>(g_player2, 4799), field<uint8_t>(g_player2, off_player_sneaking));
    }
    if (held(VK_ADD)) c.buttons |= BUTTON_ATTACK2;
    if (held(VK_SUBTRACT)) c.buttons |= BUTTON_ATTACK1;
    if (held(VK_DIVIDE)) c.buttons |= BUTTON_USE;
    c.angles[0] = angleToShort(g_p2Pitch);
    c.angles[1] = angleToShort(g_p2Yaw);
    c.angles[2] = 0;
    c.pitch = c.angles[0];
    c.yaw = c.angles[1];
    c.roll = 0;
    c.inhibited = false;
    c.cameraInhibited = false;
    c.clientGameFrame = ++g_p2Frame;
    fn<PlayerSetUsercmd_t>(rva_PlayerSetUsercmd)(g_player2, &c, &c);
}

// Experiment: spawn a second Sebastian from the "player" entityDef (the name gameLocal returns
// for the main campaign, 0x79D090) and report what the game does with it.
// The entityDef's full state text (idDeclEntityDef +0x1A0 idStr) is what spawning reads. The player
// def says renderModelInfo = { model = "pl/pl0000.md6"; ... }; Joseph (ai/partner/joseph) uses
// "pl/pl9000.md6". Same length, so the path can be swapped in place for one spawn.
static char* findModelPath(void* decl, const char* path) {
    int len = field<int>(decl, 0x1A0);
    char* text = field<char*>(decl, 0x1A8);
    if (!text || len <= 0) return nullptr;
    size_t n = strlen(path);
    for (int i = 0; i + (int)n <= len; ++i)
        if (memcmp(text + i, path, n) == 0) return text + i;
    return nullptr;
}

enum SpawnMode { SpawnAuto, SpawnSebastian, SpawnJoseph };
static void spawnSecondPlayer(void* gameLocal, int mode) {
    void* p1 = localPlayer(gameLocal);
    auto find = fn<FindEntityDef_t>(rva_FindEntityDef);
    void* list = reinterpret_cast<void*>(g_base + rva_entityDefList);
    // Adaptive: prefer the DLC Kidman player (real model + player VO) when present; otherwise a PLAIN
    // second Sebastian (no model swap -> correct rig, no bone mangling). Force Joseph via Ctrl+Alt+F8.
    const char* declName = "player";
    bool josephModel = false;
    if (mode == SpawnJoseph) {
        josephModel = true;
    } else if (mode == SpawnAuto) {
        if (find(list, "player_kid", 0, 0, 0, 0, 0, 0, 0)) declName = "player_kid";  // Kidman (DLC)
    }
    void* decl = find(list, declName, 0, 0, 0, 0, 0, 0, 0);
    logf("[ewcoop] spawn player 2: entityDef '%s' = %p, player 1 = %p (mode %d)\n", declName, decl, p1, mode);
    if (!decl) return;
    // Patches active only for this one spawn, restored right after:
    //  - idPlayer::Spawn's MAX_PLAYERS check (jge -> 6 NOPs)
    //  - the per-player record write at the end of Spawn (-> ret), sized for 1 player
    static const unsigned char nops[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    static const unsigned char ret[1] = {0xC3};
    CodePatch patches[] = {
        {rva_PlayerSpawn_maxPlayersJge, kMaxPlayersJge, nops, sizeof kMaxPlayersJge},
        {rva_MarkPlayerRecord, kMarkPlayerRecord, ret, 1, sizeof kMarkPlayerRecord},
    };
    for (auto& p : patches)
        if (!p.matches()) {
            logf("[ewcoop]   code at %#llx differs from build 2708258, not spawning\n", (unsigned long long)p.rva);
            return;
        }
    char* modelPath = josephModel ? findModelPath(decl, "pl/pl0000.md6") : nullptr;
    if (josephModel && !modelPath) logf("[ewcoop]   player model path not found in decl text, using Sebastian\n");
    if (modelPath) memcpy(modelPath, "pl/pl9000.md6", 13);  // whole-model swap to Joseph (this spawn only)
    for (auto& p : patches) p.apply();
    logf("[ewcoop]   player checks lifted, spawning (%s model)\n", modelPath ? "Joseph (whole-model)" : declName);
    void* ent = vfn<SpawnEntityDef_t>(gameLocal, slot_SpawnEntityDef)(gameLocal, decl, -1);
    for (auto& p : patches) p.restore();
    if (modelPath) memcpy(modelPath, "pl/pl0000.md6", 13);
    void* p1after = localPlayer(gameLocal);
    logf("[ewcoop]   spawned %p (name \"%s\"), is idPlayer: %d; player slot 0 is now %p (%s)\n", ent,
         entityName(ent) ? entityName(ent) : "", isA(ent, rva_idPlayer_vtable), p1after,
         p1after == p1 ? "unchanged" : "CHANGED");
    if (!ent) return;
    g_player2 = ent;
    char hexptr[32];
    snprintf(hexptr, sizeof hexptr, "%p", ent);
    SetEnvironmentVariableA("EWCOOP_PLAYER2", hexptr);  // lets a reloaded ewcoop find him again
    g_p2Pitch = 0.f;
    g_p2Yaw = p1 ? yawOf(p1) : 0.f;
    if (p1) {
        Vec3 p = originOf(p1);
        float r = yawOf(p1) / 57.2957795f;
        Vec3 dst{p.x + std::cos(r) * 80.f, p.y + std::sin(r) * 80.f, p.z + 8.f};
        void* body = playerBody(ent);
        logf("[ewcoop]   player 2 body controller %p (player 1's %p)\n", body, playerBody(p1));
        if (body) {
            Vec3 before = bodyOrigin(body);
            placePlayer(ent, dst);
            Vec3 now = bodyOrigin(body);
            logf("[ewcoop]   body was at (%.0f %.0f %.0f), moved to (%.0f %.0f %.0f), reads back (%.0f %.0f %.0f)\n",
                 before.x, before.y, before.z, dst.x, dst.y, dst.z, now.x, now.y, now.z);
        }
    }
}

static void bringToPlayer(void* player) {
    Vec3 p = originOf(player);
    float pyaw = yawOf(player);
    float r = pyaw / 57.2957795f;
    Vec3 dst{p.x + std::cos(r) * 60.f, p.y + std::sin(r) * 60.f, p.z + 8.f};  // a bit in front
    g_yaw = pyaw;
    teleport(g_joseph, dst, g_yaw, true);
    g_pos = dst;
    g_bodyYaw = g_yaw;
    logf("[ewcoop] Joseph -> (%.0f %.0f %.0f) yaw %.0f; player at (%.0f %.0f %.0f)\n",
         dst.x, dst.y, dst.z, g_yaw, p.x, p.y, p.z);
}

// Give his AI a "walk/run to position" action, exactly like his own AI builds one (0x9C16E0).
static void order(int type, const Vec3& goal) {
    ActionData a{};
    a.vtbl = reinterpret_cast<void*>(g_base + rva_PartnerActionData_vtable);
    a.position = goal;
    a.targetSpawnId = 0xFFFF;
    a.targetSerial = -1;
    fn<IdStrCtor_t>(rva_idStr_ctor)(a.editString, "", 0);
    a.editValue = kOurAction;
    a.actionType = type;
    vfn<ExecuteAction_t>(g_joseph, slot_partner_ExecuteAction)(g_joseph, &a, 2, 0);
}

static void stopMoving() {
    if (g_moving && g_joseph && g_mode == ModeAiWalk) {
        order(action_WalkTo, originOf(g_joseph));  // walk to where he stands = stop
        logf("[ewcoop] stop order\n");
        g_moving = false;
        return;
    }
    if (g_moving && g_joseph) {
        bool ok = request(g_joseph, off_partner_reqStop);
        logf("[ewcoop] Stop request sent: %d\n", ok);
    }
    g_moving = false;
}

static void puppetStep(float dt) {
    float fwd = (held(VK_NUMPAD8) ? 1.f : 0.f) - (held(VK_NUMPAD5) ? 1.f : 0.f);
    float side = (held(VK_NUMPAD9) ? 1.f : 0.f) - (held(VK_NUMPAD7) ? 1.f : 0.f);
    float turn = (held(VK_NUMPAD4) ? 1.f : 0.f) - (held(VK_NUMPAD6) ? 1.f : 0.f);
    bool run = held(VK_NUMPAD0);

    g_yaw += turn * 140.f * dt;
    float r = g_yaw / 57.2957795f;
    // id: yaw 0 = +X, yaw 90 = +Y; right-hand strafe is -90 degrees.
    float dx = std::cos(r) * fwd + std::sin(r) * side;
    float dy = std::sin(r) * fwd - std::cos(r) * side;
    bool wantMove = fwd != 0.f || side != 0.f;

    if (g_mode == ModeAiWalk) {
        if (wantMove) {
            float len = std::sqrt(dx * dx + dy * dy);
            g_orderTimer -= dt;
            if (!g_moving || run != g_run || g_orderTimer <= 0.f) {
                Vec3 o = originOf(g_joseph);
                Vec3 goal{o.x + dx / len * 250.f, o.y + dy / len * 250.f, o.z};
                order(run ? action_RunTo : action_WalkTo, goal);
                if (!g_moving || run != g_run) logf("[ewcoop] %s order -> (%.0f %.0f %.0f)\n", run ? "run" : "walk", goal.x, goal.y, goal.z);
                g_orderTimer = 0.2f;
            }
            g_moving = true;
            g_run = run;
        } else {
            stopMoving();
        }
        g_logTimer += dt;
        if (g_logTimer >= 1.f) {
            Vec3 p = originOf(g_joseph);
            float d = std::sqrt((p.x - g_lastPos.x) * (p.x - g_lastPos.x) + (p.y - g_lastPos.y) * (p.y - g_lastPos.y));
            if (g_moving) logf("[ewcoop] moved %.0f units in the last %.1fs\n", d, g_logTimer);
            g_lastPos = p;
            g_logTimer = 0.f;
        }
        return;
    }

    if (g_mode == ModeSlide) {  // v0: move by teleporting, no animation
        float speed = run ? 250.f : 110.f;
        Vec3 o = originOf(g_joseph);
        o.x += dx * speed * dt;
        o.y += dy * speed * dt;
        teleport(g_joseph, o, g_yaw, false);
        g_pos = o;
        return;
    }

    // We own the horizontal position (walk/run speed), his physics owns the height (floor),
    // and the walk/run animation is kept playing on top. Root motion is overwritten each frame.
    Vec3 cur = originOf(g_joseph);
    if (wantMove) {
        float len = std::sqrt(dx * dx + dy * dy);
        float speed = run ? 250.f : 110.f;
        g_pos.x += dx / len * speed * dt;
        g_pos.y += dy / len * speed * dt;

        float moveYaw = std::atan2(dy, dx) * 57.2957795f;
        float d = angleDiff(moveYaw, g_bodyYaw);
        float maxStep = 540.f * dt;  // smooth turn toward the move direction
        g_bodyYaw += d > maxStep ? maxStep : (d < -maxStep ? -maxStep : d);

        setMoveSpeed(g_joseph, run ? 1.f : 0.f);
        g_resend -= dt;
        if (!g_moving || run != g_run || g_resend <= 0.f) {
            bool ok = request(g_joseph, off_partner_reqMove);
            if (!g_moving || run != g_run) logf("[ewcoop] Move request (%s) sent: %d\n", run ? "run" : "walk", ok);
            else if (ok) logf("[ewcoop] Move restarted (animation had stopped)\n");
            g_resend = 0.25f;
        }
        g_moving = true;
        g_run = run;
    } else {
        stopMoving();
    }
    place(g_joseph, Vec3{g_pos.x, g_pos.y, cur.z});
    face(g_joseph, g_bodyYaw);

    g_logTimer += dt;
    if (g_logTimer >= 1.f) {
        Vec3 p = originOf(g_joseph);
        float d = std::sqrt((p.x - g_lastPos.x) * (p.x - g_lastPos.x) + (p.y - g_lastPos.y) * (p.y - g_lastPos.y));
        if (g_moving) logf("[ewcoop] moved %.0f units in the last %.1fs\n", d, g_logTimer);
        g_lastPos = p;
        g_logTimer = 0.f;
    }
}

static int64_t hkHandleAction(void* self, void* action, int64_t a3, uint32_t a4, char a5, char a6) {
    if (!g_unhook && g_puppet && self == g_joseph) {
        auto* a = static_cast<ActionData*>(action);
        bool ours = g_mode == ModeAiWalk && a->editValue == kOurAction;
        // His own AI's actions report "done" at once so the queue moves on to ours.
        if (!ours) return g_mode == ModeAiWalk ? 1 : 0;
    }
    return g_origHandleAction(self, action, a3, a4, a5, a6);
}

// Replacement for idGameLocal::GetPlayer (same logic, plus the player 2 context swap).
static void* hkGetPlayer(void* gameLocal, int idx) {
    if (idx != 0) return nullptr;
    if (void* ctx = g_ctxPlayer)
        if (GetCurrentThreadId() == g_ctxThread) return ctx;  // only on the thread running his Think
    auto mgr = field<char*>(gameLocal, off_gameLocal_entMgr);
    return fn<void* (*)(void*)>(rva_CastToPlayer)(*reinterpret_cast<void**>(mgr + off_entMgr_players));
}
static void writeCode(uintptr_t rva, const unsigned char* bytes, size_t n) {
    auto* p = reinterpret_cast<unsigned char*>(g_base + rva);
    DWORD old;
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
}

// Let the ENGINE create a second real player. idGameLocal::SetupPlayer clamps its index to 0 (`if (a2>=1)
// a2=0`), and LoadMap loops SetupPlayer(j) for j < numPlayers(gameLocal+0xAB38). We NOP the clamp and raise
// numPlayers so the engine spawns player slots 0 AND 1 (entityNumbers 0/1) at map load -- the base the
// networked 2nd client plugs into. Ctrl+Shift+P toggles; reload the map after turning it on.
static const unsigned char kSetupClamp[8] = {0x33, 0xF6, 0x83, 0xFD, 0x01, 0x0F, 0x4D, 0xEE};
static const unsigned char kNops8[8] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
static bool g_engineMP;
static void toggleEngineMP() {
    auto at = reinterpret_cast<unsigned char*>(g_base + rva_SetupPlayerClamp);
    void* game = *reinterpret_cast<void**>(g_base + rva_gameLocal);
    int* np = game ? reinterpret_cast<int*>(static_cast<char*>(game) + off_game_numPlayers) : nullptr;
    if (!g_engineMP) {
        if (memcmp(at, kSetupClamp, sizeof kSetupClamp) != 0) {
            logf("[ewcoop] SetupPlayer clamp bytes differ from build 2708258; not patching\n");
            return;
        }
        writeCode(rva_SetupPlayerClamp, kNops8, sizeof kSetupClamp);  // drop the index clamp
        auto s = reinterpret_cast<unsigned char*>(g_base + rva_PlayerSpawn_maxPlayersJge);
        if (memcmp(s, kMaxPlayersJge, sizeof kMaxPlayersJge) == 0)  // lift the MAX_PLAYERS check for slot 1
            writeCode(rva_PlayerSpawn_maxPlayersJge, kNops8, sizeof kMaxPlayersJge);
        if (np) { logf("[ewcoop] numPlayers %d -> 2\n", *np); *np = 2; }
        g_engineMP = true;
        logf("[ewcoop] engine multi-player ON (clamp removed, MAX_PLAYERS lifted, numPlayers=2). Reload the map.\n");
    } else {
        writeCode(rva_SetupPlayerClamp, kSetupClamp, sizeof kSetupClamp);
        if (np) *np = 1;
        g_engineMP = false;
        logf("[ewcoop] engine multi-player OFF\n");
    }
}
static void hookGetPlayer() {
    auto* p = reinterpret_cast<unsigned char*>(g_base + rva_GetPlayer);
    bool original = memcmp(p, kGetPlayerHead, sizeof kGetPlayerHead) == 0;
    bool ours = p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0;  // an older ewcoop's jump
    if (!original && !ours) {
        logf("[ewcoop] GetPlayer bytes differ, not hooking it\n");
        return;
    }
    unsigned char jmp[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};  // mov rax, imm64; jmp rax
    auto target = reinterpret_cast<uintptr_t>(&hkGetPlayer);
    memcpy(jmp + 2, &target, 8);
    writeCode(rva_GetPlayer, jmp, sizeof jmp);
    g_getPlayerHooked = true;
}
// While a net session runs the swap is off: inside player 2's Think it made "player 0" code (HUD, weapon
// display) use player 2, so the remote's weapon showed up as the local player's. Sneak doesn't need it (the
// IsLocalPlayer hook fixed that). Set by netRun.
static bool g_ctxSwapOff;
static void* g_thinkingPlayer;  // the idPlayer whose Think is running (always set, unlike the GetPlayer swap)
static int64_t hkPlayerThink(void* self) {
    if (g_unhook || g_ctxSwapOff) {
        g_thinkingPlayer = self;
        int64_t r = g_origThink(self);
        g_thinkingPlayer = nullptr;
        return r;
    }
    g_ctxThread = GetCurrentThreadId();
    g_ctxPlayer = self;
    int64_t r = g_origThink(self);
    g_ctxPlayer = nullptr;
    return r;
}

// Player 2 is driven by local input (later: network input), so he counts as a local player.
static bool hkIsLocal(void* self, void* a2) {
    if (!g_unhook && self == g_player2 && g_player2) return true;
    return g_origIsLocal(self, a2);
}

// Sneak toggle check 0x82F090 (no `this`): "sneak (0x200) or cancel (8) went down this frame" on GetPlayer(0)'s
// controller (current buttons +20616, previous +20552). In net sessions the GetPlayer(0) swap is off, so player
// 2's sneak read OUR buttons and never toggled. Replaced: read the buttons of the player whose Think runs.
static const unsigned char kSneakPressedHead[12] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x0D, 0x53, 0x4A, 0x28};
static bool g_sneakHooked;
static uint64_t hkSneakPressed() {
    void* p = g_thinkingPlayer && g_thinkingPlayer == g_player2 && g_player2
                  ? g_player2
                  : fn<void* (*)(void*, int)>(rva_GetPlayer)(*reinterpret_cast<void**>(g_base + rva_gameLocal), 0);
    if (!p) return 0;
    void* pres = field<void*>(p, 680);
    if (!pres) { fn<void (*)(void*)>(0x8729B0)(p); pres = field<void*>(p, 680); }
    if (!pres) return 0;
    char* c = static_cast<char*>(vfn<void* (*)(void*)>(pres, 96 / 8)(pres));
    uint32_t cur = *reinterpret_cast<uint32_t*>(c + 20616), prev = *reinterpret_cast<uint32_t*>(c + 20552);
    uint64_t v = ((cur & 0x200) && !(prev & 0x200)) ? 1 : 0;
    if ((cur & 8) && !(prev & 8)) v |= 1;
    return v;
}
static void hookSneakPressed() {
    auto* p = reinterpret_cast<unsigned char*>(g_base + 0x82F090);
    bool original = memcmp(p, kSneakPressedHead, 12) == 0;
    bool ours = p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0;
    if (!original && !ours) { logf("[ewcoop] sneak check bytes differ, not hooking\n"); return; }
    unsigned char jmp[12] = {0x48, 0xB8};
    void* target = reinterpret_cast<void*>(&hkSneakPressed);
    memcpy(jmp + 2, &target, 8);
    jmp[10] = 0xFF; jmp[11] = 0xE0;
    writeCode(0x82F090, jmp, 12);
    g_sneakHooked = true;
    logf("[ewcoop] sneak toggle check: reads the thinking player's buttons\n");
}

// No friendly fire between the two players; enemies and the world still hurt both.
static bool g_applyingNetDamage;  // replaying a remote hit: don't send it back (also used by the enemy replay)
static bool g_netStarted;         // netcode section below
static void sendPlayerHit(void* decl, void* info, const float* dir, void* loc);  // shared enemies section
static double hkPlayerDamage(void* self, void* inflictor, void* attacker, void* decl, void* a5, float* a6, void* loc) {
    if (!g_unhook && g_player2 && g_player1) {
        void* other = self == g_player2 ? g_player1 : self == g_player1 ? g_player2 : nullptr;
        if (other && (attacker == other || inflictor == other)) return 0.0;
        if (g_netStarted && !g_applyingNetDamage) {
            // Each player's own game decides his hits. Player 2's hits here would be a second, different
            // health; his real ones arrive as replayed events (applyEvents) so his stagger/knockdown plays.
            if (self == g_player2) return 0.0;
            if (self == g_player1) sendPlayerHit(decl, a5, a6, loc);
        }
    }
    return g_origDamage(self, inflictor, attacker, decl, a5, a6, loc);
}

// idPlayer SetHealth (vtable slot 89, 0x7BE370). Enemy attacks don't go through Damage (slot 80), so to find their
// real path we log the unwound call chain whenever a player's health drops, once per distinct chain.
using SetHealth_t = void (*)(void* self, float h);
static SetHealth_t g_origSetHealth;
static void hkSetHealth(void* self, float h) {
    if (!g_unhook && self && (self == g_player1 || self == g_player2)) {
        float old = *reinterpret_cast<float*>(static_cast<char*>(self) + 0x8C4);
        if (h < old - 0.5f) {
            void* frames[12];
            USHORT n = RtlCaptureStackBackTrace(1, 12, frames, nullptr);
            uint64_t key = 0;
            for (USHORT i = 0; i < n && i < 6; ++i) key = key * 1000003 ^ reinterpret_cast<uintptr_t>(frames[i]);
            static uint64_t seen[32]; static int seenN;
            bool dup = false;
            for (int i = 0; i < seenN; ++i) dup |= seen[i] == key;
            if (!dup && seenN < 32) {
                seen[seenN++] = key;
                char line[256]; int len = 0;
                for (USHORT i = 0; i < n && len < 220; ++i) {
                    auto a = reinterpret_cast<uintptr_t>(frames[i]);
                    len += a >= g_base && a < g_exeEnd
                               ? snprintf(line + len, sizeof line - len, " %#llx", (unsigned long long)(a - g_base))
                               : snprintf(line + len, sizeof line - len, " mod");
                }
                line[len] = 0;
                logf("[ewcoop] HEALTH DROP %s %.1f -> %.1f, chain:%s\n", self == g_player1 ? "player1" : "player2",
                     old, h, line);
            }
        }
    }
    g_origSetHealth(self, h);
}

static void unhookQueueRemove();  // world item sync section
static void unhookAnimCalls();    // shared enemies section
static void unhookCmdBuffer();    // map change section
static void unhookAll() {
    if (g_subclassedWnd && g_origWndProc)
        SetWindowLongPtrA(g_subclassedWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
    if (g_playAsP2 && g_game) setPlayerSlot(g_game, g_sebastian);
    g_playAsP2 = false;
    if (g_damageHook.slot) writeSlot(g_damageHook.slot, g_damageHook.orig);
    if (g_setHealthHook.slot) writeSlot(g_setHealthHook.slot, g_setHealthHook.orig);
    if (g_thinkHook.slot) writeSlot(g_thinkHook.slot, g_thinkHook.orig);
    if (g_isLocalHook.slot) writeSlot(g_isLocalHook.slot, g_isLocalHook.orig);
    if (g_getPlayerHooked) writeCode(rva_GetPlayer, kGetPlayerHead, sizeof kGetPlayerHead);
    unhookQueueRemove();
    unhookAnimCalls();
    if (g_sneakHooked) { writeCode(0x82F090, kSneakPressedHead, 12); g_sneakHooked = false; }
    writeSlot(g_runFrameHook.slot, g_runFrameHook.orig);
    if (g_actionHook.slot) writeSlot(g_actionHook.slot, g_actionHook.orig);
    logf("[ewcoop] hooks removed\n");
}

// Log any new idLib::Error message (those drop the game to the main menu).
static void checkLastError() {
    static char seen[256];
    auto* text = reinterpret_cast<const char*>(g_base + rva_lastErrorText);
    if (text[0] && strncmp(text, seen, sizeof seen - 1) != 0) {
        strncpy(seen, text, sizeof seen - 1);
        logf("[ewcoop] game error: %s\n", seen);
    }
}

// ================= own netcode: direct UDP player sync (v2) =================
// TEW's leftover MP backend never opens a joinable socket, so we use our own. Each peer runs its own
// game and sends ~30 Hz: its player's position + velocity, real camera view angles and the full movement
// input (usercmd). The receiver drives a local idPlayer (g_player2) with that input, so his own movement
// code produces Sebastian's real walk/sprint/sneak/aim animations, and only nudges his position toward
// the extrapolated remote position when they drift apart (snaps on large desync).
static SOCKET g_netSock = INVALID_SOCKET;
static sockaddr_in g_peerAddr = {};
static bool g_netIsHost, g_netLoopback;
static uint32_t g_netSeq, g_netLastRecvSeq;
static float g_netSendT, g_netLogT, g_netAge;  // g_netAge: seconds since the newest remote packet
static uint32_t g_netRecvCount, g_netDropCount, g_netSnapCount;
static int g_enemyRecvCount, g_enemySnapCount;  // shared-enemy transform sync (section below)
#pragma pack(push, 1)
struct NetPkt {
    uint32_t magic; uint8_t version, type; uint16_t flags; uint32_t seq; uint32_t timeMs;
    float px, py, pz;        // body origin
    float vx, vy, vz;        // velocity (units/s), for extrapolation
    float pitch, yaw;        // camera view angles (degrees), controller +20524
    float health;
    int32_t buttons; int16_t buttons2; int8_t fwd, right, up; uint8_t state;  // state bit0: lantern out
    float bodyYaw;          // body facing (motion object rotation); separate from the camera in third person
    char weapon[48];         // equipped inventory item decl name, e.g. "weapon/pistol" ("" = none)
    char ammo[48];           // that weapon's loaded (or switching-to) idDeclAmmo name, e.g. a crossbow bolt type
    float maxHealth;         // player+0x8C0 (upgrades raise it; the hurt stance follows health / max)
    uint16_t limits;         // script restrictions on the player (kLimitBytes order), e.g. chapter 1 slow walk
};
// Player restriction bytes written by tngTarget_PlayerStateControl_Activate (0x102BBA0) from its PLAYERSTATE_*
// mask: move/walk/run/sprint disabled, weapon fire disabled, and five more. Scripts set them on the real player
// only, so player 2 jogged and drew weapons while the remote couldn't. Synced from the remote.
static const uint16_t kLimitBytes[] = {4780, 4781, 4782, 4783, 4786, 4787, 4789, 4791, 4793, 4797};
#pragma pack(pop)
static const uint32_t kNetMagic = 0x50435745;  // 'EWCP' little-endian
static const uint8_t kNetVersion = 9;        // older peers are ignored (both PCs need this build)

// Body facing lives in the actor motion object (player+1720) as a quaternion at +352 (stored conjugated):
// read with 0x84F8A0 (GetRotation), write with 0x851EB0 (SetFacing, same input as partner Teleport).
static float motionYaw(void* player) {
    void* motion = field<void*>(player, off_partner_facing);
    if (!motion) return 0.f;
    alignas(16) float q[8] = {};
    fn<void* (*)(void*, void*)>(0x84F8A0)(motion, q);
    float x = q[0], y = q[1], z = q[2], w = q[3];
    // The getter returns the conjugated quaternion, so the plain formula gives the negated yaw
    // (log showed bodyFacing == -physYaw for every player).
    return -std::atan2(2.f * (w * z + x * y), 1.f - 2.f * (y * y + z * z)) * 57.2957795f;
}
// Equipped weapon = inventory item in idPlayer slot 2 (vfunc +1656 returns *(player + 2728 + 8*slot)).
// Its name is item->decl(+8)->name(+8). Equip by name with SelectWeapon = idPlayer vtbl +2176 (0x7BB7F0).
static const char* itemName(void* item) {
    if (!item || !readable(item, 16)) return "";
    void* decl = field<void*>(item, 8);
    if (!decl || !readable(decl, 16)) return "";
    const char* name = field<const char*>(decl, 8);
    return name && readable(name, 1) ? name : "";
}
static const char* weaponSlotName(void* player, int slot) {
    return itemName(*reinterpret_cast<void**>(static_cast<char*>(player) + 2728 + 8 * slot));
}
static const char* equippedWeapon(void* player) {
    void* item = *reinterpret_cast<void**>(static_cast<char*>(player) + 2728 + 16);
    if (!item || !readable(item, 16)) return "";
    void* decl = field<void*>(item, 8);
    if (!decl || !readable(decl, 16)) return "";
    const char* name = field<const char*>(decl, 8);
    return name && readable(name, 1) ? name : "";
}
// Ammo of the equipped weapon. The equipped item IS the idWeapon (0x883990(player, 2) returns it):
// +40 = ammoClip (idAmmoItem*, its +8 = idDeclAmmo, +16 = rounds loaded), +48 = nextAmmo (idDeclAmmo* while
// an ammo switch/reload is pending, set by SetNextAmmo 0x7C3140). The pending one wins: it's what fires next.
static const char* weaponAmmo(void* player) {
    void* weapon = *reinterpret_cast<void**>(static_cast<char*>(player) + 2728 + 16);
    if (!weapon || !readable(weapon, 56)) return "";
    void* decl = field<void*>(weapon, 48);
    if (!decl) {
        void* clip = field<void*>(weapon, 40);
        decl = clip && readable(clip, 16) ? field<void*>(clip, 8) : nullptr;
    }
    if (!decl || !readable(decl, 16)) return "";
    const char* name = field<const char*>(decl, 8);
    return name && readable(name, 1) ? name : "";
}
// Lantern in hand = inventory slot 1 holds "zwei/player/lantern" (put away it sits in slot 5; same test as
// 0x7BC960, the game's own "lantern out?" check).
static bool lanternOut(void* player) {
    return strcmp(weaponSlotName(player, 1), "zwei/player/lantern") == 0;
}
// The light itself: the lantern object (0x7BBEC0(player), the entity behind player+4520) has an "off" byte at
// +2656 (1 = dark). The per-frame lantern update 0x934B60 hides/shows the light entities from it. The game flips
// it with 0x947FB0(lantern, 0) = off and 0x947F80(lantern) = on, only from the local player's input path, so
// player 2's light never went out.
static void* lanternObject(void* player) {
    void* l = fn<void* (*)(void*)>(0x7BBEC0)(player);
    return l && readable(l, 2664) ? l : nullptr;
}
static int lanternLit(void* player) {  // -1 = no lantern object
    void* l = lanternObject(player);
    return l ? (field<uint8_t>(l, 2656) == 0 ? 1 : 0) : -1;
}
static void setMotionYaw(void* player, float yaw) {
    void* motion = field<void*>(player, off_partner_facing);
    if (!motion) return;
    Angles a{0.f, yaw, 0.f};
    alignas(16) unsigned char quat[16];
    fn<SetFacing_t>(rva_SetFacing)(motion, fn<AnglesTo_t>(rva_AnglesToQuat)(&a, quat));
}
static const int kNetPort = 28000;
static const int kNetPortRange = 10;  // host uses the first free port in 28000..28009
static bool g_netPeerConfirmed;       // client: the host has answered, g_peerAddr is its real address
static NetPkt g_remotePkt;  // newest received packet
static bool g_p2CamCalc;    // Ctrl+Shift+J: run player 2's camera calc each frame (aim pitch experiment)
// Source-engine style entity interpolation: keep recent snapshots and show the remote player
// kInterpDelay seconds in the past, blending the two snapshots around that time (like cl_interp 0.1).
// Only when the stream runs dry do we extrapolate, for at most kMaxExtrapolate (cl_extrapolate_amount).
static const float kInterpDelay = 0.1f, kMaxExtrapolate = 0.25f;
static NetPkt g_snaps[32];  // ring buffer ordered by seq
static int g_snapCount;
static NetPkt g_view;       // interpolated remote state for this frame
static Vec3 g_sendLastPos;
static float g_sendLastT;
static bool g_sendHaveLast;
static float g_angCorrPitch, g_angCorrYaw;  // closed-loop correction so player 2's view matches the remote's

// Loopback test (Ctrl+Shift+L): our own packets come back after a delay with some loss, and player 2
// replays us 150 units to the side. Tests the whole receive path on one PC.
struct DelayedPkt { NetPkt p; float due; };
static DelayedPkt g_loopQ[64];
static int g_loopCount;
static float g_loopClock;
static const float kLoopDelay = 0.12f, kLoopLoss = 0.05f, kLoopOffset = 150.f;

static char* controllerOf(void* player) {  // idPresentablePlayer: input, view angles (+20524), delta (+19660)
    void* anim = player ? field<void*>(player, off_entity_anim) : nullptr;
    return anim ? vfn<char* (*)(void*)>(anim, slot_anim_GetController)(anim) : nullptr;
}

static void g_steamModeReset();  // Steam transport section
static void netStop() {
    if (g_netSock != INVALID_SOCKET) {
        closesocket(g_netSock);
        g_netSock = INVALID_SOCKET;
        SetEnvironmentVariableA("EWCOOP_SOCKET", nullptr);
    }
    g_netHavePeer = g_remoteValid = g_netStarted = g_netLoopback = false;
    g_loopCount = 0;
    g_steamModeReset();
    logf("[net] stopped\n");
}
static void netResetRemote() {
    g_remoteValid = false;
    g_netLastRecvSeq = 0;
    g_netAge = 0.f;
    g_snapCount = 0;
    g_angCorrPitch = g_angCorrYaw = 0.f;
    g_sendHaveLast = false;
    g_netRecvCount = g_netDropCount = g_netSnapCount = 0;
    g_enemyRecvCount = g_enemySnapCount = 0;
}
static bool netStart(bool host, const char* peerIp) {
    netStop();
    netResetRemote();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { logf("[net] WSAStartup failed\n"); return false; }
    // A socket opened by an older (reloaded) ewcoop in this process would keep the port busy: close it.
    char prev[32];
    if (GetEnvironmentVariableA("EWCOOP_SOCKET", prev, sizeof prev)) {
        SOCKET old = static_cast<SOCKET>(strtoull(prev, nullptr, 10));
        if (old && old != INVALID_SOCKET && closesocket(old) == 0) logf("[net] closed socket left by an older ewcoop\n");
        SetEnvironmentVariableA("EWCOOP_SOCKET", nullptr);
    }
    g_netSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_netSock == INVALID_SOCKET) { logf("[net] socket failed %d\n", WSAGetLastError()); return false; }
    snprintf(prev, sizeof prev, "%llu", static_cast<unsigned long long>(g_netSock));
    SetEnvironmentVariableA("EWCOOP_SOCKET", prev);
    // The host listens on the first free port in 28000..28009 (a stale socket or another program may hold
    // 28000). The client takes any free port and, until the host answers, sends to all ten; then it locks
    // onto the address that answered. The host learns the client from its first packet.
    sockaddr_in local = {};
    local.sin_family = AF_INET; local.sin_addr.s_addr = INADDR_ANY;
    int boundPort = -1;
    for (int i = 0; i < (host ? kNetPortRange : 1); ++i) {
        local.sin_port = htons(host ? (u_short)(kNetPort + i) : 0);
        if (bind(g_netSock, (sockaddr*)&local, sizeof local) == 0) { boundPort = host ? kNetPort + i : 0; break; }
    }
    if (boundPort < 0) {
        logf("[net] bind failed %d (ports %d-%d all busy?)\n", WSAGetLastError(), kNetPort, kNetPort + kNetPortRange - 1);
        closesocket(g_netSock); g_netSock = INVALID_SOCKET; return false;
    }
    u_long nb = 1; ioctlsocket(g_netSock, FIONBIO, &nb);
    g_netIsHost = host; g_netStarted = true; g_netHavePeer = false; g_netPeerConfirmed = false;
    if (!host && peerIp && peerIp[0]) {
        g_peerAddr = {}; g_peerAddr.sin_family = AF_INET; g_peerAddr.sin_port = htons((u_short)kNetPort);
        inet_pton(AF_INET, peerIp, &g_peerAddr.sin_addr);
        g_netHavePeer = true;
        sockaddr_in bound = {};
        int bl = sizeof bound;
        getsockname(g_netSock, (sockaddr*)&bound, &bl);
        logf("[net] CLIENT -> %s ports %d-%d until the host answers (local UDP %d, protocol v%d)\n", peerIp, kNetPort,
             kNetPort + kNetPortRange - 1, ntohs(bound.sin_port), kNetVersion);
    } else if (host) {
        logf("[net] HOST: listening on UDP %d, waiting for peer (protocol v%d)...\n", boundPort, kNetVersion);
    } else {
        logf("[net] CLIENT over Steam (protocol v%d)\n", kNetVersion);
    }
    return true;
}
// ================= Steam transport: lobby + P2P (no VPN, no IPs) =================
// Host: Ctrl+Shift+K also creates a friends-only Steam lobby. Friend: right-click the host in the Steam friends
// list -> "Join Game" (Steam sends GameLobbyJoinRequested); we join the lobby, take its owner as the peer and send
// over Steam P2P (relayed, NAT-proof). UDP + ewhost.txt still works as a fallback.
// The game ships an old steam_api64 (SteamMatchMaking008 / SteamNetworking004): matchmaking is fetched as v009
// from the Steam client (known layout); networking slots 0..3 (send/available/read/accept) are the same in all.
static void* g_steamNet;      // ISteamNetworking
static void* g_steamMM;       // ISteamMatchmaking009
static uint64_t g_steamLobby, g_steamPeer;
static bool g_steamMode;      // current session runs over Steam P2P
static bool g_steamReady;
using RegCb_t = void (*)(void* cb, int id);
using RegCallResult_t = void (*)(void* cb, uint64_t call);
static RegCallResult_t g_steamRegCallResult;
struct SteamCb { void** vtbl; uint8_t flags; int id; };
static void steamOnCallback(SteamCb* self, void* param);
static void steamCbRun(SteamCb* self, void* param) { steamOnCallback(self, param); }
static void steamCbRun3(SteamCb* self, void* param, bool, uint64_t) { steamOnCallback(self, param); }
static int steamCbSize(SteamCb* self) {
    switch (self->id) { case 513: return 16; case 504: return 24; case 333: return 16; case 1202: return 8; default: return 32; }
}
// MSVC puts overloaded virtuals in an order we don't want to guess: both Run slots go to the same handler.
static void* g_steamCbVtbl[3] = {reinterpret_cast<void*>(&steamCbRun3), reinterpret_cast<void*>(&steamCbRun),
                                 reinterpret_cast<void*>(&steamCbSize)};
static SteamCb g_steamCbs[5];  // 513 LobbyCreated, 504 LobbyEnter, 333 GameLobbyJoinRequested, 1202 P2PSessionRequest, 506 LobbyChatUpdate

static bool steamInit() {
    if (g_steamReady) return true;
    HMODULE h = GetModuleHandleA("steam_api64.dll");
    if (!h) { logf("[steam] steam_api64.dll not loaded\n"); return false; }
    auto client = reinterpret_cast<void* (*)()>(GetProcAddress(h, "SteamClient"));
    auto user = reinterpret_cast<int (*)()>(GetProcAddress(h, "SteamAPI_GetHSteamUser"));
    auto pipe = reinterpret_cast<int (*)()>(GetProcAddress(h, "SteamAPI_GetHSteamPipe"));
    auto net = reinterpret_cast<void* (*)()>(GetProcAddress(h, "SteamNetworking"));
    auto reg = reinterpret_cast<RegCb_t>(GetProcAddress(h, "SteamAPI_RegisterCallback"));
    g_steamRegCallResult = reinterpret_cast<RegCallResult_t>(GetProcAddress(h, "SteamAPI_RegisterCallResult"));
    if (!client || !user || !pipe || !net || !reg) { logf("[steam] missing exports\n"); return false; }
    void* c = client();
    g_steamNet = net();
    // ISteamClient slot 10 = GetISteamMatchmaking(hUser, hPipe, version) (same slot in every ISteamClient version)
    if (c) g_steamMM = vfn<void* (*)(void*, int, int, const char*)>(c, 10)(c, user(), pipe(), "SteamMatchMaking009");
    if (!g_steamNet || !g_steamMM) { logf("[steam] interfaces missing (net %p, matchmaking %p)\n", g_steamNet, g_steamMM); return false; }
    static const int ids[5] = {513, 504, 333, 1202, 506};
    for (int i = 0; i < 5; ++i) {
        g_steamCbs[i] = {g_steamCbVtbl, 0, ids[i]};
        reg(&g_steamCbs[i], ids[i]);
    }
    g_steamReady = true;
    char mine[32];
    snprintf(mine, sizeof mine, "%p", static_cast<void*>(g_steamCbs));
    SetEnvironmentVariableA("EWCOOP_STEAM_OWNER", mine);
    logf("[steam] ready (P2P + lobbies; friends can Join Game from the Steam friends list)\n");
    return true;
}
static void steamSend(const void* data, int n) {
    // ISteamNetworking slot 0: SendP2PPacket(steamID, data, size, EP2PSend, channel). Unreliable (0) for the
    // stream, but the first packets go reliable (2): that is what opens the session on the other side.
    static int sent;
    int type = sent < 30 ? 2 : 0;
    bool ok = vfn<bool (*)(void*, uint64_t, const void*, uint32_t, int, int)>(g_steamNet, 0)(
        g_steamNet, g_steamPeer, data, static_cast<uint32_t>(n), type, 0);
    if (++sent <= 3 || (!ok && sent < 50)) logf("[steam] send #%d to %llx: %s\n", sent, (unsigned long long)g_steamPeer, ok ? "ok" : "FAILED");
}
// While a Steam session has no data from the other side yet: what Steam says about it (every 3 s).
// ISteamNetworking slot 5 GetP2PSessionState(steamID, P2PSessionState_t*): {u8 active, connecting, error, relay;
// i32 bytesQueued, i32 packetsQueued, u32 remoteIP, u16 remotePort}.
static void steamDiag(float dt) {
    static float t;
    if (!g_steamMode || !g_steamPeer || !g_steamNet || g_remoteValid) { t = 0.f; return; }
    if ((t -= dt) > 0.f) return;
    t = 3.f;
    uint8_t st[20] = {};
    bool ok = vfn<bool (*)(void*, uint64_t, void*)>(g_steamNet, 5)(g_steamNet, g_steamPeer, st);
    logf("[steam] session with %llx: %s active %d connecting %d error %d relay %d queued %d packets\n",
         (unsigned long long)g_steamPeer, ok ? "found," : "NONE,", st[0], st[1], st[2], st[3],
         *reinterpret_cast<int*>(st + 8));
}
static void netSendRaw(const void* data, int n) {
    if (g_steamMode && g_steamPeer && g_steamNet) { steamSend(data, n); return; }
    if (g_netSock != INVALID_SOCKET) sendto(g_netSock, (const char*)data, n, 0, (sockaddr*)&g_peerAddr, sizeof g_peerAddr);
}
static uint64_t steamLobbyOwner(uint64_t lobby) {
    uint64_t owner = 0;  // ISteamMatchmaking009 slot 35 GetLobbyOwner returns CSteamID (hidden return pointer)
    vfn<void* (*)(void*, uint64_t*, uint64_t)>(g_steamMM, 35)(g_steamMM, &owner, lobby);
    return owner;
}
static void steamCreateLobby() {
    if (!steamInit()) return;
    if (g_steamLobby) vfn<void (*)(void*, uint64_t)>(g_steamMM, 15)(g_steamMM, g_steamLobby);  // LeaveLobby
    g_steamLobby = 0;
    // slot 13 CreateLobby(ELobbyType 1 = friends only, max members) -> SteamAPICall_t. Room for 4: a stale
    // membership from a kicked join filled a 2-slot lobby ("response 4" = full) in the first friend session.
    uint64_t call = vfn<uint64_t (*)(void*, int, int)>(g_steamMM, 13)(g_steamMM, 1, 4);
    if (g_steamRegCallResult && call) g_steamRegCallResult(&g_steamCbs[0], call);
    logf("[steam] creating a friends-only lobby...\n");
}
static void g_steamModeReset() { g_steamMode = false; g_steamPeer = 0; }  // the lobby itself is kept
static void steamLeave() {
    if (g_steamReady && g_steamLobby) vfn<void (*)(void*, uint64_t)>(g_steamMM, 15)(g_steamMM, g_steamLobby);
    g_steamLobby = g_steamPeer = 0;
    g_steamMode = false;
}
static bool netStart(bool host, const char* peerIp);
static void steamOnCallback(SteamCb* self, void* param) {
    // Re-injection leaves older ewcoop copies loaded with their listeners registered: only the newest acts.
    char cur[32] = {}, mine[32];
    snprintf(mine, sizeof mine, "%p", static_cast<void*>(g_steamCbs));
    if (GetEnvironmentVariableA("EWCOOP_STEAM_OWNER", cur, sizeof cur) && strcmp(cur, mine) != 0) return;
    auto* p = static_cast<uint8_t*>(param);
    switch (self->id) {
    case 513: {  // LobbyCreated_t {EResult, uint64 lobby}
        int result = *reinterpret_cast<int*>(p);
        uint64_t lobby = *reinterpret_cast<uint64_t*>(p + 8);
        if (result != 1 || !lobby || lobby == g_steamLobby) {
            if (result != 1) logf("[steam] lobby creation failed (EResult %d)\n", result);
            return;
        }
        g_steamLobby = lobby;
        vfn<bool (*)(void*, uint64_t, const char*, const char*)>(g_steamMM, 20)(g_steamMM, lobby, "ewcoop", "9");
        logf("[steam] lobby %llx ready. Your friend presses Ctrl+Shift+O in game (not Steam's Join Game)\n",
             (unsigned long long)lobby);
        break;
    }
    case 333: {  // GameLobbyJoinRequested_t {lobby, friend}: the friend clicked Join Game
        uint64_t lobby = *reinterpret_cast<uint64_t*>(p);
        logf("[steam] joining the host's lobby %llx...\n", (unsigned long long)lobby);
        vfn<uint64_t (*)(void*, uint64_t)>(g_steamMM, 14)(g_steamMM, lobby);  // JoinLobby
        break;
    }
    case 504: {  // LobbyEnter_t {lobby, perms, locked, response}
        uint64_t lobby = *reinterpret_cast<uint64_t*>(p);
        uint32_t response = *reinterpret_cast<uint32_t*>(p + 16);
        if (response != 1) { logf("[steam] could not enter the lobby (response %u)\n", response); return; }
        g_steamLobby = lobby;
        uint64_t owner = steamLobbyOwner(lobby);
        if (g_netStarted && g_netIsHost) return;  // our own lobby
        if (!netStart(false, nullptr)) return;    // client session; packets go over Steam
        g_steamMode = true;
        g_steamPeer = owner;
        g_netHavePeer = g_netPeerConfirmed = true;
        logf("[steam] in the lobby: connecting to the host %llx over Steam P2P\n", (unsigned long long)owner);
        break;
    }
    case 1202: {  // P2PSessionRequest_t {remote}: the client's first packet
        uint64_t remote = *reinterpret_cast<uint64_t*>(p);
        if (!(g_netStarted && g_netIsHost && g_steamLobby)) return;
        vfn<bool (*)(void*, uint64_t)>(g_steamNet, 3)(g_steamNet, remote);  // AcceptP2PSessionWithUser
        bool changed = g_steamPeer && g_steamPeer != remote;
        g_steamMode = true;
        g_steamPeer = remote;
        g_netHavePeer = g_netPeerConfirmed = true;
        if (changed) netResetRemote();
        logf("[steam] friend %llx connected over Steam P2P\n", (unsigned long long)remote);
        break;
    }
    default: break;
    }
}

// ---- Steam rich presence: "Chapter 1: An Emergency Call — Co-op with <friend>" ----
// ISteamFriends017 (fetched from the Steam client like matchmaking): slot 7 GetFriendPersonaName(steamID),
// slot 43 SetRichPresence(key, value) (verified against isteamfriends.h). "status" is the free-text key.
static void* g_steamFriends;
static const char* const kChapters[15] = {
    "An Emergency Call", "Remnants", "Claws of the Horde", "The Patient", "Inner Recesses",
    "Losing Grip on Ourselves", "The Keeper", "A Planted Seed Will Grow", "The Cruelest Intentions",
    "The Craftsman's Tools", "Reunion", "The Ride", "Casualties", "Ulterior Motives", "An Evil Within"};
static void slugOf(const char* in, char* out, size_t n) {  // "The Craftsman's Tools" -> "the_craftsmans_tools"
    size_t k = 0;
    for (; *in && k + 1 < n; ++in) {
        char ch = static_cast<char>(tolower(static_cast<unsigned char>(*in)));
        if (ch == ' ' || ch == '-') out[k++] = '_';
        else if (isalnum(static_cast<unsigned char>(ch)) || ch == '_') out[k++] = ch;
    }
    out[k] = 0;
}
static void chapterTitle(const char* mapPath, char* out, size_t n) {
    const char* leaf = strrchr(mapPath, '/');
    leaf = leaf ? leaf + 1 : mapPath;
    char slug[96];
    slugOf(leaf, slug, sizeof slug);
    for (int i = 0; i < 15; ++i) {
        char s[96];
        slugOf(kChapters[i], s, sizeof s);
        if (strcmp(s, slug) == 0) { snprintf(out, n, "Chapter %d: %s", i + 1, kChapters[i]); return; }
    }
    // Unknown map: its own name, underscores to spaces, words capitalized.
    size_t k = 0; bool up = true;
    for (; *leaf && k + 1 < n; ++leaf) {
        char ch = *leaf == '_' ? ' ' : *leaf;
        out[k++] = up ? static_cast<char>(toupper(static_cast<unsigned char>(ch))) : ch;
        up = ch == ' ';
    }
    out[k] = 0;
}
static void updateRichPresence(const char* mapPath) {
    if (!g_steamReady) return;
    if (!g_steamFriends) {
        HMODULE h = GetModuleHandleA("steam_api64.dll");
        auto client = reinterpret_cast<void* (*)()>(GetProcAddress(h, "SteamClient"));
        auto user = reinterpret_cast<int (*)()>(GetProcAddress(h, "SteamAPI_GetHSteamUser"));
        auto pipe = reinterpret_cast<int (*)()>(GetProcAddress(h, "SteamAPI_GetHSteamPipe"));
        void* c = client ? client() : nullptr;  // ISteamClient slot 8 = GetISteamFriends(hUser, hPipe, version)
        if (c) g_steamFriends = vfn<void* (*)(void*, int, int, const char*)>(c, 8)(c, user(), pipe(), "SteamFriends017");
        if (!g_steamFriends) return;
    }
    char status[256], chapter[128];
    chapterTitle(mapPath && mapPath[0] ? mapPath : "", chapter, sizeof chapter);
    if (!chapter[0]) strcpy(chapter, "In the menus");
    if (g_netStarted && g_netHavePeer && g_remoteValid) {
        const char* friendName = g_steamPeer
            ? vfn<const char* (*)(void*, uint64_t)>(g_steamFriends, 7)(g_steamFriends, g_steamPeer) : nullptr;
        if (friendName && friendName[0]) snprintf(status, sizeof status, "%s \xE2\x80\x94 Co-op with %s", chapter, friendName);
        else snprintf(status, sizeof status, "%s \xE2\x80\x94 Co-op", chapter);
    } else if (g_netStarted && g_netIsHost) {
        snprintf(status, sizeof status, "%s \xE2\x80\x94 Co-op (waiting for a partner)", chapter);
    } else {
        snprintf(status, sizeof status, "%s", chapter);
    }
    static char last[256];
    if (strcmp(status, last) == 0) return;
    strcpy(last, status);
    vfn<bool (*)(void*, const char*, const char*)>(g_steamFriends, 43)(g_steamFriends, "status", status);
    logf("[steam] rich presence: %s\n", status);
}

// Find a Steam friend who sits in an ewcoop lobby in this game and join it directly (Ctrl+Shift+O). Steam's
// "Join Game" button can't be used: the game's own leftover multiplayer code also handles that request and
// loads the title screen ("kicked to lobby"). ISteamFriends017: slot 3 GetFriendCount(flags), slot 4
// GetFriendByIndex(i, flags) -> CSteamID (hidden return pointer), slot 8 GetFriendGamePlayed(id, FriendGameInfo_t*)
// with FriendGameInfo_t {gameID +0, ip +8, port +12, queryPort +14, steamIDLobby +16}.
static bool steamJoinFriendLobby() {
    if (!steamInit()) return false;
    updateRichPresence("");  // makes sure the friends interface is fetched
    if (!g_steamFriends) return false;
    const int kFlagImmediate = 4;
    int n = vfn<int (*)(void*, int)>(g_steamFriends, 3)(g_steamFriends, kFlagImmediate);
    for (int i = 0; i < n && i < 2000; ++i) {
        uint64_t id = 0;
        vfn<void* (*)(void*, uint64_t*, int, int)>(g_steamFriends, 4)(g_steamFriends, &id, i, kFlagImmediate);
        alignas(8) uint8_t info[32] = {};
        if (!id || !vfn<bool (*)(void*, uint64_t, void*)>(g_steamFriends, 8)(g_steamFriends, id, info)) continue;
        uint64_t gameId = *reinterpret_cast<uint64_t*>(info), lobby = *reinterpret_cast<uint64_t*>(info + 16);
        if ((gameId & 0xFFFFFF) != 268050 || !lobby) continue;
        const char* name = vfn<const char* (*)(void*, uint64_t)>(g_steamFriends, 7)(g_steamFriends, id);
        // Don't enter the lobby: Steam kept refusing it ("response 4", full) in the friend session. The lobby is
        // only for finding the host; P2P goes straight to his Steam ID (he accepts while hosting).
        logf("[steam] %s is hosting (lobby %llx): connecting to him over Steam P2P\n", name ? name : "a friend",
             (unsigned long long)lobby);
        if (!netStart(false, nullptr)) return false;
        g_steamMode = true;
        g_steamPeer = id;
        g_netHavePeer = g_netPeerConfirmed = true;
        return true;
    }
    logf("[steam] no friend is hosting a co-op lobby right now (host: Ctrl+Shift+K first)\n");
    return false;
}

static void netStartLoopback() {
    netStop();
    netResetRemote();
    g_netLoopback = g_netStarted = g_netHavePeer = true;
    if (g_bot) { g_bot = false; logf("[bot] follow-bot OFF (echo test drives player 2)\n"); }
    g_loopCount = 0;
    g_loopClock = 0.f;
    logf("[net] LOOPBACK test: your own packets return after %.0f ms with %.0f%% loss; player 2 replays you %.0f units to the side\n",
         kLoopDelay * 1000.f, kLoopLoss * 100.f, kLoopOffset);
}

static void buildPacket(void* p1, const Usercmd* cmd, float now, NetPkt* p) {
    *p = {};
    p->magic = kNetMagic; p->version = kNetVersion; p->type = 0; p->seq = ++g_netSeq;
    p->timeMs = (uint32_t)(now * 1000.f);
    Vec3 o = originOf(p1);
    p->px = o.x; p->py = o.y; p->pz = o.z;
    if (g_sendHaveLast && now > g_sendLastT) {
        float inv = 1.f / (now - g_sendLastT);
        p->vx = (o.x - g_sendLastPos.x) * inv; p->vy = (o.y - g_sendLastPos.y) * inv; p->vz = (o.z - g_sendLastPos.z) * inv;
    }
    g_sendLastPos = o; g_sendLastT = now; g_sendHaveLast = true;
    if (char* ctrl = controllerOf(p1)) {
        const float* view = reinterpret_cast<const float*>(ctrl + 20524);
        p->pitch = view[0]; p->yaw = view[1];
    } else {
        p->yaw = yawOf(p1);
    }
    if (cmd) {
        p->buttons = cmd->buttons; p->buttons2 = cmd->buttons2;
        p->fwd = cmd->forwardmove; p->right = cmd->rightmove; p->up = cmd->upmove;
    }
    p->bodyYaw = motionYaw(p1);
    p->health = vfn<float (*)(void*)>(p1, 88)(p1);  // GetHealth: player+0x8C4
    p->maxHealth = *reinterpret_cast<const float*>(static_cast<char*>(p1) + 0x8C0);
    strncpy(p->weapon, equippedWeapon(p1), sizeof p->weapon - 1);
    strncpy(p->ammo, weaponAmmo(p1), sizeof p->ammo - 1);
    int lit = lanternLit(p1);
    p->state = (lanternOut(p1) ? 1 : 0) | (lit == 1 ? 2 : 0) | (lit >= 0 ? 4 : 0);  // bit1 lit, bit2 lit is valid
    if (field<uint8_t>(p1, off_player_sneaking)) p->state |= 8;                   // bit3: really sneaking
    p->state |= (field<int>(p1, 3976) & 3) << 4;  // bits4-5: body mode (SetBodyMode slot 242; 1 = unarmed body)
    if (g_meJosephOn) p->state |= 0x40;            // bit6: I play as Joseph (show me as Joseph over there)
    p->limits = 0;
    for (size_t i = 0; i < sizeof kLimitBytes / sizeof kLimitBytes[0]; ++i)
        if (field<uint8_t>(p1, kLimitBytes[i])) p->limits |= 1u << i;
}

static void receivePacket(const NetPkt& in) {
    if (in.magic != kNetMagic) return;
    if (in.version != kNetVersion) {
        static bool warned;
        if (!warned) { warned = true; logf("[net] ignoring protocol v%d packets: both PCs need the same ewcoop build\n", in.version); }
        return;
    }
    // Drop old/duplicate packets (seq wraps after ~4 years at 30 Hz, so plain compare is fine).
    if (g_remoteValid && in.seq <= g_netLastRecvSeq) { ++g_netDropCount; return; }
    if (g_remoteValid && in.seq > g_netLastRecvSeq + 1) g_netDropCount += in.seq - g_netLastRecvSeq - 1;
    g_netLastRecvSeq = in.seq;
    g_remotePkt = in;
    g_remoteValid = true;
    g_netAge = 0.f;
    ++g_netRecvCount;
    if (g_snapCount == 32) {  // drop the oldest
        memmove(g_snaps, g_snaps + 1, sizeof(NetPkt) * 31);
        --g_snapCount;
    }
    g_snaps[g_snapCount++] = in;
}

static float lerpAngle(float a, float b, float t) { return a + angleDiff(b, a) * t; }

// Remote state at (newest sender time + time since it arrived - kInterpDelay).
static void interpolateRemote() {
    const NetPkt& newest = g_snaps[g_snapCount - 1];
    float renderT = newest.timeMs * 0.001f + g_netAge - kInterpDelay;
    // Find the snapshot pair around renderT.
    for (int i = g_snapCount - 1; i > 0; --i) {
        const NetPkt& a = g_snaps[i - 1];
        const NetPkt& b = g_snaps[i];
        float ta = a.timeMs * 0.001f, tb = b.timeMs * 0.001f;
        if (renderT >= ta && renderT <= tb && tb > ta) {
            float t = (renderT - ta) / (tb - ta);
            g_view = t < 0.5f ? a : b;  // buttons / input from the nearer snapshot
            g_view.px = a.px + (b.px - a.px) * t;
            g_view.py = a.py + (b.py - a.py) * t;
            g_view.pz = a.pz + (b.pz - a.pz) * t;
            g_view.vx = a.vx + (b.vx - a.vx) * t;
            g_view.vy = a.vy + (b.vy - a.vy) * t;
            g_view.vz = a.vz + (b.vz - a.vz) * t;
            g_view.yaw = lerpAngle(a.yaw, b.yaw, t);
            g_view.pitch = lerpAngle(a.pitch, b.pitch, t);
            g_view.bodyYaw = lerpAngle(a.bodyYaw, b.bodyYaw, t);
            return;
        }
    }
    if (renderT < g_snaps[0].timeMs * 0.001f) {  // not enough history yet: show the oldest
        g_view = g_snaps[0];
        return;
    }
    // Ahead of the newest snapshot (packets late or lost): extrapolate by velocity, capped.
    float t = std::min(renderT - newest.timeMs * 0.001f, kMaxExtrapolate);
    g_view = newest;
    g_view.px += newest.vx * t;
    g_view.py += newest.vy * t;
    g_view.pz += newest.vz * t;
}

// Drive player 2 from the newest remote packet: input every frame, position only as a correction.
static DWORD g_p2SyncUntil;  // player 2 is in a paired (grab/kill) animation: don't fight it with position fixes
static Vec3 g_p2HoldPos;  // where player 2 last was while the stream was live
static bool g_p2HoldValid;
static void applyRemote(void* gameLocal, float dt) {
    if (!playerAtSlot(gameLocal, 0)) return;  // need a live level
    if (!g_remoteValid) {
        // No stream (other game froze, crashed or is reconnecting): hold player 2 where he was. Left alone he
        // fell through the floor (friend's log: z went to -43512 while waiting to reconnect).
        if (g_player2 && g_p2HoldValid && isA(g_player2, rva_idPlayer_vtable)) {
            Vec3 cur = originOf(g_player2);
            if (std::fabs(cur.z - g_p2HoldPos.z) > 20.f || std::hypot(cur.x - g_p2HoldPos.x, cur.y - g_p2HoldPos.y) > 20.f)
                placePlayer(g_player2, g_p2HoldPos);
        }
        return;
    }
    if (!g_player2) {
        logf("[net] spawning remote player\n");
        spawnSecondPlayer(gameLocal, SpawnSebastian);
        g_angCorrPitch = g_angCorrYaw = 0.f;
        g_p2HoldValid = false;
        if (g_player2) {
            Vec3 start{g_remotePkt.px, g_remotePkt.py, g_remotePkt.pz};
            if (g_netLoopback) start.x += kLoopOffset;
            placePlayer(g_player2, start);
        }
    }
    if (!g_player2 || !isA(g_player2, rva_idPlayer_vtable) || g_snapCount == 0) return;
    interpolateRemote();
    const NetPkt& r = g_view;
    bool stale = g_netAge > kInterpDelay + kMaxExtrapolate;  // lost the stream: stop instead of running off

    // 1) Input. View angles are absolute; the usercmd carries them relative to the player's delta angles.
    Usercmd c = {};
    if (!stale) {
        // Strip weapon/inventory selection buttons: the game's quick-select/weapon-change handling acts on the
        // LOCAL player, so the remote's "2" switched this PC's own player (owner saw it fire the crossbow).
        // Player 2's weapon is synced by name (r.weapon) instead.
        const int32_t kSelectButtons = BUTTON_CHANGEWEAPON | 0x2000 /*WEAP_NEXT*/ | 0x4000 /*WEAP_PREV*/ |
                                       0x8000 | 0x10000 | 0x20000 /*WEAP_0..2*/ | 0x40000 | 0x80000 /*AMMO_PREV/NEXT*/ |
                                       0x100000 | 0x200000 /*QUICK_USE/SELECT*/ | 0x400000 | 0x800000 | 0x1000000 |
                                       0x2000000 /*QUICK_0..3*/ | 0x4000000 /*CYCLE_TARGET*/ | 0x8000000 /*SWITCH_POS*/ |
                                       0x10000000 /*SCOREBOARD*/ | 0x20000000 /*INVENTORY*/;
        c.buttons = r.buttons & ~kSelectButtons & ~BUTTON_LANTERN;
        c.buttons2 = 0;  // NEXT/PREV_QUICK_ITEM
        c.forwardmove = r.fwd; c.rightmove = r.right; c.upmove = r.up;
    }
    // Lantern: the forwarded press never took (player 2 kept it in slot 5), and a lost press would leave the two
    // sides flipped forever. So sync the STATE: when his lantern differs from the remote's, hold his lantern
    // button for a few frames, then wait for the raise/lower animation before checking again.
    {
        static int pressFrames;
        static float cooldown;
        cooldown -= dt;
        bool want = (r.state & 1) != 0, have = lanternOut(g_player2);
        if (pressFrames > 0) {
            c.buttons |= BUTTON_LANTERN;
            --pressFrames;
        } else if (!stale && want != have && cooldown <= 0.f) {
            static int tries;
            if (tries < 20) { ++tries; logf("[net] player 2 lantern %s (pressing his lantern button)\n", want ? "out" : "away"); }
            pressFrames = 4;
            cooldown = 2.f;
            c.buttons |= BUTTON_LANTERN;
        }
        // The light: switch it directly to match (the button above only moves the lantern between slots).
        if (!stale && (r.state & 4)) {
            bool wantLit = (r.state & 2) != 0;
            int haveLit = lanternLit(g_player2);
            if (haveLit >= 0 && (haveLit == 1) != wantLit) {
                void* l = lanternObject(g_player2);
                if (wantLit) fn<void (*)(void*)>(0x947F80)(l);
                else fn<void (*)(void*, char)>(0x947FB0)(l, 0);
                static int logs;
                if (logs < 20) { ++logs; logf("[net] player 2 lantern light %s (now %d)\n", wantLit ? "ON" : "OFF", lanternLit(g_player2)); }
            }
        }
    }
    char* ctrl = controllerOf(g_player2);
    float deltaPitch = 0.f, deltaYaw = 0.f;
    if (ctrl) {
        const float* delta = reinterpret_cast<const float*>(ctrl + 19660);
        deltaPitch = delta[0]; deltaYaw = delta[1];
    }
    c.angles[0] = angleToShort(r.pitch - deltaPitch + g_angCorrPitch);
    c.angles[1] = angleToShort(r.yaw - deltaYaw + g_angCorrYaw);
    c.pitch = c.angles[0]; c.yaw = c.angles[1];
    // Movement speed: his own movement code runs on the remote's stick input, but the remote's game may limit
    // him (chapter 1's forced slow walk, sneaking, scripted walks), so full input made player 2 run/jog here.
    // Match speeds instead: when he moves faster than the remote really does, scale his stick input down (and
    // drop sprint) until they agree. Smaller input = the game's own walk animation.
    {
        static Vec3 lastPos; static bool havePos; static float p2Speed, scale = 1.f;
        Vec3 now = originOf(g_player2);
        if (havePos && dt > 0.f) {
            float inst = std::hypot(now.x - lastPos.x, now.y - lastPos.y) / dt;
            if (inst < 2000.f) p2Speed += (inst - p2Speed) * std::min(1.f, dt * 8.f);
        }
        lastPos = now; havePos = true;
        float remoteSpeed = std::hypot(r.vx, r.vy);
        bool moving = !stale && (c.forwardmove || c.rightmove);
        if (!moving || remoteSpeed < 5.f) scale = std::min(1.f, scale + dt);  // reset when stopped
        else if (p2Speed > remoteSpeed * 1.15f) scale = std::max(0.15f, scale - dt * 1.5f);
        else if (p2Speed < remoteSpeed * 0.9f) scale = std::min(1.f, scale + dt * 1.5f);
        if (scale < 0.98f) {
            c.forwardmove = static_cast<int8_t>(c.forwardmove * scale);
            c.rightmove = static_cast<int8_t>(c.rightmove * scale);
            if (scale < 0.9f) c.buttons &= ~BUTTON_SPRINT;
        }
        static float logT;
        if ((logT -= dt) <= 0.f && moving) {
            logT = 3.f;
            logf("[net] p2 speed %.0f, remote %.0f, input scale %.2f, sneaking remote=%d p2=%d\n", p2Speed, remoteSpeed,
                 scale, (r.state & 8) ? 1 : 0, field<uint8_t>(g_player2, off_player_sneaking));
        }
    }
    // Body mode (player+3976, set by idPlayer vtable slot 242 = 0x7F5A00): 1 = the unarmed "pl0020" body, which
    // detaches all weapon models and makes CanSneak fail (it needs +3976 != 1). Player 2 ended up in 1 while the
    // remote was in 0, so he could never sneak. Match the remote's.
    if (!stale) {
        int want = (r.state >> 4) & 3, have = field<int>(g_player2, 3976);
        static float retry;
        retry -= dt;
        if (want != have && retry <= 0.f) {
            retry = 2.f;
            logf("[net] player 2 body mode %d -> %d (the other player's)\n", have, want);
            vfn<void (*)(void*, int)>(g_player2, 242)(g_player2, want);
        }
    }
    if (!stale) g_remoteIsJoseph = (r.state & 0x40) != 0;
    // Script restrictions (chapter 1 slow walk, no weapons...): copy the remote's player bytes.
    if (!stale) {
        static uint16_t lastLogged = 0xFFFF;
        for (size_t i = 0; i < sizeof kLimitBytes / sizeof kLimitBytes[0]; ++i)
            *(static_cast<uint8_t*>(g_player2) + kLimitBytes[i]) = (r.limits >> i) & 1;
        if (r.limits != lastLogged) {
            lastLogged = r.limits;
            logf("[net] player 2 restrictions from the other game: %#x (run off %d, sprint off %d, fire off %d)\n",
                 r.limits, (r.limits >> 2) & 1, (r.limits >> 3) & 1, (r.limits >> 5) & 1);
        }
    }
    // Sneak is a TOGGLE (0x82F090: sneak button went down this frame). Forwarding the remote's buttons can miss
    // or double a press, so sync the state: when his sneak differs from the remote's for 0.4 s, press it once
    // for him (one frame up, one frame down), then wait for the crouch animation.
    {
        static float mismatch, cooldown;
        static int phase;  // 2: force the button up this frame, 1: down this frame
        bool want = !stale && (r.state & 8);
        bool have = field<uint8_t>(g_player2, off_player_sneaking) != 0;
        cooldown -= dt;
        mismatch = (!stale && want != have) ? mismatch + dt : 0.f;
        if (phase == 0 && mismatch > 0.4f && cooldown <= 0.f && *reinterpret_cast<int*>(g_base + 0x9AC0558) != 0) {
            phase = 2;
            cooldown = 1.5f;
            static int logs;
            if (logs < 20) { ++logs; logf("[net] player 2 sneak %s (pressing his sneak button)\n", want ? "on" : "off"); }
        }
        // idPlayer_UpdateSneak 0x7F8910: cvar int 0x9AC0558 = 0 -> HOLD mode (sneaks while controller buttons
        // +20460 have 0x200 and CanSneak passes), else TOGGLE mode (a press switches it).
        bool holdMode = *reinterpret_cast<int*>(g_base + 0x9AC0558) == 0;
        if (holdMode) {
            phase = 0;
            if (want) c.buttons |= BUTTON_SNEAK; else c.buttons &= ~BUTTON_SNEAK;
        } else if (phase == 2) { c.buttons &= ~BUTTON_SNEAK; phase = 1; }
        else if (phase == 1) { c.buttons |= BUTTON_SNEAK; phase = 0; }
        else c.buttons &= ~BUTTON_SNEAK;  // only our presses toggle him (the remote's own presses would double)
    }
    c.clientGameFrame = ++g_p2Frame;
    fn<PlayerSetUsercmd_t>(rva_PlayerSetUsercmd)(g_player2, &c, &c);
    // Aim pitch: player think 0x8017E0 reads his aim axis (player+4080, drives the upper-body aim) from the
    // controller's camera object at ctrl+576 via the getter 0x829AE0 (axis at +168 -> ctrl+744). Only the local
    // player's camera object follows the view, so player 2 always aimed level ([aimscan]: ctrl+752 = -sin(pitch)
    // for player 1, 0 for player 2). His own view axis (ctrl+20736) is correct, so feed that in.
    if (ctrl && readable(ctrl + 744, 36) && readable(ctrl + 20736, 36)) memcpy(ctrl + 744, ctrl + 20736, 36);
    // Experimental (Ctrl+Shift+J): the controller's view calc 0x85F610 (via 0x88D610(player)) builds the camera
    // origin/axis the aim pose may read; it seems to run only for the local player. Log its effect once.
    if (g_p2CamCalc && ctrl) {
        const float* ax = reinterpret_cast<const float*>(ctrl + 20736);
        float before = ax[2];
        fn<void (*)(void*)>(0x88D610)(g_player2);
        static int logs;
        if (logs < 3) { ++logs; logf("[net] p2 camera calc: fwd z %.2f -> %.2f (view pitch %.1f)\n", before, ax[2], r.pitch); }
    }
    // The remote died and his game reloaded the checkpoint: our player 2 is still lying in the death/kill
    // animation (the replayed Sadist kill left him on the floor). Remove him; the next frame spawns a fresh one
    // at the remote's new position.
    {
        static float lastHealth = 100.f;
        if (!stale) {
            bool revived = lastHealth <= 0.f && r.health > 0.f;
            lastHealth = r.health;
            if (revived) {
                logf("[net] the other player respawned (checkpoint reload): respawning player 2\n");
                void* mgr = field<void*>(gameLocal, off_gameLocal_entMgr);
                if (g_playAsP2) { setPlayerSlot(gameLocal, g_sebastian); g_playAsP2 = false; }
                if (mgr) fn<void (*)(void*, void*, char)>(0x8F3DE0)(mgr, g_player2, 0);
                g_player2 = nullptr;
                g_josephBody = false;
                g_p2SyncUntil = 0;
                g_p2HoldValid = false;
                return;
            }
        }
    }
    // Health: the remote's own game is the authority (his hits land there). Mirror it every frame so his wounded
    // stance/limp shows here and local enemy hits on player 2 don't build up a separate health. Not below 1:
    // his death is decided on his side, not by this copy.
    if (!stale && r.health > 0.f) {
        float& hp = *reinterpret_cast<float*>(static_cast<char*>(g_player2) + 0x8C4);
        static float lastLogged = -1.f;
        if (std::fabs(hp - r.health) > 0.5f && std::fabs(lastLogged - r.health) > 10.f) {
            lastLogged = r.health;
            logf("[net] player 2 health %.0f -> %.0f\n", hp, r.health);
        }
        hp = r.health;
        if (r.maxHealth > 0.f) *reinterpret_cast<float*>(static_cast<char*>(g_player2) + 0x8C0) = r.maxHealth;
    }
    // Body facing: third-person body direction is not derived from the camera, so copy it directly.
    if (!stale) setMotionYaw(g_player2, r.bodyYaw);
    // Weapon: equip what the remote holds (retry a given weapon at most once a second).
    {
        static char lastTried[48];
        static float retryT;
        retryT -= dt;
        char want[48] = {};
        memcpy(want, r.weapon, sizeof want - 1);
        if (want[0] && strcmp(equippedWeapon(g_player2), want) != 0 && (strcmp(lastTried, want) != 0 || retryT <= 0.f)) {
            // Safety net: if equipping player 2 ever changes player 1's real weapon, log it and switch back.
            void* p1 = localPlayer(gameLocal);
            char p1Before[48] = {};
            if (p1) strncpy(p1Before, equippedWeapon(p1), sizeof p1Before - 1);
            auto select = vfn<void (*)(void*, const char*, uint8_t)>(g_player2, 2176 / 8);
            select(g_player2, want, 1);
            if (strcmp(equippedWeapon(g_player2), want) != 0) {
                // He doesn't own it (player 2 starts with the default inventory): give one, then equip.
                // Same calls the save loader uses (0x859620): inventory decl lookup + GiveItem 0x8592A0.
                void* decl = fn<FindEntityDef_t>(rva_FindEntityDef)(reinterpret_cast<void*>(g_base + 0x9A9DDC0), want,
                                                                    1, 1, 0, 0, 0, 0, 0);
                // Owner must be the presentable player (controller): GiveItem reads owner+312 for the player and
                // calls owner->vfunc(+40/+48). Passing the player itself crashed in 0x85A1F0.
                char* owner = controllerOf(g_player2);
                if (decl && owner && field<void*>(owner, 312) == g_player2) {
                    void* inv = static_cast<char*>(g_player2) + 2896;
                    fn<void* (*)(void*, void*, void*, unsigned, char, unsigned char, char)>(0x8592A0)(inv, owner, decl,
                                                                                                    1, 1, 0, 0);
                    select(g_player2, want, 1);
                } else if (decl) {
                    logf("[net] can't give %s: player 2's controller doesn't point back to him\n", want);
                }
            }
            if (p1 && strcmp(equippedWeapon(p1), p1Before) != 0) {
                logf("[net] WARNING: player 2's weapon change also switched player 1 (%s -> %s); switching back\n",
                     p1Before, equippedWeapon(p1));
                if (p1Before[0]) vfn<void (*)(void*, const char*, uint8_t)>(p1, 2176 / 8)(p1, p1Before, 1);
            }
            if (strcmp(lastTried, want) != 0)
                logf("[net] player 2 weapon -> %s (now holding \"%s\")\n", want, equippedWeapon(g_player2));
            strcpy(lastTried, want);
            retryT = 1.f;
        }
    }
    // Ammo type (crossbow bolts): player 2 kept firing his own default bolt (often explosive). When he holds the
    // same weapon as the remote but a different ammo, switch it the way the bolt menu does: ChangeAmmo
    // 0x933430(player, slot 2, weapon 0 = equipped, idDeclAmmo). It needs that ammo in his inventory, so give
    // a few first if he has none (idDeclAmmo is an inventory decl, list 0x9A9E730; GiveItem 0x8592A0).
    {
        static char lastAmmo[48];
        static float ammoRetryT;
        ammoRetryT -= dt;
        char want[48] = {}, wantWeapon[48] = {};
        memcpy(want, r.ammo, sizeof want - 1);
        memcpy(wantWeapon, r.weapon, sizeof wantWeapon - 1);
        char* owner = controllerOf(g_player2);
        if (want[0] && !strcmp(equippedWeapon(g_player2), wantWeapon) && strcmp(weaponAmmo(g_player2), want) != 0 &&
            (strcmp(lastAmmo, want) != 0 || ammoRetryT <= 0.f) && owner && field<void*>(owner, 312) == g_player2) {
            void* decl = fn<FindEntityDef_t>(rva_FindEntityDef)(reinterpret_cast<void*>(g_base + 0x9A9E730), want,
                                                                1, 1, 0, 0, 0, 0, 0);
            if (decl) {
                void* inv = static_cast<char*>(g_player2) + 2896;
                void* have = fn<void* (*)(void*, void*)>(0x858D50)(inv, decl);  // inventory item with this decl
                int rounds = have && readable(have, 20) ? field<int>(have, 16) : 0;
                if (rounds < 3)
                    fn<void* (*)(void*, void*, void*, unsigned, char, unsigned char, char)>(0x8592A0)(inv, owner, decl,
                                                                                                      5, 1, 0, 0);
                fn<void (*)(void*, int, void*, void*)>(0x933430)(g_player2, 2, nullptr, decl);
            }
            if (strcmp(lastAmmo, want) != 0)
                logf("[net] player 2 ammo -> %s (%s; now \"%s\")\n", want, decl ? "found" : "NOT FOUND",
                     weaponAmmo(g_player2));
            strcpy(lastAmmo, want);
            ammoRetryT = 1.f;
        }
    }
    // (closed-loop angle correction removed: it ran away because the view updates a frame later and pitch
    //  is clamped. The angle diagnostics in netRun's log show how cmd, view and delta relate.)

    // 2) Position: the interpolated remote position is the target; his own movement gets him there, we correct drift.
    // Not while he is attached to an enemy in a replayed grab/kill: the bind places him.
    if (g_p2SyncUntil && static_cast<int>(g_p2SyncUntil - GetTickCount()) > 0) return;
    g_p2SyncUntil = 0;
    Vec3 target{r.px, r.py, r.pz};
    if (g_netLoopback) target.x += kLoopOffset;
    Vec3 cur = originOf(g_player2);
    if (!stale) { g_p2HoldPos = target; g_p2HoldValid = true; }
    else if (g_p2HoldValid) target = g_p2HoldPos;  // stream lost: stay put instead of falling
    float ex = target.x - cur.x, ey = target.y - cur.y, ez = target.z - cur.z;
    float err = std::sqrt(ex * ex + ey * ey);
    if (err > 250.f || std::fabs(ez) > 120.f) {
        placePlayer(g_player2, target);  // large desync (teleport, respawn, fell through): snap
        ++g_netSnapCount;
    } else if (err > 6.f) {
        // Small drift: pull a fraction of the error per frame, height left to his own physics.
        float k = 1.f - std::exp(-6.f * dt);
        placePlayer(g_player2, Vec3{cur.x + ex * k, cur.y + ey * k, cur.z});
    }
}

// ================= shared enemies v1: damage replay =================
// Every character class (168 vtables, include/actor_vtables.h) has its idEntity::Damage in slot 80:
//   Damage(self, inflictor, attacker, damageDecl, dmgInfo*, dir*, location*)
// dmgInfo: amount = base(decl) * (+16) + (+12) (0x8705D0); location: 84-byte hit record, joint id at +20.
// When the LOCAL player damages a character we send the hit (target name, decl name, dmgInfo, dir, location);
// the other copy finds the same character by name (map-placed names are identical in both games) and calls its
// Damage with the same data and player 2 as attacker. Deaths follow from the replayed damage.
#pragma pack(push, 1)
struct NetEvt {
    uint32_t magic; uint8_t version, type; uint16_t flags; uint32_t id;
    char target[64];     // entity name (entity +8)
    char decl[64];       // damage decl name (decl +8), looked up in list 0x9A9C170
    uint8_t info[32];    // dmgInfo bytes
    float dir[3];
    uint8_t loc[84];     // hit location record
};
#pragma pack(pop)
enum { kEvtDamage = 1, kEvtPlayerHit = 4, kEvtRemove = 5, kEvtSyncStart = 6, kEvtSyncEnd = 7, kEvtSnapshot = 8,
       kEvtGiveItem = 10, kEvtTrigger = 11, kEvtDeath = 12, kEvtMapChange = 13, kEvtMapInfo = 14,
       kEvtInvRequest = 15, kEvtInvItem = 16, kEvtInvDone = 17,
       kEvtHasInfo = 1,
       kEvtHasDir = 2, kEvtHasLoc = 4, kEvtStore = 8 };
static const uintptr_t kDamageDeclList = 0x9A9C170;
static uint32_t g_evtSeq;
static uint32_t g_evtSeen[512];
static int g_evtSeenPos;
static NetEvt g_evtQueue[256];
static int g_evtQueueCount;
static int g_evtLogBudget = 60;

static const char* entityName(void* e) {
    if (!e || !readable(e, 16)) return nullptr;
    const char* n = field<const char*>(e, 8);
    return n && readable(n, 1) && n[0] ? n : nullptr;
}
static uintptr_t actorDamageOrig(void* self) {
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(self) - g_base;
    for (const auto& s : kActorDamageSlots)
        if (s.vtable == vt) return g_base + s.damage;
    return g_base + 0x9ED6F0;  // idEntity::Damage
}
static void sendEvent(NetEvt& e) {
    if (g_netSock == INVALID_SOCKET || !g_netHavePeer || g_netLoopback) return;
    for (int i = 0; i < 3; ++i)  // no acks yet: send three copies, the receiver dedupes by id
        netSendRaw(&e, sizeof e);
}

// Called by the asm thunk with the original arguments; returns the game's Damage to jump to.
extern "C" void ewNoDamage();  // returns 0 in rax and xmm0 (asm below)
extern "C" uintptr_t ewPreDamage(void* self, void* inflictor, void* attacker, void* decl, void** stackArgs) {
    uintptr_t orig = actorDamageOrig(self);
    // Player 2's own shots in this world would double the remote player's damage: his real hit already
    // arrives as a replayed event. So his local hits on characters do nothing; only the replay applies.
    if (!g_unhook && g_netStarted && !g_applyingNetDamage && g_player2 &&
        (attacker == g_player2 || inflictor == g_player2) && self != g_player1)
        return reinterpret_cast<uintptr_t>(&ewNoDamage);
    if (g_unhook || g_applyingNetDamage || !g_netStarted || !attacker || attacker != g_player1 || self == g_player2)
        return orig;
    const char* target = entityName(self);
    const char* dname = decl && readable(decl, 16) ? field<const char*>(decl, 8) : nullptr;
    if (!target || !dname || !readable(dname, 1)) return orig;
    NetEvt e = {};
    e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtDamage; e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
    strncpy(e.target, target, sizeof e.target - 1);
    strncpy(e.decl, dname, sizeof e.decl - 1);
    void* info = stackArgs[0];
    void* dir = stackArgs[1];
    void* loc = stackArgs[2];
    if (info && readable(info, sizeof e.info)) { memcpy(e.info, info, sizeof e.info); e.flags |= kEvtHasInfo; }
    if (dir && readable(dir, 12)) { memcpy(e.dir, dir, 12); e.flags |= kEvtHasDir; }
    if (loc && readable(loc, sizeof e.loc)) { memcpy(e.loc, loc, sizeof e.loc); e.flags |= kEvtHasLoc; }
    // These records can hold pointers (location +0x30 = hit surface; melee replay crashed in 0x8C587D following
    // it). Pointers mean nothing in the other process, so zero every 8-byte slot that looks like an address;
    // the game null-checks them. Floats, joint ids and flags never look like a canonical user-space pointer.
    auto scrub = [](uint8_t* p, size_t n) {
        for (size_t o = 0; o + 8 <= n; o += 8) {
            uint64_t v;
            memcpy(&v, p + o, 8);
            if (v >= 0x10000 && v < 0x800000000000ull && (v & 7) == 0) memset(p + o, 0, 8);
        }
    };
    scrub(e.info, sizeof e.info);
    scrub(e.loc, sizeof e.loc);
    sendEvent(e);
    if (g_evtLogBudget > 0) {
        --g_evtLogBudget;
        logf("[net] sent hit: %s <- %s (flags %x)\n", e.target, e.decl, e.flags);
    }
    return orig;
}
// Our player was hit (enemy, grenade, fall...): send it so the other copy replays it on player 2 and his body
// plays the same stagger / knockdown / hurt stance there. Same record as an enemy hit, target unused.
static void sendPlayerHit(void* decl, void* info, const float* dir, void* loc) {
    const char* dname = decl && readable(decl, 16) ? field<const char*>(decl, 8) : nullptr;
    if (!dname || !readable(dname, 1)) return;
    NetEvt e = {};
    e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtPlayerHit;
    e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
    strncpy(e.decl, dname, sizeof e.decl - 1);
    if (info && readable(info, sizeof e.info)) { memcpy(e.info, info, sizeof e.info); e.flags |= kEvtHasInfo; }
    if (dir && readable(dir, 12)) { memcpy(e.dir, dir, 12); e.flags |= kEvtHasDir; }
    if (loc && readable(loc, sizeof e.loc)) { memcpy(e.loc, loc, sizeof e.loc); e.flags |= kEvtHasLoc; }
    for (int k = 0; k < 2; ++k) {
        uint8_t* p = k ? e.loc : e.info;
        size_t n = k ? sizeof e.loc : sizeof e.info;
        for (size_t o = 0; o + 8 <= n; o += 8) {
            uint64_t v;
            memcpy(&v, p + o, 8);
            if (v >= 0x10000 && v < 0x800000000000ull && (v & 7) == 0) memset(p + o, 0, 8);
        }
    }
    sendEvent(e);
    if (g_evtLogBudget > 0) { --g_evtLogBudget; logf("[net] sent player hit: %s\n", e.decl); }
}
// x64 thunk: save the 4 register args, call ewPreDamage(self, inflictor, attacker, decl, &stackArgs[a5..a7]),
// restore, then jump to the game's Damage so its return value (float or int) passes through untouched.
extern "C" void ewDamageThunk();
asm(R"(
    .intel_syntax noprefix
    .text
    .globl ewDamageThunk
ewDamageThunk:
    sub rsp, 0x58
    mov [rsp+0x30], rcx
    mov [rsp+0x38], rdx
    mov [rsp+0x40], r8
    mov [rsp+0x48], r9
    lea rax, [rsp+0x58+0x28]
    mov [rsp+0x20], rax
    call ewPreDamage
    mov rcx, [rsp+0x30]
    mov rdx, [rsp+0x38]
    mov r8, [rsp+0x40]
    mov r9, [rsp+0x48]
    add rsp, 0x58
    jmp rax
    .globl ewNoDamage
ewNoDamage:
    xor eax, eax
    xorps xmm0, xmm0
    ret
    .att_syntax prefix
)");

// ================= world item sync: removals =================
// Picking something up (ammo, bolts stuck in walls, syringes, gel, keys...) removes that entity through the
// game's remove queue 0x7A2BC0(gameLocal, ent, store) -> 0x8F3DE0(entityMgr, ent, store) ("add to remove list" /
// "add to store list"). We patch 0x7A2BC0 and, when a prop/item/projectile disappears near our player, tell the
// other game to remove the same one: by level name, else (generated names, e.g. bolts) the nearest entity of the
// same class within 60 units of where it was.
static const unsigned char kRemoveHead[21] = {0x48, 0x8B, 0x49, 0x68, 0x45, 0x85, 0xC0, 0x0F, 0x84, 0x13, 0x12,
                                              0x15, 0x00, 0x41, 0xB0, 0x01, 0xE9, 0x0B, 0x12, 0x15, 0x00};
static const uintptr_t rva_QueueRemove = 0x7A2BC0, rva_MgrQueueRemove = 0x8F3DE0;
static bool g_removeHooked;
static int g_removeLogBudget = 120;

// MSVC RTTI class name of an object, e.g. ".?AVidProp_Usable@@" (vtable[-1] = complete object locator).
static const char* className(void* obj) {
    if (!obj || !readable(obj, 8)) return nullptr;
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(obj);
    if (vt < g_base || vt >= g_exeEnd) return nullptr;
    uintptr_t col = *reinterpret_cast<uintptr_t*>(vt - 8);
    if (col < g_base || col >= g_exeEnd || !readable(reinterpret_cast<void*>(col), 16)) return nullptr;
    uint32_t td = *reinterpret_cast<uint32_t*>(col + 12);
    const char* n = reinterpret_cast<const char*>(g_base + td + 16);
    return readable(n, 4) && n[0] == '.' ? n : nullptr;
}
static bool syncableRemoval(const char* cls) {
    static const char* const kBad[] = {"Enemy", "Npc", "Actor", "Player", "Partner", "Trigger", "Target", "Camera", "Light"};
    static const char* const kOk[] = {"Prop", "Item", "Projectile", "Pickup", "Ammo", "Arrow", "Bolt", "Moveable"};
    if (!cls) return false;
    for (const char* bad : kBad) if (strstr(cls, bad)) return false;
    for (const char* ok : kOk) if (strstr(cls, ok)) return true;
    return false;
}
static bool entityPos(void* e, Vec3& out) {
    void* phys = e && readable(e, off_entity_physics + 8) ? physicsOf(e) : nullptr;
    if (!phys || !readable(phys, 8)) return false;
    out = originOf(e);
    return true;
}
static void hkQueueRemove(void* gameLocal, void* ent, int store) {
    // Only while our player is live in the level (not during level teardown, when he may already be freed).
    // Never dereference the players here: a reload frees them mid-frame while entities are still removed.
    if (!g_unhook && g_netStarted && ent && ent != g_player1 && ent != g_player2 && g_playerPosValid[0] &&
        g_player1Alive) {  // dead = a reload is coming: its cleanup must not remove things in the other game
        const char* cls = className(ent);
        const char* name = entityName(ent);
        Vec3 pos{}, me = g_playerPos[0];
        bool havePos = entityPos(ent, pos);
        float d = havePos ? std::sqrt((pos.x - me.x) * (pos.x - me.x) + (pos.y - me.y) * (pos.y - me.y) +
                                      (pos.z - me.z) * (pos.z - me.z)) : 1e9f;
        bool close = d < 300.f;  // ("near" is a windows.h macro)
        static DWORD windowStart; static int sentInWindow;  // a burst (level script cleanup) is not pickups
        DWORD now = GetTickCount();
        if (now - windowStart > 1000) { windowStart = now; sentInWindow = 0; }
        bool sync = close && name && syncableRemoval(cls) && sentInWindow < 8;
        if (sync) ++sentInWindow;
        if (close && g_removeLogBudget > 0) {
            --g_removeLogBudget;
            logf("[net] removed near me: %s (%s) %.0f units, store %d -> %s\n", name ? name : "?", cls ? cls : "?", d,
                 store, sync ? "sent" : "not synced");
        }
        if (sync) {
            NetEvt e = {};
            e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtRemove;
            e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
            strncpy(e.target, name, sizeof e.target - 1);
            strncpy(e.decl, cls, sizeof e.decl - 1);
            e.dir[0] = pos.x; e.dir[1] = pos.y; e.dir[2] = pos.z;
            e.flags = kEvtHasDir | (store ? kEvtStore : 0);
            sendEvent(e);
        }
    }
    void* mgr = field<void*>(gameLocal, off_gameLocal_entMgr);
    fn<void (*)(void*, void*, char)>(rva_MgrQueueRemove)(mgr, ent, store ? 1 : 0);
}
static void hookQueueRemove() {
    auto* p = reinterpret_cast<unsigned char*>(g_base + rva_QueueRemove);
    bool original = memcmp(p, kRemoveHead, sizeof kRemoveHead) == 0;
    bool ours = p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0;
    if (!original && !ours) { logf("[ewcoop] remove-entity bytes differ, item sync OFF\n"); return; }
    unsigned char jmp[12] = {0x48, 0xB8};
    void* target = reinterpret_cast<void*>(&hkQueueRemove);
    memcpy(jmp + 2, &target, 8);
    jmp[10] = 0xFF; jmp[11] = 0xE0;
    writeCode(rva_QueueRemove, jmp, sizeof jmp);
    g_removeHooked = true;
    logf("[ewcoop] remove-entity hook (item/pickup sync): on\n");
}
// idPlayer_SetAlternateBody (0x7BE610) also sets Sebastian's extra body piece "pl/pl0020.md6" (attached in some
// body states) to "headless": head gone but the neck flesh stays, showing as a bloody stump under Joseph's head.
// While the Joseph look is on, detach that piece; re-activate it when the look goes off.
static void josephExtras(void* player, bool joseph) {
    if (!player) return;
    void* coll = vfn<void* (*)(void*)>(player, slot_actor_Attachments)(player);
    if (!coll || !readable(coll, 16)) return;
    int id = -1;
    fn<void (*)(void*, int*, const char*)>(0x916C30)(coll, &id, "pl/pl0020.md6");  // find attachment id by model
    if (id == -1) return;
    char* entry = static_cast<char*>(fn<void* (*)(void*, int*)>(rva_FindAttachment)(coll, &id));
    if (!entry || !readable(entry, 136)) return;
    bool detached = (entry[128] & 2) != 0;  // DetachAttachment sets flag bit1
    uint32_t uid = static_cast<uint32_t>(id);
    if (joseph && !detached) {
        fn<void (*)(void*, uint32_t*, char)>(rva_DetachAttachment)(coll, &uid, 0);
        logf("[ewcoop] Joseph look: hid Sebastian's extra body piece (neck stump)\n");
    } else if (!joseph && detached) {
        fn<void (*)(void*, uint32_t*)>(rva_ActivateAttachment)(coll, &uid);
    }
}
static void josephNormalVariant(void* player) {
    if (!player) return;
    void* coll = vfn<void* (*)(void*)>(player, slot_actor_Attachments)(player);
    if (!coll || !readable(coll, 16)) return;
    int id = -1;
    fn<void (*)(void*, int*, const char*)>(0x916C30)(coll, &id, "pl/pl9000.md6");  // Joseph's attachment
    if (id == -1) return;
    char* entry = static_cast<char*>(fn<void* (*)(void*, int*)>(rva_FindAttachment)(coll, &id));
    void* obj = entry && readable(entry, 136) ? *reinterpret_cast<void**>(entry) : nullptr;
    void* inner = obj && readable(obj, 8) ? vfn<void* (*)(void*)>(obj, 48 / 8)(obj) : nullptr;
    void* mi = inner ? fn<ModelInstance_t>(rva_ModelInstance)(inner) : nullptr;
    if (mi) fn<SetModelVariant_t>(rva_SetModelVariant)(mi, "normal", nullptr);
}
static void saveJosephChoice() {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\ewjoseph.txt", g_dir);
    // line 1: the other player shown as Joseph (Ctrl+F9); line 2: I play as Joseph (Ctrl+Shift+X)
    if (FILE* f = fopen(path, "w")) { fprintf(f, "%d\n%d\n", g_wantJoseph ? 1 : 0, g_meJoseph ? 1 : 0); fclose(f); }
}
static void loadJosephChoice() {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\ewjoseph.txt", g_dir);
    if (FILE* f = fopen(path, "r")) {
        char l1[8] = {}, l2[8] = {};
        if (fgets(l1, sizeof l1, f)) g_wantJoseph = l1[0] == '1';
        if (fgets(l2, sizeof l2, f)) { g_meJoseph = l2[0] == '1'; g_meJosephChosen = true; }
        fclose(f);
    }
    logf("[ewcoop] the other player looks like %s (Ctrl+F9 switches); you play as %s (Ctrl+Shift+X)\n",
         g_wantJoseph ? "Joseph" : "Sebastian", g_meJoseph ? "Joseph" : "Sebastian");
}
static void unhookQueueRemove() {
    if (g_nataThinkHook.slot) writeSlot(g_nataThinkHook.slot, g_nataThinkHook.orig);
    if (g_restartHook.slot) writeSlot(g_restartHook.slot, g_restartHook.orig);
    unhookCmdBuffer();
    if (g_removeHooked) writeCode(rva_QueueRemove, kRemoveHead, sizeof kRemoveHead);
    g_removeHooked = false;
}
// The other game removed an item: remove ours. Called from applyEvents.
static void applyRemoval(void* gameLocal, const NetEvt& e) {
    void* ent = vfn<void* (*)(void*, const char*)>(gameLocal, 984 / 8)(gameLocal, e.target);
    const char* cls = ent ? className(ent) : nullptr;
    const char* how = "by name";
    if (!ent || !cls || strcmp(cls, e.decl) != 0) {
        // Generated names differ between the games: nearest entity of the same class near that spot.
        ent = nullptr; how = "nearest";
        char* mgr = field<char*>(gameLocal, off_gameLocal_entMgr);
        float best = 60.f * 60.f;
        if (mgr && readable(mgr + 40, 131072 * 8)) {
            auto** list = reinterpret_cast<void**>(mgr + 40);
            for (int i = 0; i < 131072; ++i) {
                void* c = list[i];
                if (!c || c == g_player1 || c == g_player2) continue;
                const char* cc = className(c);
                if (!cc || strcmp(cc, e.decl) != 0) continue;
                Vec3 p;
                if (!entityPos(c, p)) continue;
                float dx = p.x - e.dir[0], dy = p.y - e.dir[1], dz = p.z - e.dir[2];
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < best) { best = d2; ent = c; }
            }
        }
    }
    if (ent) {
        void* mgr = field<void*>(gameLocal, off_gameLocal_entMgr);
        fn<void (*)(void*, void*, char)>(rva_MgrQueueRemove)(mgr, ent, (e.flags & kEvtStore) ? 1 : 0);
    }
    if (g_removeLogBudget > 0) {
        --g_removeLogBudget;
        logf("[net] other game removed %s (%s): %s\n", e.target, e.decl, ent ? how : "not found here");
    }
}

// ================= triggers ignore player 2 =================
// Every trigger / volume touch handler funnels into idVolume_OnTouch 0xA1D040(volume, entity): it logs
// "%s touched by %s" and adds the entity to the volume's inside list (volume+992), which is what fires level
// scripts. Player 2 walking into OUR triggers started cutscenes and moved our real player into scenes our own
// story hadn't reached (saw room without the Sadist). Each game's story now advances only by its own player.
using VolumeTouch_t = int64_t (*)(void* volume, void* ent);
static VolumeTouch_t g_origVolumeTouch;
static const unsigned char kVolumeTouchHead[13] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
                                                   0x48, 0x8B, 0xDA};
// Shared story: when OUR player first enters a trigger, the other game fires the same trigger for ITS player
// (sends the "<touch>" event 0x9B67820, the way physics does), so both games run the same scripts together.
static bool g_applyingTrigger;
static void sendTrigger(const char* name) {
    // dedupe: a volume's touch can repeat while standing in it
    static char recent[16][128]; static DWORD recentT[16]; static int pos;
    for (int i = 0; i < 16; ++i)
        if (strcmp(recent[i], name) == 0 && GetTickCount() - recentT[i] < 5000) return;
    strncpy(recent[pos % 16], name, 127); recentT[pos % 16] = GetTickCount(); ++pos;
    NetEvt e = {};
    e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtTrigger;
    e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
    strncpy(e.target, name, sizeof e.target - 1);                       // name: first 63 chars...
    if (strlen(name) > 63) strncpy(e.decl, name + 63, sizeof e.decl - 1);  // ...rest here
    if (g_playerPosValid[0]) {  // where we stood: a far-away partner is brought here first
        e.dir[0] = g_playerPos[0].x; e.dir[1] = g_playerPos[0].y; e.dir[2] = g_playerPos[0].z;
        e.flags |= kEvtHasDir;
    }
    sendEvent(e);
    logf("[net] I entered trigger %s -> sent\n", name);
}
static int64_t hkVolumeTouch(void* volume, void* ent) {
    if (!g_unhook && ent && ent == g_player2) {
        static int logs;
        if (logs < 20) {
            ++logs;
            const char* vn = entityName(volume);
            logf("[ewcoop] player 2 touched %s: ignored (only our own player triggers our story)\n", vn ? vn : "?");
        }
        return 0;
    }
    bool share = !g_unhook && !g_applyingTrigger && g_netStarted && ent && ent == g_player1 &&
                 !fn<char (*)(void*, void*)>(0xA1D620)(volume, ent);  // first entry only (not already inside)
    int64_t r = g_origVolumeTouch(volume, ent);
    if (share)
        if (const char* vn = entityName(volume)) sendTrigger(vn);
    return r;
}
static void applyTrigger(void* gameLocal, const NetEvt& e) {
    char name[128] = {};
    memcpy(name, e.target, 63);
    strncat(name, e.decl, 63);
    void* volume = vfn<void* (*)(void*, const char*)>(gameLocal, 984 / 8)(gameLocal, name);
    void* p = g_player1;
    const char* result = "fired for my player";
    if (!volume) result = "no such trigger here";
    else if (!p || !g_player1Alive) result = "skipped (I'm dead/loading)";
    else if (fn<char (*)(void*, void*)>(0xA1D620)(volume, p)) result = "skipped (I'm already in it)";
    else if (!vfn<char (*)(void*, void*)>(volume, 0x20 / 8)(volume, reinterpret_cast<void*>(g_base + 0x9B67820)))
        result = "skipped (doesn't respond to touch)";
    else {
        // idEventDef "<touch>" args: 72-byte records {type char, value}; 'e' = toucher (0x840D00 builds it),
        // 'i' = contact id (0 is fine). Touch handlers require the toucher's EF_TOUCHING flag (byte +86 & 0x20).
        // A scripted sequence started by the other player runs from wherever ours stands; far behind, he got
        // stuck walking to it and died. Bring him to where the other player triggered it (co-op catch-up).
        if ((e.flags & kEvtHasDir) && g_playerPosValid[0]) {
            Vec3 at{e.dir[0], e.dir[1], e.dir[2]};
            float dx = at.x - g_playerPos[0].x, dy = at.y - g_playerPos[0].y, dz = at.z - g_playerPos[0].z;
            float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > 600.f) {
                placePlayer(p, at);
                logf("[net] brought my player %.0f units to where the other player triggered it\n", d);
            }
        }
        alignas(16) uint8_t a0[72] = {}, a1[72] = {};
        fn<void (*)(void*, void*)>(0x840D00)(a0, p);
        a1[0] = 'i';
        uint8_t& flags = *(static_cast<uint8_t*>(p) + 86);
        uint8_t saved = flags & 0x20;
        flags |= 0x20;
        g_applyingTrigger = true;
        fn<void (*)(void*, void*, void*, void*)>(0x842460)(volume, reinterpret_cast<void*>(g_base + 0x9B67820), a0, a1);
        g_applyingTrigger = false;
        flags = static_cast<uint8_t>((flags & ~0x20) | saved);
    }
    logf("[net] other player entered trigger %s: %s\n", name, result);
}
static void hookVolumeTouch() {
    g_origVolumeTouch = reinterpret_cast<VolumeTouch_t>(installInlineHook(
        0xA1D040, kVolumeTouchHead, sizeof kVolumeTouchHead, reinterpret_cast<void*>(&hkVolumeTouch)));
    logf("[ewcoop] trigger hook (player 2 doesn't fire our triggers): %s\n", g_origVolumeTouch ? "on" : "OFF");
}

// ================= shared checkpoint restart =================
// The death screen (tngHUD_Dead, update 0xA25E00) ends by calling idGameLocal vtable +136 (slot 17, 0x7A11E0)
// with (0, 0): restart from the last checkpoint. Every death goes through it, scripted kills included (the
// Sadist's chopper kill never sets health to 0). When OUR game restarts, the other game restarts too, so both
// worlds stay at the same checkpoint.
using Restart_t = int64_t (*)(void* gameLocal, int64_t a2, int64_t a3);
static Restart_t g_origRestart;
static bool g_applyingRestart;
static DWORD g_lastRestartTick;  // a restart message arriving right after we restarted is the same death
// A restart deletes every entity mid-frame (inside the game's frame on the dying side, inside our event
// handling on the other). Both copies crashed reading the deleted player 2 later in that frame. Forget every
// level pointer at once; the netcode respawns player 2 when the new level is up.
static void onLevelRestart() {
    g_levelRestarted = true;
    g_player2 = nullptr;
    g_player1 = nullptr;
    g_josephBody = false;
    g_p2Presentable = nullptr;
    g_playerPosValid[0] = g_playerPosValid[1] = false;
    g_player1Alive = false;
    g_p2SyncOwner = g_p1SyncOwner = g_aiForceP2Owner = nullptr;
    g_p2SyncUntil = 0;
    g_p2HoldValid = false;
    SetEnvironmentVariableA("EWCOOP_PLAYER2", nullptr);
}
static int64_t hkRestart(void* gameLocal, int64_t a2, int64_t a3) {
    g_lastRestartTick = GetTickCount();
    // (Not when this restart is the end of a death the other player caused: he already restarted.)
    if (!g_unhook && !g_applyingRestart && !g_diedFromRemote && g_netStarted && g_netHavePeer) {
        NetEvt e = {};
        e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtDeath;
        e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
        sendEvent(e);
        logf("[net] my game restarts from the checkpoint (%lld, %lld) -> the other game restarts too\n",
             (long long)a2, (long long)a3);
    }
    int64_t r = g_origRestart(gameLocal, a2, a3);
    onLevelRestart();
    return r;
}
static void hookRestart() {
    if (hookSlot(g_restartHook, rva_idGameLocal_vtable, 136 / 8, 0x7A11E0, reinterpret_cast<void*>(&hkRestart)))
        g_origRestart = reinterpret_cast<Restart_t>(g_restartHook.orig);
    else
        g_restartHook.slot = nullptr;
    logf("[ewcoop] checkpoint-restart hook (shared death): %s\n", g_origRestart ? "on" : "OFF");
}

// ================= shared map / chapter changes =================
// Map changes run the console command `nextmap "<map>" "<checkpoint>" "<layers>" "<world>"` (handler 0x47A00):
// idTarget_LevelTransition (0xA275F0) formats it and queues it through the command system (object pointer at
// 0x1D6A870, vtable slot 7 = queue command text). We hook that slot: when OUR game queues a nextmap, the other game
// queues the same text, so both load the same map at the same checkpoint the game's own way.
// NetEvt text field: target(64) + decl(64) + loc(84) = up to 211 chars.
using CmdBuffer_t = void (*)(void* cmdSystem, const char* text);
static CmdBuffer_t g_origCmdBuffer;
static void** g_cmdBufferSlot;
static bool g_applyingCmd;
static char g_lastNextmap[212];  // our last nextmap command (sent to a joining partner)
static void packText(NetEvt& e, const char* text) {
    char buf[212] = {};
    strncpy(buf, text, sizeof buf - 1);
    memcpy(e.target, buf, 63);
    memcpy(e.decl, buf + 63, 63);
    memcpy(e.loc, buf + 126, 84);
}
static void unpackText(const NetEvt& e, char* out /*212*/) {
    memset(out, 0, 212);
    memcpy(out, e.target, 63);
    memcpy(out + 63, e.decl, 63);
    memcpy(out + 126, e.loc, 84);
}
static bool isNextmap(const char* t) {
    while (*t == ' ' || *t == '\t') ++t;
    return _strnicmp(t, "nextmap", 7) == 0;
}
static void hkCmdBuffer(void* sys, const char* text) {
    if (!g_unhook && text && isNextmap(text) && !g_applyingCmd) {
        strncpy(g_lastNextmap, text, sizeof g_lastNextmap - 1);
        if (g_netStarted && g_netHavePeer) {
            NetEvt e = {};
            e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtMapChange;
            e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
            packText(e, text);
            sendEvent(e);
        }
        logf("[net] my game changes map: %s -> sent\n", text);
    }
    g_origCmdBuffer(sys, text);
}
static void hookCmdBuffer() {
    void* sys = *reinterpret_cast<void**>(g_base + 0x1D6A870);
    if (!sys || !readable(sys, 8)) { logf("[ewcoop] command system not found, map sync OFF\n"); return; }
    void** vt = *reinterpret_cast<void***>(sys);
    g_cmdBufferSlot = vt + 7;
    void* cur = *g_cmdBufferSlot;
    auto c = reinterpret_cast<uintptr_t>(cur);
    if (c < g_base || c >= g_exeEnd) {  // an older ewcoop's hook: chain to the game's function it saved
        char prev[32];
        if (GetEnvironmentVariableA("EWCOOP_CMDBUF_ORIG", prev, sizeof prev)) cur = reinterpret_cast<void*>(strtoull(prev, nullptr, 16));
    } else {
        char v[32]; snprintf(v, sizeof v, "%p", cur);
        SetEnvironmentVariableA("EWCOOP_CMDBUF_ORIG", v);
    }
    g_origCmdBuffer = reinterpret_cast<CmdBuffer_t>(cur);
    writeSlot(g_cmdBufferSlot, reinterpret_cast<void*>(&hkCmdBuffer));
    logf("[ewcoop] map-change hook (nextmap shared): on\n");
}
static void unhookCmdBuffer() {
    if (g_cmdBufferSlot && g_origCmdBuffer) writeSlot(g_cmdBufferSlot, reinterpret_cast<void*>(g_origCmdBuffer));
}
static const char* currentMapName(void* gameLocal) {  // idGameLocal +0xC8 idStr (data pointer at +0xD0)
    const char* n = gameLocal && readable(static_cast<char*>(gameLocal) + 0xD0, 8) ? field<const char*>(gameLocal, 0xD0) : nullptr;
    return n && readable(n, 1) ? n : "";
}
static char g_hostMap[128], g_hostNextmap[212];  // client: what the host is on
static void applyMapEvent(void* gameLocal, const NetEvt& e) {
    char text[212];
    unpackText(e, text);
    if (e.type == kEvtMapChange) {
        if (!g_origCmdBuffer || !isNextmap(text)) return;
        logf("[net] the other player changes map: %s -> doing the same\n", text);
        g_applyingCmd = true;
        g_origCmdBuffer(*reinterpret_cast<void**>(g_base + 0x1D6A870), text);
        g_applyingCmd = false;
        strncpy(g_lastNextmap, text, sizeof g_lastNextmap - 1);
        return;
    }
    // kEvtMapInfo (host, every 5 s): target = map name, rest = the host's last nextmap command (may be empty)
    char map[64] = {};
    memcpy(map, e.target, 63);
    const char* rest = text + 63;
    strncpy(g_hostMap, map, sizeof g_hostMap - 1);
    strncpy(g_hostNextmap, rest, sizeof g_hostNextmap - 1);
    const char* mine = currentMapName(gameLocal);
    static char warnedFor[64];
    if (map[0] && strcmp(map, mine) != 0 && strcmp(warnedFor, map) != 0) {
        strncpy(warnedFor, map, sizeof warnedFor - 1);
        logf("[net] WARNING: the host is on map \"%s\", you are on \"%s\". Load the same chapter/checkpoint, or press "
             "Ctrl+Shift+M to go to the host's map%s\n", map, mine, g_hostNextmap[0] ? " and checkpoint" : "");
    }
}
static void sendMapInfo(void* gameLocal) {
    NetEvt e = {};
    e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtMapInfo;
    e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
    char buf[212] = {};
    strncpy(buf, currentMapName(gameLocal), 62);
    strncpy(buf + 63, g_lastNextmap, sizeof buf - 64);
    memcpy(e.target, buf, 63); memcpy(e.decl, buf + 63, 63); memcpy(e.loc, buf + 126, 84);
    sendEvent(e);
}
static void goToHostMap() {
    char cmd[256];
    if (g_hostNextmap[0]) strncpy(cmd, g_hostNextmap, sizeof cmd - 1);
    else if (g_hostMap[0]) snprintf(cmd, sizeof cmd, "nextmap \"%s\" \"\" \"0\" \"0\"\n", g_hostMap);
    else { logf("[net] Ctrl+Shift+M: don't know the host's map yet\n"); return; }
    logf("[net] going to the host's map: %s\n", cmd);
    g_applyingCmd = true;
    g_origCmdBuffer(*reinterpret_cast<void**>(g_base + 0x1D6A870), cmd);
    g_applyingCmd = false;
}

// ================= visiting player: protected saves, co-op character, joining from the menu =================
// The host's game is the co-op world (the host plays his own campaign). The VISITOR (client) plays a separate
// co-op character: his inventory is kept in ewcoop_character.txt next to the DLL, never in his campaign save,
// and his game writes no saves while connected (a NG+ campaign stays out of a fresh co-op world and a chapter-1
// co-op session can't overwrite his chapter-9 save). First time: he starts with a copy of the host's inventory.

// -- saves: idSaveGameProcessorRageSave begin-save 0x7DB860(processor, name, player, spot) -> 1 ok / 0 fail.
// Returning 0 makes both callers (checkpoint autosave 0x8FF030, quicksave 0x901660) bail out cleanly: no file,
// no "saving" callback, and the in-memory checkpoint used by restart-on-death is untouched.
using BeginSave_t = char (*)(void* proc, const char* name, void* player, const char* spot);
static BeginSave_t g_origBeginSave;
static const unsigned char kBeginSaveHead[12] = {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1};
static char hkBeginSave(void* proc, const char* name, void* player, const char* spot) {
    if (!g_unhook && g_netStarted && !g_netIsHost && !g_netLoopback) {
        static int logs;
        if (logs < 10) { ++logs; logf("[coop] save skipped (%s): visiting a co-op game, your campaign save is untouched\n", name ? name : "?"); }
        return 0;
    }
    return g_origBeginSave(proc, name, player, spot);
}

// -- inventory: idList<idInventoryItem*> at player+2896 ({+0 array, +8 count}); item +8 decl (decl +8 name),
// +16 count. Set count 0x7C5960(item, value, 0) (clamped to capacity). Find by name 0x858D90(inv, name).
// Upgrade levels (from GetCurrentUpgradeLevel 0x957A30 and its per-category getters): player dwords at
// +0x8B20 health, +0x8B28 stamina, +0x8B38 melee, +0x8B3C syringe, +0x8B40/+0x8B44/+0x8B4C (cat 14-16); 12 dwords
// +0x8B20..+0x8B4C are kept as one block. Weapon upgrades: the weapon's inventory item dwords +0x6C..+0x90.
struct InvEntry { char name[64]; int count; bool hasUpg; int upg[10]; };
static const int kInvMax = 160;
static const int kPlayerUpgOff = 0x8B20, kPlayerUpgCount = 12, kItemUpgOff = 0x6C, kItemUpgCount = 10;
static void readPlayerUpgrades(void* p, int* out) { memcpy(out, static_cast<char*>(p) + kPlayerUpgOff, kPlayerUpgCount * 4); }
static void writePlayerUpgrades(void* p, const int* in) { memcpy(static_cast<char*>(p) + kPlayerUpgOff, in, kPlayerUpgCount * 4); }
static int readInventory(void* player, InvEntry* out) {
    char* inv = static_cast<char*>(player) + 2896;
    auto** items = *reinterpret_cast<char***>(inv);
    int n = *reinterpret_cast<int*>(inv + 8), k = 0;
    if (!items || n <= 0 || n > 1000 || !readable(items, n * 8)) return 0;
    for (int i = 0; i < n && k < kInvMax; ++i) {
        char* it = items[i];
        if (!it || !readable(it, 24)) continue;
        void* decl = *reinterpret_cast<void**>(it + 8);
        const char* dn = decl && readable(decl, 16) ? field<const char*>(decl, 8) : nullptr;
        if (!dn || !readable(dn, 1) || !dn[0]) continue;
        strncpy(out[k].name, dn, 63); out[k].name[63] = 0;
        out[k].count = *reinterpret_cast<int*>(it + 16);
        out[k].hasUpg = strncmp(dn, "weapon/", 7) == 0 && readable(it + kItemUpgOff, kItemUpgCount * 4);
        if (out[k].hasUpg) memcpy(out[k].upg, it + kItemUpgOff, kItemUpgCount * 4);
        ++k;
    }
    return k;
}
static bool isEssentialItem(const char* n) {  // never zeroed: fists, lantern, the map's key/story items
    return strstr(n, "fists") || strstr(n, "lantern") || strstr(n, "key") || strstr(n, "map") || strstr(n, "memo");
}
static void applyInventory(void* player, const InvEntry* want, int nWant) {
    char* inv = static_cast<char*>(player) + 2896;
    char* owner = controllerOf(player);
    InvEntry have[kInvMax];
    int nHave = readInventory(player, have);
    int set = 0, given = 0, zeroed = 0;
    for (int i = 0; i < nWant; ++i) {
        char* it = static_cast<char*>(fn<void* (*)(void*, const char*)>(0x858D90)(inv, want[i].name));
        if (it) {
            fn<void (*)(void*, int, char)>(0x7C5960)(it, want[i].count, 0);
            if (want[i].hasUpg) memcpy(it + kItemUpgOff, want[i].upg, kItemUpgCount * 4);
            ++set;
            continue;
        }
        void* decl = fn<FindEntityDef_t>(rva_FindEntityDef)(reinterpret_cast<void*>(g_base + 0x9A9DDC0), want[i].name, 1, 1, 0, 0, 0, 0, 0);
        if (!decl) decl = fn<FindEntityDef_t>(rva_FindEntityDef)(reinterpret_cast<void*>(g_base + 0x9A9E730), want[i].name, 1, 1, 0, 0, 0, 0, 0);
        if (decl && owner && field<void*>(owner, 312) == player && want[i].count > 0) {
            g_applyingGive = true;
            fn<void* (*)(void*, void*, void*, unsigned, char, unsigned char, char)>(0x8592A0)(inv, owner, decl, want[i].count, 1, 0, 0);
            g_applyingGive = false;
            if (want[i].hasUpg)
                if (char* ni = static_cast<char*>(fn<void* (*)(void*, const char*)>(0x858D90)(inv, want[i].name)))
                    memcpy(ni + kItemUpgOff, want[i].upg, kItemUpgCount * 4);
            ++given;
        }
    }
    for (int i = 0; i < nHave; ++i) {  // things the co-op character doesn't have: counts to 0 (weapons stay, see below)
        bool keep = false;
        for (int j = 0; j < nWant && !keep; ++j) keep = strcmp(have[i].name, want[j].name) == 0;
        if (keep || isEssentialItem(have[i].name) || strncmp(have[i].name, "weapon/", 7) == 0) continue;
        if (char* it = static_cast<char*>(fn<void* (*)(void*, const char*)>(0x858D90)(inv, have[i].name))) {
            fn<void (*)(void*, int, char)>(0x7C5960)(it, 0, 0);
            ++zeroed;
        }
    }
    logf("[coop] co-op character applied: %d items set, %d given, %d emptied (weapons you own but the co-op "
         "character doesn't are kept for now)\n", set, given, zeroed);
}
static void characterPath(char* out, size_t n) { snprintf(out, n, "%s\\ewcoop_character.txt", g_dir); }
static int parseInts(const char* s, int* out, int max) {
    int k = 0;
    while (*s && k < max) {
        out[k++] = static_cast<int>(strtol(s, const_cast<char**>(&s), 10));
        while (*s == ',' || *s == ' ') ++s;
        if (*s == '\r' || *s == '\n') break;
    }
    return k;
}
static int loadCharacter(InvEntry* out, int* playerUpg, bool* haveUpg) {
    char path[MAX_PATH]; characterPath(path, sizeof path);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int k = 0; char line[400];
    *haveUpg = false;
    while (k < kInvMax && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (strcmp(line, "player_upgrades") == 0) {
            *haveUpg = parseInts(tab + 1, playerUpg, kPlayerUpgCount) == kPlayerUpgCount;
            continue;
        }
        strncpy(out[k].name, line, 63); out[k].name[63] = 0;
        out[k].count = atoi(tab + 1);
        char* tab2 = strchr(tab + 1, '\t');
        out[k].hasUpg = tab2 && parseInts(tab2 + 1, out[k].upg, kItemUpgCount) == kItemUpgCount;
        ++k;
    }
    fclose(f);
    return k;
}
static void saveCharacter(void* player) {
    InvEntry inv[kInvMax];
    int n = readInventory(player, inv);
    if (n <= 0) return;
    int upg[kPlayerUpgCount];
    readPlayerUpgrades(player, upg);
    char path[MAX_PATH]; characterPath(path, sizeof path);
    if (FILE* f = fopen(path, "w")) {
        fprintf(f, "# EvilWithThem co-op character: inventory + upgrade levels. Delete it to start over from the host's.\n");
        fprintf(f, "player_upgrades\t");
        for (int i = 0; i < kPlayerUpgCount; ++i) fprintf(f, i ? ",%d" : "%d", upg[i]);
        fprintf(f, "\n");
        for (int i = 0; i < n; ++i) {
            fprintf(f, "%s\t%d", inv[i].name, inv[i].count);
            if (inv[i].hasUpg) {
                fprintf(f, "\t");
                for (int j = 0; j < kItemUpgCount; ++j) fprintf(f, j ? ",%d" : "%d", inv[i].upg[j]);
            }
            fprintf(f, "\n");
        }
        fclose(f);
    }
}
// Session state (client side).
static bool g_coopCharApplied;       // this session's character is on our player
static bool g_coopCharRequested;     // asked the host for his inventory (first time)
static InvEntry g_hostInv[kInvMax];  // the host's inventory as it arrives
static int g_hostInvCount;
static float g_hostInvDoneAt = -1.f; // apply a moment after the "done" marker (UDP may reorder)
static int g_hostPlayerUpg[kPlayerUpgCount];
static float g_coopClock;
static void coopSendInventory(void* player) {  // host: answer a request, one event per item
    InvEntry inv[kInvMax];
    int n = readInventory(player, inv);
    for (int i = 0; i < n; ++i) {
        NetEvt e = {};
        e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtInvItem;
        e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
        strncpy(e.target, inv[i].name, 63);
        memcpy(e.info, &inv[i].count, 4);
        if (inv[i].hasUpg) { e.flags |= 1; memcpy(e.loc, inv[i].upg, kItemUpgCount * 4); }
        sendEvent(e);
    }
    NetEvt d = {};
    d.magic = kNetMagic; d.version = kNetVersion; d.type = kEvtInvDone;
    d.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
    memcpy(d.info, &n, 4);
    readPlayerUpgrades(player, reinterpret_cast<int*>(d.loc));  // 12 dwords = 48 of the 84 bytes
    sendEvent(d);
    logf("[coop] sent my inventory (%d items) to the visiting player\n", n);
}
static void coopEvent(const NetEvt& e) {
    if (e.type == kEvtInvRequest) { if (g_netIsHost && g_player1 && g_player1Alive) coopSendInventory(g_player1); return; }
    if (g_netIsHost) return;
    if (e.type == kEvtInvItem) {
        char name[64] = {}; memcpy(name, e.target, 63);
        int cnt; memcpy(&cnt, e.info, 4);
        InvEntry* slot = nullptr;
        for (int i = 0; i < g_hostInvCount && !slot; ++i)
            if (strcmp(g_hostInv[i].name, name) == 0) slot = &g_hostInv[i];
        if (!slot && g_hostInvCount < kInvMax) { slot = &g_hostInv[g_hostInvCount++]; strcpy(slot->name, name); }
        if (!slot) return;
        slot->count = cnt;
        slot->hasUpg = (e.flags & 1) != 0;
        if (slot->hasUpg) memcpy(slot->upg, e.loc, kItemUpgCount * 4);
    } else if (e.type == kEvtInvDone) {
        memcpy(g_hostPlayerUpg, e.loc, kPlayerUpgCount * 4);
        g_hostInvDoneAt = g_coopClock + 1.f;
    }
}
// Per frame (client): apply the character once per session; keep the file up to date.
static void coopCharacterTick(void* gameLocal, float dt) {
    g_coopClock += dt;
    static bool wasSession;
    bool session = g_netStarted && !g_netIsHost && !g_netLoopback && g_netHavePeer && g_remoteValid;
    if (wasSession && !session) {  // left (or lost) the session: forget, the next join applies again
        g_coopCharApplied = g_coopCharRequested = false;
        g_hostInvCount = 0; g_hostInvDoneAt = -1.f;
    }
    wasSession = session;
    if (!session || !g_player1 || !g_player1Alive || !playerAtSlot(gameLocal, 0)) return;
    if (!g_coopCharApplied) {
        static InvEntry mine[kInvMax];
        int upg[kPlayerUpgCount]; bool haveUpg = false;
        int n = loadCharacter(mine, upg, &haveUpg);
        if (n >= 0) {
            applyInventory(g_player1, mine, n);
            if (haveUpg) writePlayerUpgrades(g_player1, upg);
            g_coopCharApplied = true;
            logf("[coop] your co-op character is back (%d items%s from ewcoop_character.txt)\n", n,
                 haveUpg ? " + upgrade levels" : "");
        } else if (!g_coopCharRequested) {
            g_coopCharRequested = true;
            NetEvt r = {};
            r.magic = kNetMagic; r.version = kNetVersion; r.type = kEvtInvRequest;
            r.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
            sendEvent(r);
            logf("[coop] first co-op game: asking the host for a starting inventory\n");
        } else if (g_hostInvDoneAt >= 0.f && g_coopClock >= g_hostInvDoneAt) {
            applyInventory(g_player1, g_hostInv, g_hostInvCount);
            writePlayerUpgrades(g_player1, g_hostPlayerUpg);
            saveCharacter(g_player1);
            g_coopCharApplied = true;
            logf("[coop] new co-op character created from the host's inventory (%d items)\n", g_hostInvCount);
        }
        return;
    }
    static float saveT;
    if ((saveT -= dt) <= 0.f) { saveT = 30.f; saveCharacter(g_player1); }
}

// -- joining from the main menu: nextmap needs a running map, so load the host's map with `slowmap` first, then
// jump to his checkpoint with his last nextmap.
static int g_menuJoinStage;  // 0 idle, 1 map load issued, 2 checkpoint jump issued
static bool inMenu(void* gameLocal) {
    const char* m = currentMapName(gameLocal);
    return !playerAtSlot(gameLocal, 0) || !m[0] || strstr(m, "menu");
}
static void queueCommand(const char* text) {
    if (!g_origCmdBuffer) return;
    g_applyingCmd = true;
    g_origCmdBuffer(*reinterpret_cast<void**>(g_base + 0x1D6A870), text);
    g_applyingCmd = false;
}
static void menuJoinTick(void* gameLocal) {
    // Only for a join made FROM THE MAIN MENU: the decision is taken once, when the host's map is first known.
    // "No player loaded" is also true on the death screen / reload, which made this reload the host's map after
    // a death and kicked the joiner out (first friend session).
    static bool decided;
    if (!g_netStarted || g_netIsHost) { g_menuJoinStage = 0; decided = false; return; }
    if (!g_hostMap[0]) return;
    if (!decided) {
        decided = true;
        bool menu = inMenu(gameLocal) && GetTickCount() - g_lastRestartTick > 30000;
        if (!menu) g_menuJoinStage = 2;  // joined from inside a level: never auto-load (Ctrl+Shift+M does it by hand)
    }
    if (g_menuJoinStage == 0) {
        char cmd[200];
        snprintf(cmd, sizeof cmd, "slowmap \"%s\"\n", g_hostMap);
        logf("[coop] in the menu: loading the host's map (%s)\n", cmd);
        queueCommand(cmd);
        g_menuJoinStage = 1;
    } else if (g_menuJoinStage == 1 && !inMenu(gameLocal) && strcmp(currentMapName(gameLocal), g_hostMap) == 0 &&
               g_player1Alive) {
        if (g_hostNextmap[0]) {
            logf("[coop] on the host's map: jumping to his checkpoint (%s)\n", g_hostNextmap);
            queueCommand(g_hostNextmap);
        }
        g_menuJoinStage = 2;
    }
}

// ================= shared key items =================
// Picking up a key removes it from the other world too (item sync), so the other player could never open that
// door. Inventory GiveItem 0x8592A0(inventory, owner, decl, count, ...): when OUR player gets a key item, the
// other game gives the same item to its own player.
using GiveItem_t = void* (*)(void* inv, void* owner, void* decl, unsigned count, char a5, unsigned char a6, char a7);
static GiveItem_t g_origGiveItem;
static const unsigned char kGiveItemHead[14] = {0x40, 0x55, 0x56, 0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x38,
                                                0x45, 0x8B, 0xF9};
static int g_giveLogBudget = 60;
static bool sharedItem(const char* n) {
    char low[96] = {};
    for (int i = 0; n[i] && i < 95; ++i) low[i] = static_cast<char>(tolower(static_cast<unsigned char>(n[i])));
    return strstr(low, "key") != nullptr;
}
static void* hkGiveItem(void* inv, void* owner, void* decl, unsigned count, char a5, unsigned char a6, char a7) {
    if (!g_unhook && !g_applyingGive && g_player1 && inv == static_cast<char*>(g_player1) + 2896 && decl &&
        readable(decl, 16)) {
        const char* dn = field<const char*>(decl, 8);
        if (dn && readable(dn, 1)) {
            bool share = g_netStarted && sharedItem(dn);
            if (g_giveLogBudget > 0) { --g_giveLogBudget; logf("[net] I got item %s x%u%s\n", dn, count, share ? " -> shared with the other player" : ""); }
            if (share) {
                NetEvt e = {};
                e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtGiveItem;
                e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
                strncpy(e.decl, dn, sizeof e.decl - 1);
                e.info[0] = static_cast<uint8_t>(count > 255 ? 255 : count);
                sendEvent(e);
            }
        }
    }
    return g_origGiveItem(inv, owner, decl, count, a5, a6, a7);
}
static void applyGiveItem(const NetEvt& e) {
    void* decl = fn<FindEntityDef_t>(rva_FindEntityDef)(reinterpret_cast<void*>(g_base + 0x9A9DDC0), e.decl, 1, 1,
                                                        0, 0, 0, 0, 0);
    char* owner = g_player1 ? controllerOf(g_player1) : nullptr;
    bool ok = decl && owner && field<void*>(owner, 312) == g_player1 && g_origGiveItem;
    if (ok) {
        // Already have it (both picked one up, or a resend): don't double it.
        void* have = fn<void* (*)(void*, void*)>(0x858D50)(static_cast<char*>(g_player1) + 2896, decl);
        if (have) { logf("[net] other player got %s; I already have it\n", e.decl); return; }
        g_applyingGive = true;
        g_origGiveItem(static_cast<char*>(g_player1) + 2896, owner, decl, e.info[0] ? e.info[0] : 1, 1, 0, 0);
        g_applyingGive = false;
    }
    logf("[net] other player got %s: %s\n", e.decl, ok ? "given to me too" : "item not found here");
}
static void hookGiveItem() {
    g_origGiveItem = reinterpret_cast<GiveItem_t>(installInlineHook(0x8592A0, kGiveItemHead, sizeof kGiveItemHead,
                                                                    reinterpret_cast<void*>(&hkGiveItem)));
    logf("[ewcoop] give-item hook (shared keys): %s\n", g_origGiveItem ? "on" : "OFF");
}

// ================= grabs, QTE struggles, scripted kills: synchro motions =================
// idAnimatedEntity::StartSynchroMotion 0x91EB60(owner, targetIndex, action, joint): the enemy (owner) binds the
// target in its synchroTargets list (idList of entity handles at owner+1144, count +1152) to one of its joints
// and sends it the action, which starts the player's paired animation (e.g. "SP_Damage_500_275" at
// "pc_attach"). EndSynchroMotion 0x91B800(owner, setPos) unbinds all targets and frees the list.
// When our player is the target we send it; the other game runs the same call on the same enemy with player 2
// as the only target (its list swapped for a one-entry list during the call).
using StartSynchro_t = int64_t (*)(void* owner, int idx, const char* action, const char* joint);
using EndSynchro_t = int64_t (*)(void* owner, char setPos);
static StartSynchro_t g_origStartSynchro;
static EndSynchro_t g_origEndSynchro;
static const unsigned char kStartSynchroHead[15] = {0x48, 0x89, 0x7C, 0x24, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57,
                                                    0x48, 0x8D, 0x6C, 0x24, 0xE0};
static const unsigned char kEndSynchroHead[14] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x70, 0x20, 0x55, 0x41, 0x54,
                                                  0x41, 0x55, 0x41, 0x56};
static bool g_applyingSynchro;

static void* resolveHandle(void* handle) {  // 0x79C7D0: entity handle -> entity (or null)
    return fn<void* (*)(void*)>(0x79C7D0)(handle);
}
static int64_t hkStartSynchro(void* owner, int idx, const char* action, const char* joint) {
    if (!g_unhook && !g_applyingSynchro && g_netStarted && g_player1 && owner) {
        int n = field<int>(owner, 1152);
        auto* list = field<char*>(owner, 1144);
        void* target = nullptr;
        if (list && idx >= 0 && idx < n && readable(list + idx * 8, 8)) {
            uint64_t h = *reinterpret_cast<uint64_t*>(list + idx * 8);
            target = resolveHandle(&h);
        }
        const char* name = entityName(owner);
        if (target == g_player1 && name) {
            NetEvt e = {};
            e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtSyncStart;
            e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
            strncpy(e.target, name, sizeof e.target - 1);
            if (action && readable(action, 1)) strncpy(e.decl, action, sizeof e.decl - 1);
            if (joint && readable(joint, 1)) strncpy(reinterpret_cast<char*>(e.loc), joint, sizeof e.loc - 1);
            e.flags = (action ? kEvtHasInfo : 0) | (joint ? kEvtHasLoc : 0);
            sendEvent(e);
            g_p1SyncOwner = owner;
            logf("[net] grabbed: %s starts '%s' on me (joint %s) -> sent\n", name, action ? action : "-",
                 joint ? joint : "-");
        } else if (target == g_player2) {
            logf("[net] local enemy %s grabbing player 2 ('%s') - his own game decides that, ignored here\n",
                 name ? name : "?", action ? action : "-");
            return 0;
        }
    }
    return g_origStartSynchro(owner, idx, action, joint);
}
static int64_t hkEndSynchro(void* owner, char setPos) {
    if (!g_unhook && !g_applyingSynchro && g_netStarted && owner && owner == g_p1SyncOwner) {
        g_p1SyncOwner = nullptr;
        if (const char* name = entityName(owner)) {
            NetEvt e = {};
            e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtSyncEnd;
            e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
            strncpy(e.target, name, sizeof e.target - 1);
            e.flags = setPos ? kEvtStore : 0;
            sendEvent(e);
            logf("[net] released by %s -> sent\n", name);
        }
    }
    return g_origEndSynchro(owner, setPos);
}
// Run fn with owner's synchroTargets temporarily = [player 2]. idList at +1144: ptr, num, size, granularity,
// flags (+1163: 0/3 = owns memory, EndSynchroMotion frees it otherwise, so mark ours as not owned).
template <typename F> static void withP2AsTarget(void* gameLocal, void* owner, F call) {
    char* mgr = field<char*>(gameLocal, off_gameLocal_entMgr);
    int idx = field<int>(g_player2, 16);
    if (!mgr || idx < 0 || idx >= 131072) return;
    static uint32_t one[4];
    one[0] = static_cast<uint32_t>(idx) | (*reinterpret_cast<uint32_t*>(mgr + 524336 + 4 * idx) << 16);
    auto* table = *reinterpret_cast<uint32_t**>(mgr + 786488);
    one[1] = table && readable(table + idx, 4) ? table[idx] : 0;
    char saved[24];
    char* l = static_cast<char*>(owner) + 1144;
    memcpy(saved, l, sizeof saved);
    *reinterpret_cast<void**>(l) = one;
    *reinterpret_cast<int*>(l + 8) = 1;
    *reinterpret_cast<int*>(l + 12) = 1;
    l[19] = 1;
    g_applyingSynchro = true;
    call();
    g_applyingSynchro = false;
    memcpy(l, saved, sizeof saved);
}
static void applySynchro(void* gameLocal, const NetEvt& e) {
    void* owner = vfn<void* (*)(void*, const char*)>(gameLocal, 984 / 8)(gameLocal, e.target);
    bool ok = owner && g_player2 && isA(g_player2, rva_idPlayer_vtable) && g_origStartSynchro && g_origEndSynchro;
    if (ok && e.type == kEvtSyncStart) {
        char action[64], joint[84];
        memcpy(action, e.decl, sizeof action); action[63] = 0;
        memcpy(joint, e.loc, sizeof joint); joint[83] = 0;
        // Put him where the remote really is first, then let the bind take over.
        Vec3 at{g_view.px, g_view.py, g_view.pz};
        placePlayer(g_player2, at);
        withP2AsTarget(gameLocal, owner, [&] {
            g_origStartSynchro(owner, 0, (e.flags & kEvtHasInfo) ? action : nullptr, (e.flags & kEvtHasLoc) ? joint : "");
        });
        g_p2SyncOwner = owner;
        g_p2SyncUntil = GetTickCount() + 20000;
        // The Sadist (idNpc_Nata) plays his half from his own state machine (0x101A100, state int at +9564):
        // state 11 chases, 12 = the kill (anim request 12, then on his "Crush" event he finishes "player 0").
        // Put him straight into 12 with the same anim; hkNataThink makes his "player 0" = player 2 meanwhile.
        const char* cls = className(owner);
        if (cls && strcmp(cls, ".?AVidNpc_Nata@@") == 0) {
            *reinterpret_cast<int*>(static_cast<char*>(owner) + 9564) = 12;
            // +10560 = committed: his per-frame "is the player in sight" check (runs in states >= 8 until this
            // is set) otherwise sent him straight back to chasing (state 11), at our own player.
            *(static_cast<char*>(owner) + 10560) = 1;
            fn<void (*)(void*, int)>(0x7CD8C0)(owner, 4);
            fn<void (*)(void*, int)>(0x7CC920)(owner, 12);
            g_aiForceP2Owner = owner;
            logf("[net] Sadist %s put into his kill state on player 2\n", e.target);
        }
    } else if (ok && e.type == kEvtSyncEnd && owner == g_p2SyncOwner) {
        withP2AsTarget(gameLocal, owner, [&] { g_origEndSynchro(owner, (e.flags & kEvtStore) ? 1 : 0); });
        if (owner == g_aiForceP2Owner) {
            // The real kill never leaves state 12 (the victim's game reloads). Here the world goes on: back to
            // chasing (11), still committed so he doesn't restart the approach.
            *reinterpret_cast<int*>(static_cast<char*>(owner) + 9564) = 11;
            g_aiForceP2Owner = nullptr;
        }
        g_p2SyncOwner = nullptr;
        g_p2SyncUntil = 0;
    }
    logf("[net] other player %s %s (%s): %s\n", e.type == kEvtSyncStart ? "grabbed by" : "released by", e.target,
         e.type == kEvtSyncStart ? e.decl : "-", ok ? "replayed on player 2" : "enemy/player 2 not found here");
}
// idNpc_Nata (Sadist) vtable slot 14 = 0x101A100, his per-frame grab/kill state code.
using NataThink_t = int64_t (*)(void*);
static NataThink_t g_origNataThink;
static int64_t hkNataThink(void* self) {
    bool force = !g_unhook && self && self == g_aiForceP2Owner && g_player2;
    if (force) g_aiForceP2 = true;
    int64_t r = g_origNataThink(self);
    g_aiForceP2 = false;
    // Kill finished or interrupted: he left the kill states (back to chasing, 11).
    if (force && *reinterpret_cast<int*>(static_cast<char*>(self) + 9564) == 11) g_aiForceP2Owner = nullptr;
    // The release never arrived (lost packets): after the 20 s grab window, let him go back to chasing.
    if (force && g_aiForceP2Owner && !g_p2SyncUntil) {
        *reinterpret_cast<int*>(static_cast<char*>(self) + 9564) = 11;
        g_aiForceP2Owner = nullptr;
    }
    return r;
}
static void hookSynchro() {
    if (hookSlot(g_nataThinkHook, 0x18D3F48, 14, 0x101A100, reinterpret_cast<void*>(&hkNataThink)))
        g_origNataThink = reinterpret_cast<NataThink_t>(g_nataThinkHook.orig);
    else
        g_nataThinkHook.slot = nullptr;
    g_origStartSynchro = reinterpret_cast<StartSynchro_t>(installInlineHook(
        0x91EB60, kStartSynchroHead, sizeof kStartSynchroHead, reinterpret_cast<void*>(&hkStartSynchro)));
    g_origEndSynchro = reinterpret_cast<EndSynchro_t>(installInlineHook(
        0x91B800, kEndSynchroHead, sizeof kEndSynchroHead, reinterpret_cast<void*>(&hkEndSynchro)));
    logf("[ewcoop] synchro-motion hooks (grab/kill sync): %s / %s\n", g_origStartSynchro ? "on" : "OFF",
         g_origEndSynchro ? "on" : "OFF");
}

static void hookActorDamage() {
    int hooked = 0, skipped = 0;
    for (const auto& s : kActorDamageSlots) {
        void** slot = reinterpret_cast<void**>(g_base + s.vtable) + 80;
        auto cur = reinterpret_cast<uintptr_t>(*slot);
        bool original = cur == g_base + s.damage;
        bool oldHook = cur < g_base || cur >= g_exeEnd;
        if (!original && !oldHook) { ++skipped; continue; }
        writeSlot(slot, reinterpret_cast<void*>(&ewDamageThunk));
        ++hooked;
    }
    logf("[ewcoop] enemy damage hooked in %d character classes (%d skipped)\n", hooked, skipped);
}

static void queueEvent(const NetEvt& e) {
    for (uint32_t id : g_evtSeen)
        if (id == e.id) return;  // duplicate copy
    g_evtSeen[g_evtSeenPos++ % 512] = e.id;
    if (g_evtQueueCount < 256) g_evtQueue[g_evtQueueCount++] = e;
}
static void applyEvents(void* gameLocal) {
    for (int i = 0; i < g_evtQueueCount; ++i) {
        NetEvt& e = g_evtQueue[i];
        e.target[63] = e.decl[63] = 0;
        if (e.type == kEvtRemove) {
            applyRemoval(gameLocal, e);
            continue;
        }
        if (e.type == kEvtSyncStart || e.type == kEvtSyncEnd) {
            applySynchro(gameLocal, e);
            continue;
        }
        if (e.type == kEvtSnapshot) {
            takeSnapshot(gameLocal, e.target);
            continue;
        }
        if (e.type == kEvtGiveItem) {
            applyGiveItem(e);
            continue;
        }
        if (e.type == kEvtTrigger) {
            applyTrigger(gameLocal, e);
            continue;
        }
        if (e.type == kEvtMapChange || e.type == kEvtMapInfo) {
            applyMapEvent(gameLocal, e);
            continue;
        }
        if (e.type == kEvtInvRequest || e.type == kEvtInvItem || e.type == kEvtInvDone) {
            coopEvent(e);
            continue;
        }
        if (e.type == kEvtDeath) {
            // The other game restarts from its checkpoint (death): restart ours the same way (the call the death
            // screen makes). Older builds send this on health 0; same handling.
            if (GetTickCount() - g_lastRestartTick < 20000) {
                logf("[net] the other game restarted too (same death)\n");
                continue;
            }
            // Die the game's own way: the "kill" event (idEventDef 0x9B67D20, what the `kill` console command
            // posts to the player) runs the full death: tinnitus, red screen, death screen, checkpoint restart.
            if (g_player1 && g_player1Alive && !g_diedFromRemote) {
                g_diedFromRemote = true;
                logf("[net] the other player died: my player dies too (full death sequence, then restart)\n");
                fn<void (*)(void*, void*, void*)>(0x841560)(g_player1, reinterpret_cast<void*>(g_base + 0x9B67D20),
                                                           nullptr);
            }
            continue;
        }
        if (e.type == kEvtPlayerHit) {
            // The other player was hit in his game: replay it on player 2 so the reaction animation plays.
            // His health stays the remote's (applyRemote mirrors it), so pad it during the call: this copy
            // must never kill him.
            void* decl = fn<FindEntityDef_t>(rva_FindEntityDef)(reinterpret_cast<void*>(g_base + kDamageDeclList),
                                                                e.decl, 1, 1, 0, 0, 0, 0, 0);
            bool ok = decl && g_player2 && isA(g_player2, rva_idPlayer_vtable) && g_origDamage;
            if (ok) {
                alignas(16) uint8_t info[32], loc[88];
                alignas(16) float dir[4] = {};
                memcpy(info, e.info, 32);
                memcpy(loc, e.loc, 84);
                memcpy(dir, e.dir, 12);
                float& hp = *reinterpret_cast<float*>(static_cast<char*>(g_player2) + 0x8C4);
                float keep = hp;
                hp = 10000.f;
                g_applyingNetDamage = true;
                g_origDamage(g_player2, g_player2, g_player2, decl, (e.flags & kEvtHasInfo) ? info : nullptr,
                             (e.flags & kEvtHasDir) ? dir : nullptr, (e.flags & kEvtHasLoc) ? loc : nullptr);
                g_applyingNetDamage = false;
                hp = keep;
            }
            if (g_evtLogBudget > 0) { --g_evtLogBudget; logf("[net] got player hit: %s : %s\n", e.decl, ok ? "replayed on player 2" : "skipped"); }
            continue;
        }
        void* ent = vfn<void* (*)(void*, const char*)>(gameLocal, 984 / 8)(gameLocal, e.target);
        void* decl = fn<FindEntityDef_t>(rva_FindEntityDef)(reinterpret_cast<void*>(g_base + kDamageDeclList), e.decl,
                                                             1, 1, 0, 0, 0, 0, 0);
        bool ok = ent && decl && g_player2;
        if (ok) {
            alignas(16) uint8_t info[32], loc[88];
            alignas(16) float dir[4] = {};
            memcpy(info, e.info, 32);
            memcpy(loc, e.loc, 84);
            memcpy(dir, e.dir, 12);
            g_applyingNetDamage = true;
            vfn<void (*)(void*, void*, void*, void*, void*, void*, void*)>(ent, 80)(
                ent, g_player2, g_player2, decl, (e.flags & kEvtHasInfo) ? info : nullptr,
                (e.flags & kEvtHasDir) ? dir : nullptr, (e.flags & kEvtHasLoc) ? loc : nullptr);
            g_applyingNetDamage = false;
        }
        if (g_evtLogBudget > 0) {
            --g_evtLogBudget;
            logf("[net] got hit: %s <- %s : %s\n", e.target, e.decl,
                 ok ? "applied" : !ent ? "no such entity here" : !decl ? "unknown damage decl" : "no player 2");
        }
    }
    g_evtQueueCount = 0;
}

// ================= shot sync: projectile start + direction =================
// Every projectile (crossbow bolts included) is launched through 0x953D10(gameLocal, info, a3, a4). info is a
// 176-byte launch record copied as-is into the projectile list (copy 0x8660D0; simulated in 0x867250):
// +0 owner, +64 start origin, +88 direction (12 floats in 64..111, all plain floats), +128 projectile decl.
// Player 2 fires from his own replica of the remote's aim (100 ms behind, own camera position), so his bolts
// landed elsewhere. Now: the shooter sends his launch record's start + direction, and when player 2 launches
// a projectile on this side we overwrite his record with the oldest unused remote shot.
enum { kEvtShot = 3 };
static const uintptr_t rva_LaunchProjectile = 0x953D10;
static const unsigned char kLaunchPrologue[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
                                                  0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
#pragma pack(push, 1)
struct NetShot { uint32_t magic; uint8_t version, type; uint16_t flags; uint32_t id; float launch[12]; char weapon[48]; };
#pragma pack(pop)
using Launch_t = char (*)(void*, void*, void*, void*);
static Launch_t g_origLaunch;
struct PendingShot { float launch[12]; float age; };
static PendingShot g_shotQ[8];
static int g_shotQCount;
static uint32_t g_shotSeq;
static int g_shotLogBudget = 30;

static bool ownedBy(void* owner, void* player) {
    if (!owner || !player) return false;
    if (owner == player) return true;
    return readable(owner, 320) && field<void*>(owner, 312) == player;  // presentable/controller -> player
}
static char hkLaunch(void* gameLocal, void* info, void* a3, void* a4) {
    if (!g_unhook && g_netStarted && info && readable(info, 112)) {
        void* owner = field<void*>(info, 0);
        float* launch = reinterpret_cast<float*>(static_cast<char*>(info) + 64);
        if (g_player1 && ownedBy(owner, g_player1) && g_netSock != INVALID_SOCKET && g_netHavePeer && !g_netLoopback) {
            NetShot s = {};
            s.magic = kNetMagic; s.version = kNetVersion; s.type = kEvtShot;
            s.id = ++g_shotSeq ^ (GetCurrentProcessId() << 20);
            memcpy(s.launch, launch, sizeof s.launch);
            strncpy(s.weapon, equippedWeapon(g_player1), sizeof s.weapon - 1);
            for (int i = 0; i < 2; ++i)
                netSendRaw(&s, sizeof s);
            if (g_shotLogBudget > 0) {
                --g_shotLogBudget;
                logf("[net] sent shot (%s) from (%.0f %.0f %.0f) dir (%.2f %.2f %.2f)\n", s.weapon, launch[0], launch[1],
                     launch[2], launch[6], launch[7], launch[8]);
            }
        } else if (g_player2 && ownedBy(owner, g_player2) && g_shotQCount > 0) {
            memcpy(launch, g_shotQ[0].launch, sizeof g_shotQ[0].launch);
            memmove(g_shotQ, g_shotQ + 1, sizeof(PendingShot) * --g_shotQCount);
            if (g_shotLogBudget > 0) { --g_shotLogBudget; logf("[net] player 2 shot: using the remote's start + direction\n"); }
        } else if (g_player2 && ownedBy(owner, g_player2) && g_shotLogBudget > 0) {
            --g_shotLogBudget;
            logf("[net] player 2 shot with no remote shot queued (kept his own aim)\n");
        }
    }
    return g_origLaunch(gameLocal, info, a3, a4);
}
static void receiveShot(const NetShot& s) {
    static uint32_t seen[16];
    static int seenPos;
    for (uint32_t id : seen)
        if (id == s.id) return;
    seen[seenPos++ % 16] = s.id;
    if (g_shotQCount == 8) { memmove(g_shotQ, g_shotQ + 1, sizeof(PendingShot) * 7); --g_shotQCount; }
    PendingShot& p = g_shotQ[g_shotQCount++];
    memcpy(p.launch, s.launch, sizeof p.launch);
    p.age = 0.f;
}
static void ageShots(float dt) {  // a shot player 2 never fired here (dropped input) must not steer a later one
    for (int i = 0; i < g_shotQCount;) {
        g_shotQ[i].age += dt;
        if (g_shotQ[i].age > 1.f) { memmove(g_shotQ + i, g_shotQ + i + 1, sizeof(PendingShot) * (--g_shotQCount - i)); }
        else ++i;
    }
}

// ================= shared enemies v2: transform streaming =================
// The per-actor update 0x91F2C0 runs for every character each frame and ends by copying the actor's pose
// into its motion object (+1720: origin +0x150, facing quat +0x160) and actor physics (+696), then
// UpdateVisuals. Hooking it lets the host read each character's rendered pose and the client overwrite it:
//   host   -> once per net tick send the characters near either player, keyed by level name (the same names
//             the damage replay uses; map-placed names match across copies)
//   client -> for a name the host streams, place the actor at the interpolated host pose every frame
// so both games show the same enemy in the same place. Enemy AI, attacks and animation stay local for now.
using ActorUpdate_t = int64_t (*)(void*);
static const int kEnemyMax = 12;             // characters per packet
static const int kEnemyNameLen = 64;
static const int kEnemyCap = 32;             // most we collect / keep per side
static const float kEnemyRange = 3000.f;     // stream only characters near either player
static const float kEnemySendInterval = 1.f / 20.f;
static const float kEnemyInterp = 0.1f;      // Source-style delay before the shown pose
#pragma pack(push, 1)
// flags bit0 (host -> client only): "this one is yours" (the client's player is nearer; the client's AI decides)
struct EnemyState { char name[kEnemyNameLen]; float px, py, pz; float qx, qy, qz, qw; uint8_t flags; };
struct NetEnemy { uint32_t magic; uint8_t version, type; uint16_t count; uint32_t seq; EnemyState e[kEnemyMax]; };
#pragma pack(pop)
enum { kEvtEnemy = 2 };
static const unsigned char kActorUpdatePrologue[12] = {0x4c, 0x8b, 0xdc, 0x55, 0x53, 0x56,
                                                       0x41, 0x56, 0x49, 0x8d, 0x6b, 0xa1};

static float g_enemyTime, g_enemySendT;  // g_enemyTime: monotonic seconds for interpolation (advanced in netRun)
static uint32_t g_enemySeq;
static bool g_enemySync = true;          // Ctrl+Shift+Y toggles
static int g_enemyLogBudget = 40;

struct EnemySample { char name[kEnemyNameLen]; Vec3 p; float q[4]; float d2; uint8_t flags; };
static EnemySample g_enemySnap[kEnemyCap];       // host: characters seen this frame
struct EnemyRecv { char name[kEnemyNameLen]; Vec3 p0, p1; float q0[4], q1[4]; float t0, t1; int n; };
static EnemyRecv g_enemyRecv[kEnemyCap];         // client: last two samples per streamed character

static bool isCharacterVtable(uintptr_t rva) {
    for (const auto& s : kActorDamageSlots)
        if (s.vtable == rva) return true;
    return false;
}
static void readMotionPose(void* motion, Vec3* o, float* q) {
    fn<void* (*)(void*, void*)>(rva_MotionGetOrigin)(motion, o);
    fn<void* (*)(void*, void*)>(rva_MotionGetFacing)(motion, q);
}
// Set an actor's pose exactly as 0x91F2C0 does: motion origin+facing, physics origin+axis, then refresh the
// render entity. Survives until the actor's own AI update next frame (which we overwrite again).
static void placeActor(void* actor, const Vec3& o, const float* q) {
    if (void* motion = field<void*>(actor, off_partner_facing)) {
        fn<SetFacingOrigin_t>(rva_SetFacingOrigin)(motion, &o);
        fn<SetFacing_t>(rva_SetFacing)(motion, q);
    }
    if (void* phys = field<void*>(actor, off_actor_physics); validEnt(phys)) {
        vfn<SetOrigin_t>(phys, slot_phys_SetOrigin)(phys, &o, -1);
        alignas(16) unsigned char mat[48] = {};
        fn<void* (*)(const void*, void*)>(rva_QuatToMat3)(q, mat);
        vfn<SetAxis_t>(phys, slot_phys_SetAxis)(phys, mat, -1);
    }
    fn<void (*)(void*)>(rva_UpdateVisuals)(actor);
}
static void nlerpQ(const float* a, const float* b, float t, float* out) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float s = d < 0.f ? -1.f : 1.f;
    for (int i = 0; i < 4; ++i) out[i] = a[i] + (b[i] * s - a[i]) * t;
    float n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (n > 1e-6f) for (int i = 0; i < 4; ++i) out[i] /= n;
}
static EnemyRecv* findRecv(const char* name) {
    for (int i = 0; i < g_enemyRecvCount; ++i)
        if (strcmp(g_enemyRecv[i].name, name) == 0) return &g_enemyRecv[i];
    return nullptr;
}
static void storeEnemySample(const EnemyState& e) {
    EnemyRecv* r = findRecv(e.name);
    if (!r) {
        if (g_enemyRecvCount >= kEnemyCap) return;
        r = &g_enemyRecv[g_enemyRecvCount++];
        memset(r, 0, sizeof *r);
        strncpy(r->name, e.name, kEnemyNameLen - 1);
    }
    r->p0 = r->p1; memcpy(r->q0, r->q1, sizeof r->q0); r->t0 = r->t1;
    r->p1 = Vec3{e.px, e.py, e.pz};
    r->q1[0] = e.qx; r->q1[1] = e.qy; r->q1[2] = e.qz; r->q1[3] = e.qw;
    r->t1 = g_enemyTime;
    if (r->n < 2) ++r->n;
}
static void interpRecv(const EnemyRecv* r, Vec3* o, float* q) {
    if (r->n < 2) { *o = r->p1; memcpy(q, r->q1, sizeof r->q1); return; }
    float span = r->t1 - r->t0;
    float u = span > 1e-4f ? (g_enemyTime - kEnemyInterp - r->t0) / span : 1.f;
    u = u < 0.f ? 0.f : u > 1.f ? 1.f : u;
    o->x = r->p0.x + (r->p1.x - r->p0.x) * u;
    o->y = r->p0.y + (r->p1.y - r->p0.y) * u;
    o->z = r->p0.z + (r->p1.z - r->p0.z) * u;
    nlerpQ(r->q0, r->q1, u, q);
}
static float dist2ToPlayers(const Vec3& o) {
    // Cached positions (hkRunFrame, after the players are confirmed alive): reading the players here crashed
    // during a death reload, which frees them mid-frame while actors still update.
    float best = kEnemyRange * kEnemyRange;
    for (int i = 0; i < 2; ++i) {
        if (!g_playerPosValid[i]) continue;
        Vec3 pp = g_playerPos[i];
        float d = (o.x - pp.x) * (o.x - pp.x) + (o.y - pp.y) * (o.y - pp.y) + (o.z - pp.z) * (o.z - pp.z);
        if (d < best) best = d;
    }
    return best;
}
// ---- ownership: the game whose player is nearer runs that enemy ----
// Each enemy is "owned" by one copy: its AI decides there, and the other copy follows (pose + animation calls).
// The host decides: client owns it when the client's player is clearly nearer (150-unit hysteresis), and says so
// in the stream (flags bit0). Owner streams pose and forwards its animation requests/params; the follower drops
// its own AI's animation calls for that enemy and plays the forwarded ones.
struct EnemyCtl {
    void* ent; uint32_t hash; char name[kEnemyNameLen];
    bool mine, follow;           // this frame's role
    bool clientOwns;             // host: current decision (with hysteresis)
    float clientOwnsUntil;       // client: host said "yours" until this time
    float lastSeen;
    float lastParam[32]; bool paramSent[32];
    int ownReq;          // owner: his latest animation request (re-sent every second so late followers catch up)
    int suppressedReq;   // follower: his own AI's latest (muted) request, replayed when he stops following
    int appliedReq;      // follower: latest request played from the owner
    bool wasFollowing;
};
static EnemyCtl g_ctl[kEnemyCap];
static int g_ctlCount;
static bool g_applyingAnim;
static uint32_t nameHash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
    return h;
}
static EnemyCtl* ctlFor(void* ent, const char* name) {
    for (int i = 0; i < g_ctlCount; ++i)
        if (g_ctl[i].ent == ent && strcmp(g_ctl[i].name, name) == 0) return &g_ctl[i];
    for (int i = 0; i < g_ctlCount; ++i)  // drop stale entries (removed enemies, reloads)
        if (g_enemyTime - g_ctl[i].lastSeen > 2.f) { g_ctl[i] = g_ctl[--g_ctlCount]; --i; }
    if (g_ctlCount >= kEnemyCap) return nullptr;
    EnemyCtl& c = g_ctl[g_ctlCount++];
    memset(&c, 0, sizeof c);
    c.ent = ent; c.hash = nameHash(name);
    c.ownReq = c.suppressedReq = c.appliedReq = -1;
    strncpy(c.name, name, kEnemyNameLen - 1);
    return &c;
}
static EnemyCtl* ctlByEnt(void* ent) {  // animation hooks: only enemies we track this second
    for (int i = 0; i < g_ctlCount; ++i)
        if (g_ctl[i].ent == ent && g_enemyTime - g_ctl[i].lastSeen < 1.f) return &g_ctl[i];
    return nullptr;
}
static EnemyCtl* ctlByHash(uint32_t h) {
    for (int i = 0; i < g_ctlCount; ++i)
        if (g_ctl[i].hash == h && g_enemyTime - g_ctl[i].lastSeen < 1.f) return &g_ctl[i];
    return nullptr;
}
static float dist2(const Vec3& a, const Vec3& b) {
    return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z);
}
static void collectSample(const char* name, const Vec3& o, const float* q, float d2, uint8_t flags) {
    for (int i = 0; i < g_enemySnapCount; ++i)
        if (strcmp(g_enemySnap[i].name, name) == 0) {
            g_enemySnap[i].p = o; memcpy(g_enemySnap[i].q, q, 16); g_enemySnap[i].d2 = d2; g_enemySnap[i].flags = flags;
            return;
        }
    if (g_enemySnapCount < kEnemyCap) {
        EnemySample& s = g_enemySnap[g_enemySnapCount++];
        strncpy(s.name, name, kEnemyNameLen - 1); s.name[kEnemyNameLen - 1] = 0;
        s.p = o; memcpy(s.q, q, 16); s.d2 = d2; s.flags = flags;
    }
}
static bool animRequestRaw(char* ai, int idx);  // animation forwarding, below
// Called by the 0x91F2C0 hook after the actor's own update ran.
static void onActorUpdate(void* actor) {
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(actor) - g_base;
    if (!isCharacterVtable(vt)) return;
    if (actor == g_player1 || actor == g_player2 || actor == g_joseph) return;
    void* motion = field<void*>(actor, off_partner_facing);
    const char* name = entityName(actor);
    if (!motion || !name) return;
    // Hanging / ragdoll corpses are characters too; they filled the stream and crowded out real enemies.
    if (strstr(name, "corpse") || strstr(name, "ragdoll")) return;
    Vec3 o; float q[4];
    readMotionPose(motion, &o, q);
    float d2 = dist2ToPlayers(o);
    if (d2 >= kEnemyRange * kEnemyRange) return;  // nobody near: not shared
    EnemyCtl* c = ctlFor(actor, name);
    if (!c) return;
    c->lastSeen = g_enemyTime;
    bool mine;
    if (g_netIsHost) {
        // A dead player's game is about to reload: it keeps sending its (frozen, post-kill) enemy until then, and
        // the other copy copied that freeze. The living player's game runs every enemy meanwhile.
        bool clientDead = g_remoteValid && g_view.health <= 0.f, hostDead = !g_player1Alive;
        if (clientDead && !hostDead) c->clientOwns = false;
        else if (hostDead && !clientDead) c->clientOwns = true;
        else if (g_playerPosValid[0] && g_playerPosValid[1]) {
            float dHost = std::sqrt(dist2(o, g_playerPos[0])), dClient = std::sqrt(dist2(o, g_playerPos[1]));
            if (!c->clientOwns && dClient < dHost - 150.f) c->clientOwns = true;
            else if (c->clientOwns && dHost < dClient - 150.f) c->clientOwns = false;
        }
        mine = !c->clientOwns;
        collectSample(name, o, q, d2, c->clientOwns ? 1 : 0);  // always listed, so the client learns ownership
    } else {
        mine = c->clientOwnsUntil > g_enemyTime;
        if (mine) collectSample(name, o, q, d2, 0);
    }
    if (mine != c->mine) logf("[net] enemy %s: now run by %s\n", name, mine ? "this game" : "the other game");
    c->mine = mine;
    c->follow = false;
    EnemyRecv* r = mine ? nullptr : findRecv(name);
    // No fresh pose for a second (other copy crashed/reloading, or stopped streaming this one): let his own AI
    // move him again. Pinned to an old pose he walked in place.
    bool follow = !mine && r && r->n > 0 && g_enemyTime - r->t1 <= 1.f;
    if (c->wasFollowing && !follow) {
        // Handing him back to his own AI: his animation is wherever the other game left it (e.g. the end of a kill,
        // since that player's game reloaded). Replay what his own AI last asked for, which we had muted.
        if (c->suppressedReq >= 0) {
            g_applyingAnim = true;
            bool ok = animRequestRaw(static_cast<char*>(actor), c->suppressedReq);
            g_applyingAnim = false;
            logf("[net] enemy %s back to his own AI: replayed his request %d (%s)\n", name, c->suppressedReq,
                 ok ? "ok" : "rejected");
        }
    }
    if (follow && !c->wasFollowing) c->appliedReq = -1;  // take the owner's next "current request" as-is
    c->wasFollowing = follow;
    if (!follow) return;
    c->follow = true;
    interpRecv(r, &o, q);
    placeActor(actor, o, q);
}

// ---- animation forwarding ----
// Every enemy animates through two tiny functions (1900+ call sites), replaced here with C copies:
//   0x7CC920(ai, i)        send morpheme request table[i] (ai+8416 table, ai+8424 count)  e.g. attack, chase
//   0x7CD8C0(ai, i, float) set morpheme float param table[i] (ai+8448 table, ai+8456 count) e.g. speed
// Owner: forward. Follower: ignore its own AI's calls, play the forwarded ones.
#pragma pack(push, 1)
struct AnimOp { uint32_t hash; uint8_t kind; uint8_t pad; uint16_t idx; float val; uint32_t seq; };
static const int kAnimMax = 64;
struct NetAnim { uint32_t magic; uint8_t version, type; uint16_t count; uint32_t seq; AnimOp op[kAnimMax]; };
#pragma pack(pop)
enum { kEvtAnim = 9 };
static AnimOp g_animOut[kAnimMax];  // this frame's ops
static int g_animOutCount;
static AnimOp g_animResend[32];     // requests, each sent in 3 consecutive packets (UDP loss)
static int g_animResendLeft[32], g_animResendCount;
static uint32_t g_animSeq, g_animSeen[512];
static int g_animSeenPos;
static int g_animLogBudget = 30;
static const uintptr_t rva_AiAnimRequest = 0x7CC920, rva_AiAnimParam = 0x7CD8C0;
static unsigned char g_animReqOrig[12], g_animParamOrig[12];
static bool g_animHooked;

static bool animRequestRaw(char* ai, int idx) {
    int cnt = *reinterpret_cast<uint16_t*>(ai + 8424);
    if (cnt <= idx) return false;
    int id = (*reinterpret_cast<int**>(ai + 8416))[idx];
    return id != -1 && fn<char (*)(void*, int, char)>(0x907BA0)(ai + 1280, id, 1) != 0;
}
static int64_t animParamRaw(char* ai, int idx, float v) {
    int64_t result = *reinterpret_cast<uint16_t*>(ai + 8456);
    if (idx < static_cast<int>(result)) {
        uint16_t id = (*reinterpret_cast<uint16_t**>(ai + 8448))[idx];
        result = 0xFFFF;
        if (id != 0xFFFF) return fn<int64_t (*)(void*, uint16_t, float)>(0x9094A0)(ai + 1280, id, v);
    }
    return result;
}
static void queueOp(uint32_t hash, uint8_t kind, int idx, float val) {
    if (g_animOutCount >= kAnimMax) return;
    AnimOp& op = g_animOut[g_animOutCount++];
    op = {hash, kind, 0, static_cast<uint16_t>(idx), val, ++g_animSeq};
    if ((kind == 0 || kind == 3) && g_animResendCount < 32) {
        g_animResend[g_animResendCount] = op; g_animResendLeft[g_animResendCount++] = 2;
    }
}
static void queueAnimOp(const EnemyCtl* c, uint8_t kind, int idx, float val) { queueOp(c->hash, kind, idx, val); }

// ---- scripted player animations (kind 3) ----
// Level scripts (idTarget_PlayerActions: walk to a spot, look around, kick a door...) end up as morpheme requests
// on the player's animation network (player+1280) via idMorpheme_SendRequest 0x907BA0(net, id, flag). While OUR
// player is under script control (MOVE_DISABLED +4780 or WALK_DISABLED +4781), forward his requests; the other
// game plays them on player 2, so the scripted moves show there instead of a standing Sebastian.
using MorphRequest_t = char (*)(void* net, int id, char flag);
static MorphRequest_t g_origMorphRequest;
static const unsigned char kMorphRequestHead[8] = {0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x49, 0x18};
static char hkMorphRequest(void* net, int id, char flag) {
    if (!g_unhook && g_netStarted && !g_applyingAnim && g_player1 && net == static_cast<char*>(g_player1) + 1280 &&
        id >= 0 && id < 65536 &&
        (field<uint8_t>(g_player1, 4780) || field<uint8_t>(g_player1, 4781))) {
        queueOp(0, 3, id, flag ? 1.f : 0.f);
        static int logs;
        if (logs < 40) { ++logs; logf("[net] my scripted animation request %d -> sent\n", id); }
    }
    return g_origMorphRequest(net, id, flag);
}
static bool hkAnimRequest(char* ai, int idx) {
    if (!g_unhook && g_netStarted && !g_applyingAnim) {
        if (EnemyCtl* c = ctlByEnt(ai)) {
            if (c->follow) { c->suppressedReq = idx; return false; }  // the owner's game decides his animation
            if (c->mine) { c->ownReq = idx; queueAnimOp(c, 0, idx, 0.f); }
        }
    }
    return animRequestRaw(ai, idx);
}
static int64_t hkAnimParam(char* ai, int idx, float v) {
    if (!g_unhook && g_netStarted && !g_applyingAnim) {
        if (EnemyCtl* c = ctlByEnt(ai)) {
            if (c->follow) return 0xFFFF;
            if (c->mine && idx >= 0 && idx < 32 && (!c->paramSent[idx] || std::fabs(c->lastParam[idx] - v) > 0.01f)) {
                c->paramSent[idx] = true; c->lastParam[idx] = v;
                queueAnimOp(c, 1, idx, v);
            }
        }
    }
    return animParamRaw(ai, idx, v);
}
static void sendAnimOps() {
    if (g_netSock == INVALID_SOCKET || !g_netHavePeer) { g_animOutCount = g_animResendCount = 0; return; }
    // Once a second: each enemy we run, his current request (kind 2), for a follower that joined late.
    static float digestT;
    digestT -= 1.f / 60.f;
    if (digestT <= 0.f) {
        digestT = 1.f;
        for (int i = 0; i < g_ctlCount && g_animOutCount < kAnimMax; ++i)
            if (g_ctl[i].mine && g_ctl[i].ownReq >= 0 && g_enemyTime - g_ctl[i].lastSeen < 1.f)
                g_animOut[g_animOutCount++] = {g_ctl[i].hash, 2, 0, static_cast<uint16_t>(g_ctl[i].ownReq), 0.f, ++g_animSeq};
    }
    NetAnim pkt = {};
    pkt.magic = kNetMagic; pkt.version = kNetVersion; pkt.type = kEvtAnim; pkt.seq = ++g_enemySeq;
    int n = 0;
    for (int i = 0; i < g_animOutCount && n < kAnimMax; ++i) pkt.op[n++] = g_animOut[i];
    for (int i = 0; i < g_animResendCount;) {  // earlier requests again
        bool fresh = false;
        for (int j = 0; j < g_animOutCount; ++j) fresh |= g_animOut[j].seq == g_animResend[i].seq;
        if (!fresh && n < kAnimMax) { pkt.op[n++] = g_animResend[i]; --g_animResendLeft[i]; }
        if (g_animResendLeft[i] <= 0) { g_animResend[i] = g_animResend[--g_animResendCount]; g_animResendLeft[i] = g_animResendLeft[g_animResendCount]; }
        else ++i;
    }
    g_animOutCount = 0;
    if (n == 0) return;
    pkt.count = static_cast<uint16_t>(n);
    netSendRaw(&pkt, sizeof pkt);
}
static void receiveAnimOps(const NetAnim& pkt) {
    int n = pkt.count > kAnimMax ? kAnimMax : pkt.count;
    for (int i = 0; i < n; ++i) {
        const AnimOp& op = pkt.op[i];
        if (op.kind == 0 || op.kind == 3) {  // requests are resent: play each once
            bool seen = false;
            for (uint32_t s : g_animSeen) seen |= s == op.seq;
            if (seen) continue;
            g_animSeen[g_animSeenPos++ % 512] = op.seq;
        }
        if (op.kind == 3) {  // the other player's scripted animation: play it on player 2
            if (g_player2 && g_origMorphRequest) {
                g_applyingAnim = true;
                char ok = g_origMorphRequest(static_cast<char*>(g_player2) + 1280, op.idx, op.val != 0.f);
                g_applyingAnim = false;
                static int logs;
                if (logs < 40) { ++logs; logf("[net] other player's scripted animation request %d -> %s on player 2\n", op.idx, ok ? "played" : "rejected"); }
            }
            continue;
        }
        EnemyCtl* c = ctlByHash(op.hash);
        if (!c || !c->follow) continue;
        if (op.kind == 2 && op.idx == c->appliedReq) continue;  // already in the owner's current animation
        g_applyingAnim = true;
        if (op.kind == 0 || op.kind == 2) {
            bool ok = animRequestRaw(static_cast<char*>(c->ent), op.idx);
            c->appliedReq = op.idx;
            if (g_animLogBudget > 0) {
                --g_animLogBudget;
                logf("[net] %s: other game's animation request %d%s -> %s\n", c->name, op.idx,
                     op.kind == 2 ? " (catch-up)" : "", ok ? "played" : "rejected");
            }
        } else {
            animParamRaw(static_cast<char*>(c->ent), op.idx, op.val);
        }
        g_applyingAnim = false;
    }
}
static void hookAnimCalls() {
    auto patch = [](uintptr_t rva, unsigned char* save, void* target) {
        auto* p = reinterpret_cast<unsigned char*>(g_base + rva);
        bool ours = p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0;
        if (!ours) memcpy(save, p, 12);
        unsigned char jmp[12] = {0x48, 0xB8};
        memcpy(jmp + 2, &target, 8);
        jmp[10] = 0xFF; jmp[11] = 0xE0;
        writeCode(rva, jmp, 12);
    };
    static const unsigned char kReq[4] = {0x0F, 0xB7, 0x81, 0xE8}, kPar[4] = {0x0F, 0xB7, 0x81, 0x08};
    auto* r = reinterpret_cast<unsigned char*>(g_base + rva_AiAnimRequest);
    auto* a = reinterpret_cast<unsigned char*>(g_base + rva_AiAnimParam);
    bool okR = memcmp(r, kReq, 4) == 0 || (r[0] == 0x48 && r[1] == 0xB8);
    bool okA = memcmp(a, kPar, 4) == 0 || (a[0] == 0x48 && a[1] == 0xB8);
    if (!okR || !okA) { logf("[ewcoop] enemy animation calls differ, animation sync OFF\n"); return; }
    // An older ewcoop's jump is replaced; the original bytes are known.
    static const unsigned char kReqOrig[12] = {0x0f, 0xb7, 0x81, 0xe8, 0x20, 0x00, 0x00, 0x3b, 0xc2, 0x7e, 0x22, 0x48};
    static const unsigned char kParOrig[12] = {0x0f, 0xb7, 0x81, 0x08, 0x21, 0x00, 0x00, 0x3b, 0xd0, 0x7d, 0x24, 0x48};
    memcpy(g_animReqOrig, kReqOrig, 12);
    memcpy(g_animParamOrig, kParOrig, 12);
    patch(rva_AiAnimRequest, g_animReqOrig, reinterpret_cast<void*>(&hkAnimRequest));
    patch(rva_AiAnimParam, g_animParamOrig, reinterpret_cast<void*>(&hkAnimParam));
    g_animHooked = true;
    logf("[ewcoop] enemy animation hooks (owner's animations shown in both games): on\n");
    g_origMorphRequest = reinterpret_cast<MorphRequest_t>(installShortHook(
        0x907BA0, kMorphRequestHead, sizeof kMorphRequestHead, reinterpret_cast<void*>(&hkMorphRequest)));
    logf("[ewcoop] player animation request hook (scripted moves shown on player 2): %s\n",
         g_origMorphRequest ? "on" : "OFF");
}
static void unhookAnimCalls() {
    if (!g_animHooked) return;
    writeCode(rva_AiAnimRequest, g_animReqOrig, 12);
    writeCode(rva_AiAnimParam, g_animParamOrig, 12);
    if (g_origMorphRequest) writeCode(0x907BA0, kMorphRequestHead, sizeof kMorphRequestHead);
    g_animHooked = false;
}
static ActorUpdate_t g_origActorUpdate;
static int64_t hkActorUpdate(void* actor) {
    int64_t r = g_origActorUpdate(actor);
    if (!g_unhook && g_enemySync && g_netStarted && actor) onActorUpdate(actor);
    return r;
}
static void sendEnemies() {
    if (g_netSock == INVALID_SOCKET || !g_netHavePeer || g_enemySnapCount == 0) return;
    int idx[kEnemyCap];
    int n = g_enemySnapCount < kEnemyMax ? g_enemySnapCount : kEnemyMax;
    for (int i = 0; i < g_enemySnapCount; ++i) idx[i] = i;
    for (int i = 0; i < n; ++i) {  // partial selection: bring the n nearest to the front
        int m = i;
        for (int j = i + 1; j < g_enemySnapCount; ++j)
            if (g_enemySnap[idx[j]].d2 < g_enemySnap[idx[m]].d2) m = j;
        int t = idx[i]; idx[i] = idx[m]; idx[m] = t;
    }
    NetEnemy pkt = {};
    pkt.magic = kNetMagic; pkt.version = kNetVersion; pkt.type = kEvtEnemy; pkt.seq = ++g_enemySeq;
    pkt.count = (uint16_t)n;
    for (int i = 0; i < n; ++i) {
        const EnemySample& s = g_enemySnap[idx[i]];
        EnemyState& e = pkt.e[i];
        strncpy(e.name, s.name, kEnemyNameLen - 1);
        e.px = s.p.x; e.py = s.p.y; e.pz = s.p.z;
        e.qx = s.q[0]; e.qy = s.q[1]; e.qz = s.q[2]; e.qw = s.q[3];
        e.flags = s.flags;
    }
    netSendRaw(&pkt, sizeof pkt);
}

static void netRun(void* gameLocal, const Usercmd* p1cmd, float dt) {
    g_keepRunning = g_netStarted;
    g_ctxSwapOff = g_netStarted && !g_playAsP2;
    if (!g_netStarted) return;
    installKeepRunning();
    // (Mirroring focus into the active-app flag byte_9A52D20 was removed: the engine throttles itself hard when
    //  it's 0, so the background copy crawled. Background input is blocked by the DirectInput hook instead,
    //  and the weapon "leak" was really forwarded select buttons.)
    *reinterpret_cast<uint8_t*>(g_base + 0x9A52D20) = 1;
    g_netAge += dt;
    g_enemyTime += dt;
    g_loopClock += dt;
    g_netSendT -= dt;
    if (g_netSendT <= 0.f) {
        g_netSendT = 1.f / 30.f;
        if (void* p1 = localPlayer(gameLocal)) {
            NetPkt p;
            buildPacket(p1, p1cmd, g_loopClock, &p);
            if (g_netLoopback) {
                if ((rand() % 1000) >= (int)(kLoopLoss * 1000) && g_loopCount < 64)
                    g_loopQ[g_loopCount++] = {p, g_loopClock + kLoopDelay + (rand() % 40) * 0.001f};  // + 0-40 ms jitter
            } else if (g_netHavePeer && g_netSock != INVALID_SOCKET) {
                if (!g_netIsHost && !g_netPeerConfirmed) {  // client probing: try every host port
                    sockaddr_in a = g_peerAddr;
                    for (int i = 0; i < kNetPortRange; ++i) {
                        a.sin_port = htons((u_short)(kNetPort + i));
                        sendto(g_netSock, (const char*)&p, sizeof p, 0, (sockaddr*)&a, sizeof a);
                    }
                } else {
                    netSendRaw(&p, sizeof p);
                }
            }
        }
    }
    if (g_netIsHost && g_netHavePeer && !g_netLoopback) {  // host: tell the client which map/checkpoint we're on
        static float mapInfoT;
        if ((mapInfoT -= dt) <= 0.f) { mapInfoT = 5.f; sendMapInfo(gameLocal); }
    }
    if (g_enemySync && !g_netLoopback) {  // both copies stream the characters they run (see "ownership")
        g_enemySendT -= dt;
        if (g_enemySendT <= 0.f) { g_enemySendT = kEnemySendInterval; sendEnemies(); }
        sendAnimOps();  // every frame: animation calls of the characters we run
    }
    if (g_netLoopback) {
        for (int i = 0; i < g_loopCount;) {
            if (g_loopQ[i].due <= g_loopClock) {
                receivePacket(g_loopQ[i].p);
                g_loopQ[i] = g_loopQ[--g_loopCount];
            } else {
                ++i;
            }
        }
    } else if (g_netSock != INVALID_SOCKET) {
        for (int i = 0; i < 96; ++i) {  // drain (Steam P2P first, then UDP)
            alignas(8) char buf[1500];
            sockaddr_in from = {}; int fl = sizeof from;
            int n = -1;
            bool viaSteam = false;
            // Steam P2P: the client always (he started it); the host whenever hosting - a friend's first packet is
            // accepted right here (the P2PSessionRequest callback never came in the friend session).
            if ((g_steamMode || (g_netIsHost && g_steamReady)) && g_steamNet) {
                uint32_t size = 0;  // ISteamNetworking slot 1 IsP2PPacketAvailable, slot 2 ReadP2PPacket
                if (vfn<bool (*)(void*, uint32_t*, int)>(g_steamNet, 1)(g_steamNet, &size, 0) && size <= sizeof buf) {
                    uint32_t got = 0; uint64_t remote = 0;
                    if (vfn<bool (*)(void*, void*, uint32_t, uint32_t*, uint64_t*, int)>(g_steamNet, 2)(
                            g_steamNet, buf, sizeof buf, &got, &remote, 0)) {
                        if (remote != g_steamPeer) {
                            if (!g_netIsHost || !remote) continue;  // client: not our host
                            // Host: a new friend (or the same friend after a restart) - take him as the partner.
                            vfn<bool (*)(void*, uint64_t)>(g_steamNet, 3)(g_steamNet, remote);  // AcceptP2PSessionWithUser
                            bool changed = g_steamPeer != 0;
                            g_steamMode = true;
                            g_steamPeer = remote;
                            g_netHavePeer = g_netPeerConfirmed = true;
                            if (changed) netResetRemote();
                            logf("[steam] friend %llx connected over Steam P2P (first packet)\n", (unsigned long long)remote);
                        }
                        n = static_cast<int>(got);
                        viaSteam = true;
                    }
                }
            }
            if (n < 0) n = recvfrom(g_netSock, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
            if (n <= 0) break;
            if (n == (int)sizeof(NetEvt)) {  // damage event
                NetEvt ev;
                memcpy(&ev, buf, sizeof ev);
                if (ev.magic == kNetMagic && ev.version == kNetVersion && (ev.type == kEvtDamage || ev.type == kEvtPlayerHit || ev.type == kEvtRemove ||
                     ev.type == kEvtSyncStart || ev.type == kEvtSyncEnd || ev.type == kEvtSnapshot || ev.type == kEvtGiveItem ||
                     ev.type == kEvtTrigger || ev.type == kEvtDeath || ev.type == kEvtMapChange ||
                     ev.type == kEvtMapInfo || ev.type == kEvtInvRequest || ev.type == kEvtInvItem ||
                     ev.type == kEvtInvDone)) queueEvent(ev);
                continue;
            }
            if (n == (int)sizeof(NetShot)) {  // projectile launch
                NetShot s;
                memcpy(&s, buf, sizeof s);
                if (s.magic == kNetMagic && s.version == kNetVersion && s.type == kEvtShot) receiveShot(s);
                continue;
            }
            if (n == (int)sizeof(NetAnim)) {  // enemy animation calls from the copy that runs them
                NetAnim na;
                memcpy(&na, buf, sizeof na);
                if (na.magic == kNetMagic && na.version == kNetVersion && na.type == kEvtAnim) receiveAnimOps(na);
                continue;
            }
            if (n == (int)sizeof(NetEnemy)) {  // enemy transform batch
                NetEnemy en;
                memcpy(&en, buf, sizeof en);
                if (en.magic == kNetMagic && en.version == kNetVersion && en.type == kEvtEnemy) {
                    int c = en.count > kEnemyMax ? kEnemyMax : en.count;
                    for (int i = 0; i < c; ++i) {
                        en.e[i].name[kEnemyNameLen - 1] = 0;
                        if (!g_netIsHost && (en.e[i].flags & 1)) {  // host: "you run this one"
                            for (int k = 0; k < g_ctlCount; ++k)
                                if (strcmp(g_ctl[k].name, en.e[i].name) == 0) g_ctl[k].clientOwnsUntil = g_enemyTime + 0.5f;
                            continue;
                        }
                        storeEnemySample(en.e[i]);
                    }
                    if (g_enemyLogBudget > 0) { --g_enemyLogBudget; logf("[net] enemy state: %d characters\n", c); }
                }
                continue;
            }
            NetPkt in;
            memcpy(&in, buf, n < (int)sizeof in ? n : (int)sizeof in);
            if (n != (int)sizeof(NetPkt)) {
                static bool warned;
                if (!warned && n >= 6) { warned = true; logf("[net] got a %d-byte packet (expected %d): the other PC runs a different ewcoop build\n", n, (int)sizeof(NetPkt)); }
                continue;
            }
            // Follow the address valid packets come from: first contact, the client locking onto the host's
            // port, or the other side reconnecting with a new port (rejoin, restarted copy, changed NAT mapping).
            if (!viaSteam && in.magic == kNetMagic && in.version == kNetVersion &&
                (!g_netHavePeer || !g_netPeerConfirmed || from.sin_addr.s_addr != g_peerAddr.sin_addr.s_addr ||
                 from.sin_port != g_peerAddr.sin_port)) {
                bool changed = g_netPeerConfirmed;
                g_peerAddr = from;
                g_netHavePeer = g_netPeerConfirmed = true;
                if (changed) netResetRemote();  // new session from the other side: drop old snapshots/seq
                logf("[net] %s peer %s:%d\n", changed ? "peer moved to" : "connected to", inet_ntoa(from.sin_addr),
                     ntohs(from.sin_port));
            }
            receivePacket(in);
        }
    }
    ageShots(dt);
    steamDiag(dt);
    applyRemote(gameLocal, dt);
    applyEvents(gameLocal);
    if (g_levelRestarted) {  // a checkpoint restart just wiped the level: nothing below may touch it this frame
        g_ctlCount = 0;
        g_enemyRecvCount = g_enemySnapCount = 0;
        return;
    }
    g_netLogT += dt;
    if (g_netLogT >= 3.f) {
        g_netLogT = 0.f;
        // Angle diagnostics: for each player, the usercmd angles (deg), view angles (+20524), delta (+19660).
        void* who[2] = {localPlayer(gameLocal), g_player2};
        for (int i = 0; i < 2; ++i) {
            char* ctl = who[i] ? controllerOf(who[i]) : nullptr;
            if (!ctl) continue;
            const Usercmd* cm = reinterpret_cast<const Usercmd*>(ctl + 20396 + 64);  // slot A current cmd
            const float* view = reinterpret_cast<const float*>(ctl + 20524);
            const float* delta = reinterpret_cast<const float*>(ctl + 19660);
            logf("[net] angles %s: cmd=(%.1f %.1f) view=(%.1f %.1f) delta=(%.1f %.1f) physYaw=%.1f bodyFacing=%.1f\n",
                 i ? "p2" : "p1", cm->angles[0] * 360.f / 65536.f, cm->angles[1] * 360.f / 65536.f, view[0], view[1],
                 delta[0], delta[1], yawOf(who[i]), motionYaw(who[i]));
            // Camera result of the controller's view calc 0x85F610: origin +20724, axis +20736 (row 0 = forward).
            // If the aim pose follows this and player 2's never updates, his forward z stays ~0 when looking up.
            const float* vo = reinterpret_cast<const float*>(ctl + 20724);
            const float* ax = reinterpret_cast<const float*>(ctl + 20736);
            {
                const float* h = reinterpret_cast<const float*>(static_cast<char*>(who[i]) + 0x8C0);
                logf("[net] health %s: GetHealth=%.1f  +8C0..8CC = %.1f %.1f %.1f %.1f  (remote pkt health %.1f)\n",
                     i ? "p2" : "p1", vfn<float (*)(void*)>(who[i], 88)(who[i]), h[0], h[1], h[2], h[3],
                     g_remotePkt.health);
            }
            logf("[net] camera %s: origin=(%.0f %.0f %.0f) fwd=(%.2f %.2f %.2f) lantern slot=%d lit=%d\n", i ? "p2" : "p1",
                 vo[0], vo[1], vo[2], ax[0], ax[1], ax[2], lanternOut(who[i]) ? 1 : 0, lanternLit(who[i]));
            logf("[net] weapons %s: slot0=\"%s\" slot1=\"%s\" slot2=\"%s\" slot3=\"%s\" slot4=\"%s\" slot5=\"%s\"\n",
                 i ? "p2" : "p1", weaponSlotName(who[i], 0), weaponSlotName(who[i], 1), weaponSlotName(who[i], 2),
                 weaponSlotName(who[i], 3), weaponSlotName(who[i], 4), weaponSlotName(who[i], 5));
        }
        // Wounded-flag hunt: player 1 (not wounded) and player 2 (wounded pose) have the same health, so the
        // pose comes from some other state. Diff both bodies: bytes that are 0 vs 1 and floats in [0,1] that
        // differ a lot. Keep only differences that persist over 3 scans (3 s apart) to drop transient noise.
        {
            static int woundScans = 3;
            struct Cand { uint8_t region; uint8_t kind; uint32_t off; };  // kind 0 = byte flag, 1 = 0..1 float
            static Cand cands[4096];
            static int candCount = -1;
            char* e1 = static_cast<char*>(who[0]);
            char* e2 = static_cast<char*>(who[1]);
            char* k1 = e1 ? controllerOf(e1) : nullptr;
            char* k2 = e2 ? controllerOf(e2) : nullptr;
            if (woundScans > 0 && e1 && e2 && k1 && k2 && std::fabs(*reinterpret_cast<float*>(e1 + 0x8C4) -
                                                                    *reinterpret_cast<float*>(e2 + 0x8C4)) < 0.5f) {
                char* A[2] = {e1, k1};
                char* B[2] = {e2, k2};
                const int size[2] = {16384, 24576};
                auto differs = [&](int rg, int kind, uint32_t off) {
                    if (kind == 0) {
                        uint8_t a = A[rg][off], b = B[rg][off];
                        return a != b && a <= 1 && b <= 1;
                    }
                    float a, b;
                    memcpy(&a, A[rg] + off, 4);
                    memcpy(&b, B[rg] + off, 4);
                    return std::isfinite(a) && std::isfinite(b) && a >= 0.f && a <= 1.f && b >= 0.f && b <= 1.f &&
                           std::fabs(a - b) > 0.3f;
                };
                bool ok = readable(e1, size[0]) && readable(e2, size[0]) && readable(k1, size[1]) && readable(k2, size[1]);
                if (ok) {
                    --woundScans;
                    if (candCount < 0) {
                        candCount = 0;
                        for (int rg = 0; rg < 2; ++rg)
                            for (uint32_t off = 0; off < (uint32_t)size[rg]; ++off)
                                for (int kind = 0; kind < 2; ++kind) {
                                    if (kind == 1 && (off & 3)) continue;
                                    if (candCount < 4096 && differs(rg, kind, off))
                                        cands[candCount++] = {(uint8_t)rg, (uint8_t)kind, off};
                                }
                    } else {
                        int n = 0;
                        for (int i = 0; i < candCount; ++i)
                            if (differs(cands[i].region, cands[i].kind, cands[i].off)) cands[n++] = cands[i];
                        candCount = n;
                    }
                    logf("[woundscan] scan %d: %d candidates\n", 3 - woundScans, candCount);
                    if (woundScans == 0) {
                        for (int i = 0; i < candCount && i < 300; ++i) {
                            const Cand& c = cands[i];
                            if (c.kind == 0)
                                logf("[woundscan] %s+%u byte: p1=%u p2=%u\n", c.region ? "ctrl" : "ent", c.off,
                                     (unsigned)(uint8_t)A[c.region][c.off], (unsigned)(uint8_t)B[c.region][c.off]);
                            else
                                logf("[woundscan] %s+%u float: p1=%.3f p2=%.3f\n", c.region ? "ctrl" : "ent", c.off,
                                     *reinterpret_cast<float*>(A[c.region] + c.off), *reinterpret_cast<float*>(B[c.region] + c.off));
                        }
                    }
                }
            }
        }
        // Aim-pitch hunt: player 2's view angles and camera are right but his body aims level, so whatever
        // drives the aim pose is some other field. While the local player looks clearly up/down, list the
        // floats in his controller and entity that track his pitch (deg, rad or sin, either sign) where player
        // 2's same field does NOT track player 2's pitch. Those are the candidates.
        static int scans = 6;
        char* c1 = who[0] ? controllerOf(who[0]) : nullptr;
        char* c2 = who[1] ? controllerOf(who[1]) : nullptr;
        if (scans > 0 && c1 && c2) {
            float p1p = reinterpret_cast<const float*>(c1 + 20524)[0];
            float p2p = reinterpret_cast<const float*>(c2 + 20524)[0];
            if (std::fabs(p1p) > 15.f && std::fabs(p1p - p2p) > 10.f) {
                --scans;
                auto match = [](float v, float deg) {
                    float rad = deg * 0.0174533f, s = std::sin(rad);
                    const float cands[6] = {deg, -deg, rad, -rad, s, -s};
                    for (int k = 0; k < 6; ++k)
                        if (std::fabs(v - cands[k]) <= std::fabs(cands[k]) * 0.08f + 0.01f) return k;
                    return -1;
                };
                static const char* kinds[6] = {"deg", "-deg", "rad", "-rad", "sin", "-sin"};
                logf("[aimscan] p1 pitch %.1f, p2 pitch %.1f\n", p1p, p2p);
                struct Region { const char* name; char* a; char* b; int size; };
                Region regs[2] = {{"ctrl", c1, c2, 24576}, {"ent", static_cast<char*>(who[0]), static_cast<char*>(who[1]), 16384}};
                int shown = 0;
                for (const Region& rg : regs) {
                    if (!readable(rg.a, rg.size) || !readable(rg.b, rg.size)) continue;
                    for (int off = 0; off + 4 <= rg.size && shown < 40; off += 4) {
                        if (rg.name[0] == 'c' && off >= 20524 && off < 20536) continue;  // the view angles themselves
                        float v1, v2;
                        memcpy(&v1, rg.a + off, 4);
                        memcpy(&v2, rg.b + off, 4);
                        if (!std::isfinite(v1) || !std::isfinite(v2)) continue;
                        int k1 = match(v1, p1p);
                        if (k1 < 0) continue;
                        int k2 = match(v2, p2p);
                        logf("[aimscan] %s+%d: p1=%.3f (%s)  p2=%.3f %s\n", rg.name, off, v1, kinds[k1], v2,
                             k2 == k1 ? "tracks too" : "STUCK");
                        ++shown;
                    }
                }
            }
        }
        Vec3 p2 = g_player2 ? originOf(g_player2) : Vec3{};
        logf("[net] %s peer=%s remote=%s age=%.0fms recv=%u lost=%u snaps=%u | remote pos=(%.0f %.0f %.0f) vel=%.0f view=(%.0f %.0f) "
             "| p2 pos=(%.0f %.0f %.0f) angCorr=(%.1f %.1f)\n",
             g_netLoopback ? "LOOPBACK" : g_netIsHost ? "host" : "client", g_netHavePeer ? "y" : "n",
             g_remoteValid ? "y" : "n", g_netAge * 1000.f, g_netRecvCount, g_netDropCount, g_netSnapCount,
             g_remotePkt.px, g_remotePkt.py, g_remotePkt.pz,
             std::sqrt(g_remotePkt.vx * g_remotePkt.vx + g_remotePkt.vy * g_remotePkt.vy), g_remotePkt.pitch,
             g_remotePkt.yaw, p2.x, p2.y, p2.z, g_angCorrPitch, g_angCorrYaw);
    }
}

// ---- solo companion bot (Ctrl+Shift+B): a local 2nd Sebastian that follows you, leg-animated.
// Lets you test the co-op feel without a peer. It walks toward a follow point behind you and sprints to
// catch up; teleports if left far behind. (Combat is future work; use F9 Joseph for a fighting partner.)
static void botStep(void* gameLocal, float dt) {
    if (!g_bot) return;
    if (!playerAtSlot(gameLocal, 0)) return;  // need a live level
    if (!g_player2) spawnSecondPlayer(gameLocal, SpawnSebastian);
    if (!g_player2 || !isA(g_player2, rva_idPlayer_vtable)) return;
    void* p1 = localPlayer(gameLocal);
    if (!p1) return;
    Vec3 pp = originOf(p1);
    float pyaw = yawOf(p1) / 57.2957795f;
    Vec3 target{pp.x - std::cos(pyaw) * 100.f, pp.y - std::sin(pyaw) * 100.f, pp.z};  // ~100u behind you
    Vec3 bp = originOf(g_player2);
    float dx = target.x - bp.x, dy = target.y - bp.y;
    float dist = std::sqrt(dx * dx + dy * dy);
    Usercmd c = {};
    float moveYaw = dist > 8.f ? std::atan2(dy, dx) * 57.2957795f : yawOf(p1);
    c.angles[1] = angleToShort(moveYaw); c.yaw = c.angles[1];
    if (dist > 60.f) c.forwardmove = 127;          // walk toward the follow point (legs animate)
    if (dist > 400.f) c.buttons |= BUTTON_SPRINT;  // sprint to catch up
    c.clientGameFrame = ++g_p2Frame;
    fn<PlayerSetUsercmd_t>(rva_PlayerSetUsercmd)(g_player2, &c, &c);
    // No pathfinding: if he's trying to walk but barely moved over ~0.7s (a wall), blink to the follow point.
    static Vec3 last;
    static float stuckT, stuckMoved;
    float moved = std::sqrt((bp.x - last.x) * (bp.x - last.x) + (bp.y - last.y) * (bp.y - last.y));
    last = bp;
    if (dist > 130.f) {
        stuckT += dt;
        stuckMoved += moved;
        if (stuckT >= 0.7f) {
            if (stuckMoved < 40.f) placePlayer(g_player2, Vec3{target.x, target.y, pp.z});  // wall -> blink
            stuckT = 0.f;
            stuckMoved = 0.f;
        }
    } else {
        stuckT = 0.f;
        stuckMoved = 0.f;
    }
    if (dist > 500.f) placePlayer(g_player2, Vec3{target.x, target.y, pp.z});  // left far behind -> catch up
}

// ================= Ctrl+Shift+F12: snapshot for bug reports =================
// Writes snapshots\<id>\c<pid>.txt (players, nearby characters, recent sync events) and c<pid>.png (this game's
// window) next to the DLL, and tells the other copy to do the same into the same folder.
extern "C" {
struct EwGdipInput { UINT32 version; void* cb; BOOL noThread; BOOL noCodecs; };
int WINAPI GdiplusStartup(ULONG_PTR* token, const EwGdipInput* in, void* out);
int WINAPI GdipCreateBitmapFromHBITMAP(HBITMAP bm, HPALETTE pal, void** out);
int WINAPI GdipSaveImageToFile(void* img, const WCHAR* file, const CLSID* enc, const void* params);
int WINAPI GdipDisposeImage(void* img);
}
static bool saveWindowPng(HWND w, const char* path) {
    static ULONG_PTR token;
    if (!token) { EwGdipInput in = {1, nullptr, FALSE, FALSE}; GdiplusStartup(&token, &in, nullptr); }
    RECT r;
    if (!w || !GetClientRect(w, &r) || r.right <= 0 || r.bottom <= 0) return false;
    HDC wdc = GetDC(w), mdc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, r.right, r.bottom);
    HGDIOBJ old = SelectObject(mdc, bm);
    // PW_CLIENTONLY | PW_RENDERFULLCONTENT (captures D3D content even when covered); BitBlt as a fallback.
    if (!PrintWindow(w, mdc, 1 | 2)) BitBlt(mdc, 0, 0, r.right, r.bottom, wdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);
    bool ok = false;
    void* img = nullptr;
    if (GdipCreateBitmapFromHBITMAP(bm, nullptr, &img) == 0 && img) {
        static const CLSID kPng = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
        WCHAR wpath[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
        ok = GdipSaveImageToFile(img, wpath, &kPng, nullptr) == 0;
        GdipDisposeImage(img);
    }
    DeleteObject(bm); DeleteDC(mdc); ReleaseDC(w, wdc);
    return ok;
}
static void snapPlayer(FILE* f, const char* label, void* p) {
    if (!p || !isA(p, rva_idPlayer_vtable)) { fprintf(f, "%s: none\n", label); return; }
    Vec3 o = originOf(p);
    char* pc = static_cast<char*>(p);
    fprintf(f, "%s %p \"%s\": pos (%.0f %.0f %.0f) yaw %.0f health %.1f/%.1f sneaking %d weapon \"%s\" lantern out %d lit %d\n",
            label, p, entityName(p) ? entityName(p) : "", o.x, o.y, o.z, yawOf(p), *reinterpret_cast<float*>(pc + 0x8C4),
            *reinterpret_cast<float*>(pc + 0x8C0), pc[off_player_sneaking], equippedWeapon(p), lanternOut(p),
            lanternLit(p));
}
static void takeSnapshot(void* gameLocal, const char* id) {
    char dir[MAX_PATH], path[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\snapshots", g_dir);
    CreateDirectoryA(dir, nullptr);
    snprintf(dir, sizeof dir, "%s\\snapshots\\%s", g_dir, id);
    CreateDirectoryA(dir, nullptr);
    DWORD pid = GetCurrentProcessId();
    sneakDiag("player 1", g_player1);  // into the log, picked up by "recent events" below
    sneakDiag("player 2", g_player2);
    logf("[ewcoop] sneak mode cvar 0x9AC0558 = %d (0 = hold)\n", *reinterpret_cast<int*>(g_base + 0x9AC0558));
    snprintf(path, sizeof path, "%s\\c%lu.png", dir, pid);
    bool shot = saveWindowPng(g_subclassedWnd ? g_subclassedWnd : findGameWindow(), path);
    snprintf(path, sizeof path, "%s\\c%lu.txt", dir, pid);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "snapshot %s, copy pid %lu, net %s peer %d remote %d age %.0f ms, screenshot %s\n", id, pid,
            g_netStarted ? (g_netIsHost ? "host" : "client") : "off", g_netHavePeer, g_remoteValid, g_netAge,
            shot ? "ok" : "FAILED");
    fprintf(f, "map \"%s\", last nextmap \"%s\", host map \"%s\", steam lobby %llx peer %llx (steam mode %d)\n",
            currentMapName(gameLocal), g_lastNextmap, g_hostMap, (unsigned long long)g_steamLobby,
            (unsigned long long)g_steamPeer, g_steamMode);
    if (g_player1) {  // Joseph look on us: Sebastian's base model should have 0 visible pieces ("bodyless")
        void* inst = mainModelInstance(g_player1);
        fprintf(f, "my look: %s, Sebastian base model %p visible pieces %d\n", g_meJosephOn ? "Joseph" : "Sebastian",
                inst, inst ? visibleMeshCount(inst) : -1);
        // My attachments: 136-byte entries (+0 object, +124 id, +128 flags bit1 = detached); any string a field
        // points to (model names) is printed, plus the visible piece count of the attachment's model instance.
        void* coll = vfn<void* (*)(void*)>(g_player1, slot_actor_Attachments)(g_player1);
        char* list = coll && readable(coll, 16) ? field<char*>(coll, 0) : nullptr;
        int n = coll && readable(coll, 16) ? field<int>(coll, 8) : 0;
        fprintf(f, "my attachments: %d\n", n);
        for (int i = 0; list && i < n && i < 32 && readable(list + i * 136, 136); ++i) {
            char* en = list + i * 136;
            void* obj = *reinterpret_cast<void**>(en);
            const char* cls = className(obj);
            fprintf(f, "  #%d id %#x flags %#x obj %s", i, *reinterpret_cast<uint32_t*>(en + 124), en[128] & 0xFF, cls ? cls : "?");
            for (int off = 8; off < 120; off += 8) {
                const char* s = *reinterpret_cast<const char**>(en + off);
                if (reinterpret_cast<uintptr_t>(s) > 0x10000 && readable(s, 8) && isprint(static_cast<unsigned char>(s[0])) &&
                    isprint(static_cast<unsigned char>(s[1])) && isprint(static_cast<unsigned char>(s[2])))
                    fprintf(f, " [+%d \"%.60s\"]", off, s);
            }
            if (obj && readable(obj, 8)) {
                void* inner = vfn<void* (*)(void*)>(obj, 48 / 8)(obj);  // same as the game: obj->vfunc(+48) -> entity
                void* mi = inner ? fn<ModelInstance_t>(rva_ModelInstance)(inner) : nullptr;
                if (mi) fprintf(f, " model instance visible pieces %d", visibleMeshCount(mi));
            }
            fprintf(f, "\n");
        }
    }
    snapPlayer(f, "player 1 (me)", g_player1);
    snapPlayer(f, "player 2 (other)", g_player2);
    fprintf(f, "player 2: joseph look %d, attached to %s, position fixes paused %d ms, kill-forced enemy %p\n",
            g_josephBody, g_p2SyncOwner ? (entityName(g_p2SyncOwner) ? entityName(g_p2SyncOwner) : "?") : "nothing",
            g_p2SyncUntil ? static_cast<int>(g_p2SyncUntil - GetTickCount()) : 0, g_aiForceP2Owner);
    fprintf(f, "remote packet: pos (%.0f %.0f %.0f) health %.1f buttons %#x state %#x weapon \"%.47s\"\n", g_view.px,
            g_view.py, g_view.pz, g_view.health, g_view.buttons, g_view.state, g_view.weapon);
    // Flag-like fields that differ between player 1 and player 2 (scripts restrict only player 1, e.g. chapter 1
    // walk-only / no weapons). Bytes 0..8 on both sides, skipping 8-byte slots that hold a pointer on either.
    if (g_player1 && g_player2 && readable(g_player1, 0x3400) && readable(g_player2, 0x3400)) {
        auto* a = static_cast<const uint8_t*>(g_player1);
        auto* b = static_cast<const uint8_t*>(g_player2);
        auto ptrLike = [](const uint8_t* p) {
            uint64_t v; memcpy(&v, p, 8);
            return v >= 0x10000 && v < 0x800000000000ull;
        };
        fprintf(f, "\nplayer 1 vs player 2 flag-like fields (offset: p1 p2):\n");
        int shown = 0;
        for (int off = 0; off < 0x3400 && shown < 250; off += 8) {
            if (ptrLike(a + off) || ptrLike(b + off)) continue;
            for (int k = 0; k < 8; ++k)
                if (a[off + k] != b[off + k] && a[off + k] <= 8 && b[off + k] <= 8) {
                    fprintf(f, "  +%d: %u %u\n", off + k, a[off + k], b[off + k]);
                    ++shown;
                }
        }
    }
    // Characters within 2500 units of me.
    fprintf(f, "\ncharacters near me:\n");
    char* mgr = field<char*>(gameLocal, off_gameLocal_entMgr);
    if (mgr && g_playerPosValid[0] && readable(mgr + 40, 131072 * 8)) {
        auto** list = reinterpret_cast<void**>(mgr + 40);
        Vec3 me = g_playerPos[0];
        for (int i = 0; i < 131072; ++i) {
            void* c = list[i];
            if (!c || c == g_player1 || c == g_player2 || !readable(c, 8)) continue;
            uintptr_t vt = *reinterpret_cast<uintptr_t*>(c);
            if (vt < g_base || vt >= g_exeEnd || !isCharacterVtable(vt - g_base)) continue;
            if (const char* cn = entityName(c); cn && (strstr(cn, "corpse") || strstr(cn, "ragdoll"))) continue;
            Vec3 p;
            if (!entityPos(c, p)) continue;
            float d = std::sqrt((p.x - me.x) * (p.x - me.x) + (p.y - me.y) * (p.y - me.y) + (p.z - me.z) * (p.z - me.z));
            if (d > 2500.f) continue;
            const char* cls = className(c);
            fprintf(f, "  %s (%s) %.0f away, pos (%.0f %.0f %.0f), health %.1f", entityName(c) ? entityName(c) : "?",
                    cls ? cls : "?", d, p.x, p.y, p.z, *reinterpret_cast<float*>(static_cast<char*>(c) + 0x8C4));
            if (cls && strcmp(cls, ".?AVidNpc_Nata@@") == 0)
                fprintf(f, ", Sadist state %d", *reinterpret_cast<int*>(static_cast<char*>(c) + 9564));
            if (EnemyRecv* r = findRecv(entityName(c) ? entityName(c) : ""))
                fprintf(f, ", streamed (last pose %.1f s ago)", g_enemyTime - r->t1);
            fprintf(f, "\n");
        }
    }
    // Recent events from our log (the periodic diagnostics left out).
    fprintf(f, "\nrecent events:\n");
    snprintf(path, sizeof path, "%s\\ewcoop_%lu.log", g_dir, pid);
    if (FILE* lf = fopen(path, "rb")) {
        static char buf[400000];
        fseek(lf, 0, SEEK_END);
        long size = ftell(lf), start = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
        fseek(lf, start, SEEK_SET);
        size_t n = fread(buf, 1, sizeof buf - 1, lf);
        buf[n] = 0;
        fclose(lf);
        static const char* const kSkip[] = {"] angles", "] health p", "] camera p", "] weapons p", "body at", "peer=",
                                            "p2 speed", "base model visible", "[woundscan]", "[aimscan]"};
        const char* lines[40]; int count = 0;
        for (char* line = strtok(buf, "\r\n"); line; line = strtok(nullptr, "\r\n")) {
            bool skip = false;
            for (const char* s : kSkip) skip |= strstr(line, s) != nullptr;
            if (skip) continue;
            if (count == 40) { memmove(lines, lines + 1, sizeof lines[0] * 39); count = 39; }
            lines[count++] = line;
        }
        for (int i = 0; i < count; ++i) fprintf(f, "  %s\n", lines[i]);
    }
    fclose(f);
    logf("[ewcoop] snapshot %s written to %s\n", id, dir);
}
static void snapshotHotkey(void* gameLocal) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char id[32];
    snprintf(id, sizeof id, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    takeSnapshot(gameLocal, id);
    if (g_netStarted && g_netHavePeer) {  // the other copy snapshots into the same folder
        NetEvt e = {};
        e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtSnapshot;
        e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
        strncpy(e.target, id, sizeof e.target - 1);
        sendEvent(e);
    }
}

static int64_t hkRunFrame(void* self, int64_t a2, int64_t a3, char a4) {
    g_enemySnapCount = 0;  // this frame's host-side enemy collection starts fresh (filled during RunFrame)
    g_levelRestarted = false;
    int64_t r = g_origRunFrame(self, a2, a3, a4);
    if (g_unhook) return r;
    if (g_levelRestarted) {  // the game restarted from a checkpoint inside this frame: the level is gone
        g_ctlCount = 0;
        g_enemyRecvCount = g_enemySnapCount = 0;
        return r;
    }
    // Once the Render hook has captured the render-world pointer, unhook Render immediately so only one
    // frame is ever exposed to our hook (the render world object persists, so the pointer stays valid).
    if (g_renderHookOn && g_renderWorld) {
        g_renderHookOn = false;
        auto* p = reinterpret_cast<unsigned char*>(g_base + rva_RenderWorld_Render);
        DWORD old;
        VirtualProtect(p, sizeof kRenderPrologue, PAGE_EXECUTE_READWRITE, &old);
        memcpy(p, kRenderPrologue, sizeof kRenderPrologue);
        VirtualProtect(p, sizeof kRenderPrologue, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, sizeof kRenderPrologue);
        logf("[ewcoop] render-world hook removed; render world = %p\n", g_renderWorld);
    }
    checkLastError();

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = g_last.QuadPart ? float(now.QuadPart - g_last.QuadPart) / float(g_freq.QuadPart) : 0.f;
    g_last = now;
    if (dt > 0.1f) dt = 0.1f;

    updateHud();  // compute HUD/tag state; the GDI overlay window is disabled (it wedged the renderer)

    // The engine print listener + net_verbose are opt-in (Ctrl+Shift+L, or auto for host/join) so a plain
    // injection touches nothing of the engine's print chain.
    if (g_captureNet && !g_printListenerOn) {
        g_printListenerOn = true;
        installPrintListener();
        runConsole("net_verbose 1");
    }
    {
        static bool directSet;
        if (!directSet) { directSet = true; forceDirectBackend(); }  // before any hosting/joining
    }

    static bool adopted;
    if (!adopted) {  // first frame after (re)load: pick up a player 2 spawned by an older ewcoop
        adopted = true;
        // Dump the "player" and Joseph entityDef state text (idDeclEntityDef +0x1A0 idStr) to files.
        auto find = fn<FindEntityDef_t>(rva_FindEntityDef);
        void* list = reinterpret_cast<void*>(g_base + rva_entityDefList);
        const char* names[] = {"player", "ai/partner/joseph"};
        const char* files[] = {"decl_player.txt", "decl_joseph.txt"};
        for (int i = 0; i < 2; ++i) {
            char* decl = static_cast<char*>(find(list, names[i], 0, 0, 0, 0, 0, 0, 0));
            if (!decl) continue;
            int len = *reinterpret_cast<int*>(decl + 0x1A0);
            const char* text = *reinterpret_cast<const char**>(decl + 0x1A8);
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s\\%s", g_dir, files[i]);
            if (FILE* f = fopen(path, "wb")) {
                if (text && len > 0) fwrite(text, 1, len, f);
                fclose(f);
            }
            logf("[ewcoop] decl %s at %p, state text %d bytes -> %s\n", names[i], decl, len, files[i]);
        }
        g_game = self;
        char buf[32];
        if (GetEnvironmentVariableA("EWCOOP_PLAYER2", buf, sizeof buf)) {
            void* p = reinterpret_cast<void*>(strtoull(buf, nullptr, 16));
            // The recorded pointer may be a player 2 a reload already deleted (freed memory can still look
            // like a player): only adopt him if the entity list still has him under his name.
            const char* nm = p && p != localPlayer(self) && isA(p, rva_idPlayer_vtable) ? entityName(p) : nullptr;
            void* live = nm ? vfn<void* (*)(void*, const char*)>(self, 984 / 8)(self, nm) : nullptr;
            if (!live || live != p) {
                if (p) logf("[ewcoop] recorded player 2 %p is gone, not adopting him\n", p);
                SetEnvironmentVariableA("EWCOOP_PLAYER2", nullptr);
            } else {
                g_player2 = p;
                g_p2Yaw = yawOf(p);
                logf("[ewcoop] picked up existing player 2 %p\n", p);
            }
        }
    }
    if (self != g_game) {  // new map / reload: old entity pointers are gone
        if (g_game) logf("[ewcoop] game instance changed, forgetting Joseph\n");
        g_game = self;
        g_joseph = nullptr;
        g_player2 = nullptr;
        g_puppet = g_moving = false;
        g_josephBody = false;
        g_playAsP2 = false;
        g_sebastian = nullptr;
        g_reloadT = 0.f;  // reset the settle timer; SetupPlayer(1) must wait for the level to finish loading
        armRenderHook();   // re-capture the (new) render world for this map
    }
    if (g_joseph && !isA(g_joseph, rva_idPartner01_vtable)) {
        logf("[ewcoop] Joseph entity is gone\n");
        g_joseph = nullptr;
        g_puppet = g_moving = false;
    }

    // pressed() consumes the key edge, so read F9 once and route it by modifier.
    bool f9 = pressed(VK_F9);
    bool f9Ctrl = f9 && held(VK_CONTROL);
    if (f9 && !f9Ctrl) {  // plain F9: AI partner Joseph
        void* player = localPlayer(self);
        logf("[ewcoop] F9: player = %p\n", player);
        if (!g_joseph) g_joseph = spawnJoseph(self);
        if (g_joseph && player) bringToPlayer(player);
    }
    // Ctrl+F8 = a SECOND Sebastian (default, correct rig); Ctrl+Alt+F8 = whole-model swap to Joseph
    if (pressed(VK_F8) && held(VK_CONTROL))
        spawnSecondPlayer(self, held(VK_MENU) ? SpawnJoseph : SpawnSebastian);
    if (f9Ctrl && !g_player2) logf("[ewcoop] Ctrl+F9: no player 2 (press Ctrl+F8 first)\n");
    // Ctrl+Shift+G: find which attachment is Sebastian's hair. Each press lists player 2's attachments
    // (idActor vfunc 112: idList ptr/num, 136-byte entries: +0 object, +124 id, +128 flags) and hides the next
    // one; Ctrl+Shift+V shows them all again. (Was Ctrl+Alt+F9, which Windows/drivers can swallow.)
    bool attachHide = held(VK_CONTROL) && held(VK_SHIFT) && pressed('G');
    bool attachShow = held(VK_CONTROL) && held(VK_SHIFT) && pressed('V');
    if ((attachHide || attachShow) && !g_player2) logf("[ewcoop] attachments: no player 2 (Ctrl+F8 spawns one)\n");
    if (g_player2 && (attachHide || attachShow)) {
        void* coll = vfn<void* (*)(void*)>(g_player2, slot_actor_Attachments)(g_player2);
        char* list = coll && readable(coll, 16) ? field<char*>(coll, 0) : nullptr;
        int n = coll && readable(coll, 16) ? field<int>(coll, 8) : 0;
        static int next;
        bool restore = attachShow;
        logf("[ewcoop] player 2 attachments: %d%s\n", n, restore ? " (showing all)" : "");
        for (int i = 0; list && i < n && i < 32 && readable(list + i * 136, 136); ++i) {
            char* en = list + i * 136;
            void* obj = *reinterpret_cast<void**>(en);
            uint32_t id = *reinterpret_cast<uint32_t*>(en + 124);
            const char* cls = className(obj);
            logf("[ewcoop]   #%d id %#x flags %#x obj %p %s%s\n", i, id, en[128] & 0xFF, obj, cls ? cls : "?",
                 !restore && i == next % n ? "  <- hiding this one" : "");
            if (restore) fn<void (*)(void*, uint32_t*)>(rva_ActivateAttachment)(coll, &id);
            else if (i == next % n) fn<void (*)(void*, uint32_t*, char)>(rva_DetachAttachment)(coll, &id, 0);
        }
        if (!restore && n > 0) ++next;
        if (restore) next = 0;
    } else if (g_player2 && f9Ctrl) {
        void* anim = field<void*>(g_player2, off_entity_anim);
        void* inst = anim ? fn<ModelInstance_t>(rva_ModelInstance)(vfn<void* (*)(void*)>(anim, 48 / 8)(anim)) : nullptr;
        if (held(VK_SHIFT)) {  // cycle the main model's head variant
            static const char* variants[] = {"normal", "headless", "bodyless", "none", "hidden"};
            static int vi;
            vi = (vi + 1) % 5;
            if (inst) fn<SetModelVariant_t>(rva_SetModelVariant)(inst, variants[vi], nullptr);
            logf("[ewcoop] player 2 main model %p head variant -> \"%s\"\n", inst, variants[vi]);
        } else {  // toggle Joseph's mesh through the game's alternate-body switch
            g_josephBody = !g_josephBody;
            g_wantJoseph = g_josephBody;
            saveJosephChoice();
            void* r = vfn<AltBody_t>(g_player2, slot_player_AltBody)(g_player2, g_josephBody, "pl/pl9000.md6");
            logf("[ewcoop] player 2 alt body pl9000 %s (returned %p, main model %p)\n",
                 g_josephBody ? "ON" : "OFF", r, inst);
        }
    }
    // A save load deletes player 2 without a new game instance. Freed memory can still look like a player
    // (vtable intact), so isA alone passed and the 3 s diagnostics then read a dead controller: both copies
    // crashed on load. Confirm he is still in the entity list: look him up by the name he had when we got him.
    {
        static void* known;
        static char name[64];
        if (g_player2 != known) {
            known = g_player2;
            name[0] = 0;
            if (const char* n = g_player2 ? entityName(g_player2) : nullptr) strncpy(name, n, sizeof name - 1);
        }
        if (g_player2 && name[0]) {
            void* found = vfn<void* (*)(void*, const char*)>(self, 984 / 8)(self, name);
            if (found != g_player2) {
                SetEnvironmentVariableA("EWCOOP_PLAYER2", nullptr);
                logf("[ewcoop] player 2 (%s) is no longer in the level (save load?), dropping him\n", name);
                if (g_playAsP2) { setPlayerSlot(self, g_sebastian); g_playAsP2 = false; }
                g_player2 = nullptr;
                known = nullptr;
                g_josephBody = false;
            }
        }
    }
    if (g_player2 && !isA(g_player2, rva_idPlayer_vtable)) {
        logf("[ewcoop] player 2 entity is gone\n");
        if (g_playAsP2) { setPlayerSlot(self, g_sebastian); g_playAsP2 = false; }
        g_player2 = nullptr;
        g_josephBody = false;
    }
    g_p2Presentable = g_player2 ? presentableOf(g_player2) : nullptr;  // checked above: safe to read here
    {
        // Refresh player 1 here, before netRun: it used last frame's pointer, which a death reload had freed
        // (the 3 s diagnostics crashed reading it). Same assignment as later in this function.
        g_player1 = g_playAsP2 ? g_sebastian : localPlayer(self);
        void* ps[2] = {g_player1, g_player2};
        for (int i = 0; i < 2; ++i) {
            g_playerPosValid[i] = ps[i] && physicsOf(ps[i]);
            if (g_playerPosValid[i]) g_playerPos[i] = originOf(ps[i]);
        }
        g_player1Alive = ps[0] && *reinterpret_cast<float*>(static_cast<char*>(ps[0]) + 0x8C4) > 0.f;
        // Shared death: when our player dies, the other game takes its player down too, so both reload the same
        // checkpoint and the two worlds stay at the same point of the story.
        // Our player died (health 0): tell the other game right away so both deaths play together. Scripted
        // kills that never set health to 0 are sent from the checkpoint-restart hook instead.
        static bool wasAlive;
        if (wasAlive && !g_player1Alive && ps[0] && g_netStarted && g_netHavePeer && !g_diedFromRemote) {
            NetEvt e = {};
            e.magic = kNetMagic; e.version = kNetVersion; e.type = kEvtDeath;
            e.id = ++g_evtSeq ^ (GetCurrentProcessId() << 20);
            sendEvent(e);
            logf("[net] I died -> the other player dies too\n");
        }
        wasAlive = g_player1Alive;
        if (g_player1Alive) g_diedFromRemote = false;
    }
    // Joseph look is a lasting choice (Ctrl+F9): re-apply it when player 2 respawns (death reload, reconnect),
    // a second after he appears so his model is fully set up. A partner who plays AS Joseph (his packet's state
    // bit6) is always shown as Joseph here.
    {
        static void* seen; static float age;
        if (g_player2 != seen) { seen = g_player2; age = 0.f; }
        age += dt;
        bool want = g_wantJoseph || g_remoteIsJoseph;
        if (g_player2 && want && !g_josephBody && age > 1.f) {
            g_josephBody = true;
            vfn<AltBody_t>(g_player2, slot_player_AltBody)(g_player2, true, "pl/pl9000.md6");
            logf("[ewcoop] player 2 shown as Joseph%s\n", g_remoteIsJoseph ? " (he plays as Joseph)" : "");
        } else if (g_player2 && !want && g_josephBody && age > 1.f) {
            g_josephBody = false;
            vfn<AltBody_t>(g_player2, slot_player_AltBody)(g_player2, false, "pl/pl9000.md6");
            logf("[ewcoop] player 2 shown as Sebastian again\n");
        }
    }
    // Ctrl+Shift+X: play as Joseph yourself (same body switch on your own player; Sebastian's animations).
    {
        if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('X')) {
            g_meJoseph = !g_meJoseph;
            g_meJosephChosen = true;
            saveJosephChoice();
            logf("[ewcoop] you play as %s\n", g_meJoseph ? "Joseph" : "Sebastian");
        }
        // Default pairing: the host is Sebastian, the joining player is Joseph (unless he chose otherwise).
        if (!g_meJosephChosen) {
            bool joined = g_netStarted && !g_netIsHost && !g_netLoopback && g_netHavePeer;
            if (joined != g_meJoseph) {
                g_meJoseph = joined;
                if (joined) logf("[ewcoop] you joined: you play as Joseph (Ctrl+Shift+X to switch, remembered)\n");
            }
        }
        static void* seen; static float age;
        if (g_player1 != seen) { seen = g_player1; age = 0.f; g_meJosephOn = false; }
        age += dt;
        if (g_player1 && g_player1Alive && age > 1.f && g_meJoseph != g_meJosephOn) {
            vfn<AltBody_t>(g_player1, slot_player_AltBody)(g_player1, g_meJoseph, "pl/pl9000.md6");
            g_meJosephOn = g_meJoseph;
        }
        if (g_meJosephOn && g_player1)  // keep Sebastian's body hidden under Joseph's mesh (as for player 2)
            if (void* inst = mainModelInstance(g_player1)) fn<SetModelVariant_t>(rva_SetModelVariant)(inst, "bodyless", nullptr);
        josephExtras(g_player1, g_meJosephOn);
        if (g_player2) josephExtras(g_player2, g_josephBody);
        // Joseph's model (pl9000) has a cut-neck piece for a scene; his "normal" variant hides it (the AI Joseph
        // gets that at spawn, our attached copy didn't). Re-applied twice a second in case a scene resets it.
        static float variantT;
        if ((variantT -= dt) <= 0.f) {
            variantT = 0.5f;
            if (g_meJosephOn) josephNormalVariant(g_player1);
            if (g_player2 && g_josephBody) josephNormalVariant(g_player2);
        }
    }
    // Player 1's newest usercmd from the idUserCmdMgr RunFrame was given (see game.h).
    const Usercmd* p1cmd = nullptr;
    if (a2) {
        int wf = *reinterpret_cast<int*>(a2 + 0x4000);
        int idx = (wf > 1 ? wf - 1 : 0) % 128;
        p1cmd = reinterpret_cast<const Usercmd*>(a2 + idx * 0x40);
    }
    if (pressed(VK_F5) && held(VK_CONTROL) && p1cmd) dumpUsercmd("player 1", *p1cmd);
    netRun(self, p1cmd, dt);  // our own UDP transform sync (no-op unless a net session is running)
    if (g_levelRestarted) return r;  // a shared restart (other player died) just wiped the level
    botStep(self, dt);        // solo follow-bot (no-op unless Ctrl+Shift+B is on)
    // Ctrl+F7: switch which character the game drives (camera + input + HUD). Points the player slot at P2;
    // the presentable IsLocal hook keeps the HUD alive for him.
    if (g_player2 && held(VK_CONTROL) && pressed(VK_F7)) {
        void* sebastian = g_playAsP2 ? g_sebastian : localPlayer(self);
        g_playAsP2 = !g_playAsP2;
        if (g_playAsP2) { g_sebastian = sebastian; setPlayerSlot(self, g_player2); }
        else            { setPlayerSlot(self, g_sebastian); }
        logf("[ewcoop] now controlling: %s\n", g_playAsP2 ? "player 2 (Joseph)" : "player 1 (Sebastian)");
    }
    g_player1 = g_playAsP2 ? g_sebastian : localPlayer(self);
    hookPresentableLocal();  // once P1 is known
    if (pressed(VK_F12) && held(VK_CONTROL)) {
        if (held(VK_SHIFT)) {  // Ctrl+Shift+F12: snapshot both copies for a bug report
            snapshotHotkey(self);
        } else {  // entityDef addresses, for scanning their layout
            auto find = fn<FindEntityDef_t>(rva_FindEntityDef);
            void* list = reinterpret_cast<void*>(g_base + rva_entityDefList);
            logf("[ewcoop] decls: player=%p joseph=%p\n", find(list, "player", 0, 0, 0, 0, 0, 0, 0),
                 find(list, "ai/partner/joseph", 0, 0, 0, 0, 0, 0, 0));
        }
    }
    if (pressed(VK_F3) && held(VK_CONTROL)) {
        sneakDiag("player 1", g_player1);
        sneakDiag("player 2", g_player2);
    }
    if (pressed(VK_F1) && held(VK_CONTROL)) { if (g_player2) dumpMeshKits(g_player2); mpDiag(); }
    if (held(VK_CONTROL) && pressed(VK_F10)) {  // aggro split on/off (checked before the plain-F10 puppet key)
        g_aiAggro = !g_aiAggro;
        logf("[ewcoop] AI aggro %s\n", g_aiAggro ? "on (enemies target the nearest player)" : "off (player 1 only)");
    }
    if (held(VK_CONTROL) && pressed(VK_F11)) {  // Ctrl+F11 ewcmd.txt; Ctrl+Shift+F11 dumps all commands
        if (held(VK_SHIFT)) dumpCommands();
        else runConsoleFile();
    }
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('J')) {  // Ctrl+Shift+J: join as a client (this instance)
        char ip[64] = {};
        if (!GetEnvironmentVariableA("EWCOOP_HOST", ip, sizeof ip) || !ip[0]) {
            char path[MAX_PATH];  // no env var: fall back to ewhost.txt next to the DLL (simplest for a friend)
            snprintf(path, sizeof path, "%s\\ewhost.txt", g_dir);
            if (FILE* f = fopen(path, "rb")) {
                if (!fgets(ip, sizeof ip, f)) ip[0] = 0;
                fclose(f);
                for (char* p = ip; *p; ++p)
                    if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') { *p = 0; break; }
                if (ip[0] == '#' || ip[0] == ';') ip[0] = 0;  // comment / placeholder line
            }
        }
        if (!ip[0]) strcpy(ip, "127.0.0.1");
        const char* port = "27015";
        auto vp = reinterpret_cast<char**>(g_base + rva_cvarNetPort + off_cvar_valueString);
        if (readable(vp, 8) && *vp) port = *vp;
        runConsole("net_usePlatformBackend 0");
        forceDirectBackend();
        g_captureNet = true;
        char cmd[96];
        snprintf(cmd, sizeof cmd, "connect %s", ip);  // net_port comes from the net_port cvar
        logf("[ewcoop] joining host %s (net_port %s); net_verbose on, logging net status for 60s\n", ip, port);
        runConsole(cmd);
        g_netWatch = true;
        g_netTimer = 60.f;
    }
    // OUR netcode: Ctrl+Shift+K host, Ctrl+Shift+O join (ewhost.txt), Ctrl+Shift+U stop.
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('K')) {
        if (netStart(true, nullptr)) steamCreateLobby();  // UDP (VPN/LAN) and a Steam lobby, whichever joins
    }
    bool joinKey = held(VK_CONTROL) && held(VK_SHIFT) && pressed('O');
    // Join: a friend's Steam co-op lobby first (no IP needed); ewhost.txt / UDP only if nobody is hosting on Steam.
    if (joinKey && steamJoinFriendLobby()) joinKey = false;
    if (joinKey) {
        char ip[64] = {};
        if (!GetEnvironmentVariableA("EWCOOP_HOST", ip, sizeof ip) || !ip[0]) {
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s\\ewhost.txt", g_dir);
            if (FILE* f = fopen(path, "rb")) {
                // First line that isn't a comment (the shipped file starts with # help lines; only reading the
                // first line made every join fall back to 127.0.0.1).
                char line[128];
                while (fgets(line, sizeof line, f)) {
                    char* q = line;
                    while (*q == ' ' || *q == '\t') ++q;
                    if (*q == '#' || *q == ';' || *q == '\r' || *q == '\n' || !*q) continue;
                    strncpy(ip, q, sizeof ip - 1);
                    break;
                }
                fclose(f);
                for (char* q = ip; *q; ++q)
                    if (*q == '\r' || *q == '\n' || *q == ' ' || *q == '\t') { *q = 0; break; }
            }
        }
        if (!ip[0]) strcpy(ip, "127.0.0.1");
        netStart(false, ip);
    }
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('U')) {
        if (g_netStarted && !g_netIsHost && g_coopCharApplied && g_player1 && g_player1Alive) {
            saveCharacter(g_player1);
            logf("[coop] co-op character saved\n");
        }
        netStop();
        steamLeave();
    }
    // Ctrl+Shift+M (client): load the host's map at the host's checkpoint (nextmap, the game's own way).
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('M') && g_origCmdBuffer) goToHostMap();
    {   // Steam callbacks (friend clicked Join Game) need our listeners registered before the click
        static bool once;
        if (!once) { once = true; steamInit(); }
        static float presenceT;
        if ((presenceT -= dt) <= 0.f) { presenceT = 2.f; updateRichPresence(currentMapName(self)); }
    }
    coopCharacterTick(self, dt);  // visiting player: co-op character on join, kept in its file
    menuJoinTick(self);           // visiting player in the main menu: load the host's map and checkpoint
    // Ctrl+Shift+L (solo test, from the main menu): load chapter 1 with the menu-safe map command.
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('L')) {
        logf("[coop] test: loading chapter 1 from here with slowmap\n");
        queueCommand("slowmap \"root/chapter/an_emergency_call\"\n");
    }
    // Ctrl+Shift+H: testing aid, put this copy's own player at 1 health (lowest that is still alive).
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('H')) {
        if (void* p1 = localPlayer(self)) {
            float& hp = *reinterpret_cast<float*>(static_cast<char*>(p1) + 0x8C4);
            logf("[ewcoop] test: health %.0f -> 1\n", hp);
            hp = 1.f;
        }
    }
    // Ctrl+Alt+F6 (works in ALL copies, focused or not): write-watch both players' in-hand weapon slot
    // (player + 2728 + 16) to find what switches player 1 when player 2 is equipped.
    {
        static bool down;
        bool now = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                   (GetAsyncKeyState(VK_F6) & 0x8000);
        if (now && !down && g_player1) {
            static bool watching;
            watching = !watching;
            if (watching) {
                // Now: health (player + 0x8C4). Enemy hits on the player don't pass idPlayer::Damage (slot 80),
                // so this finds the code that really lowers it.
                g_watch[0] = reinterpret_cast<uintptr_t>(g_player1) + 0x8C4;
                g_watch[1] = g_player2 ? reinterpret_cast<uintptr_t>(g_player2) + 0x8C4 : 0;
                g_watchReads = false;
                g_watchExec = false;
                g_watchSeenCount = 0;
                g_watchHits = 0;
                logf("[ewcoop] health watch ON: DR0 = player 1 (%p), DR1 = player 2 (%p)\n", g_player1, g_player2);
            } else {
                logf("[ewcoop] health watch OFF, hits %ld\n", g_watchHits);
            }
            if (HANDLE th = CreateThread(nullptr, 0, watchThread, watching ? (LPVOID)1 : nullptr, 0, nullptr)) CloseHandle(th);
        }
        down = now;
    }
    // Ctrl+Shift+F1: let a second copy of the game start on this PC (for two-copy testing).
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed(VK_F1)) {
        auto* h = reinterpret_cast<HANDLE*>(g_base + rva_instanceMutex);
        if (*h) {
            BOOL ok = CloseHandle(*h);
            *h = nullptr;
            logf("[ewcoop] instance mutex released (%s): you can start a second copy now\n", ok ? "ok" : "CloseHandle failed");
        } else {
            logf("[ewcoop] instance mutex already released\n");
        }
    }
    // Ctrl+Shift+E: loopback "echo" test of the whole netcode on one PC (player 2 replays you, delayed).
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('E')) {
        if (g_netLoopback) netStop();
        else netStartLoopback();
    }
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('B')) {  // Ctrl+Shift+B: solo follow-bot on/off
        g_bot = !g_bot;
        logf("[bot] follow-bot %s\n", g_bot ? "ON" : "OFF");
    }
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('J')) {  // Ctrl+Shift+J: run player 2's camera calc (aim pitch test)
        g_p2CamCalc = !g_p2CamCalc;
        logf("[net] player 2 camera calc (experimental aim pitch) %s\n", g_p2CamCalc ? "ON" : "OFF");
    }
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('Y')) {  // Ctrl+Shift+Y: shared-enemy transform sync
        g_enemySync = !g_enemySync;
        logf("[net] enemy transform sync %s\n", g_enemySync ? "ON" : "OFF");
    }
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('T')) {  // teleport ME to the friend's position
        if (g_netSock != INVALID_SOCKET && g_remoteValid) {
            if (void* p1 = localPlayer(self)) {
                Vec3 dst{g_remotePkt.px, g_remotePkt.py + 60.f, g_remotePkt.pz};
                placePlayer(p1, dst);
                logf("[net] teleported to friend (%.0f %.0f %.0f)\n", dst.x, dst.y, dst.z);
            }
        } else {
            logf("[net] teleport: no net session / no remote yet\n");
        }
    }
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('N')) {  // Ctrl+Shift+N: toggle net status logging
        g_netWatch = !g_netWatch;
        if (g_netWatch) g_captureNet = true;  // also start capturing the engine NET trace
        logf("[ewcoop] net status logging %s (engine capture %s)\n", g_netWatch ? "ON" : "OFF",
             g_captureNet ? "ON" : "OFF");
    }
    if (held(VK_CONTROL) && !held(VK_SHIFT) && !held(VK_MENU) && pressed('H')) {  // Ctrl+H: toggle the debug HUD
        g_hudOn = !g_hudOn;
        logf("[ewcoop] debug HUD %s\n", g_hudOn ? "ON" : "OFF");
    }
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('M')) forceMember();  // Ctrl+Shift+M: force member experiment
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('I')) hostInfo();     // Ctrl+Shift+I: print host IP + port for the friend
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('P')) toggleEngineMP();  // Ctrl+Shift+P: engine 2nd player on/off
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('S')) mpStartMatch();  // Ctrl+Shift+S: StartMatch (Start Game)
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('L')) mpLoadMap();     // Ctrl+Shift+L: force LoadMap
    // Ctrl+F4: log every piece of code that reads player 1's controller buttons (both cmd slots).
    // Ctrl+F2: execute breakpoints on the input step 0x862650 and UpdateSneak 0x7F8910.
    if (pressed(VK_F2) && held(VK_CONTROL)) {
        static bool watching;
        watching = !watching;
        if (watching) {
            void* a1 = g_player1 ? field<void*>(g_player1, off_entity_anim) : nullptr;
            void* a2p = g_player2 ? field<void*>(g_player2, off_entity_anim) : nullptr;
            logf("[ewcoop] exec watch: player 1 %p (controller %p), player 2 %p (controller %p)\n", g_player1, a1,
                 g_player2, a2p);
            g_watch[0] = g_base + 0x862650;
            g_watch[1] = g_base + 0x7F8910;
            g_watchExec = true;
            g_watchReads = false;
            g_watchSeenCount = 0;
            g_watchHits = 0;
        } else {
            logf("[ewcoop] watch total hits: %ld\n", g_watchHits);
        }
        if (HANDLE th = CreateThread(nullptr, 0, watchThread, watching ? (LPVOID)1 : nullptr, 0, nullptr)) CloseHandle(th);
    }
    // Ctrl+Shift+Home: partner-death experiment. Kill the AI Joseph we spawned (F9) with the game's own "kill"
    // event, to see whether the partner-death sequence (the one we want for a co-op partner) plays.
    if (pressed(VK_HOME) && held(VK_CONTROL) && held(VK_SHIFT)) {
        if (g_joseph && validEnt(g_joseph)) {
            logf("[ewcoop] partner-death test: killing Joseph %p (%s)\n", g_joseph, entityName(g_joseph));
            fn<void (*)(void*, void*, void*)>(0x841560)(g_joseph, reinterpret_cast<void*>(g_base + 0x9B67D20), nullptr);
        } else {
            logf("[ewcoop] partner-death test: no Joseph (press F9 first)\n");
        }
    }
    // Death screen state (tngHUD_Dead static instance 0x9B7DEC0: state +96, timer +92): log every change, to see
    // which deaths go through it.
    {
        static int lastState = -999;
        int st = *reinterpret_cast<int*>(g_base + 0x9B7DEC0 + 96);
        if (st != lastState) {
            logf("[ewcoop] death screen state %d -> %d (timer %d)\n", lastState, st,
                 *reinterpret_cast<int*>(g_base + 0x9B7DEC0 + 92));
            lastState = st;
        }
    }
    // Ctrl+Shift+F4: same, but on player 2's controller.
    if (pressed(VK_F4) && held(VK_CONTROL) && g_player1) {
        static bool watching;
        watching = !watching;
        void* who = held(VK_SHIFT) && g_player2 ? g_player2 : g_player1;
        if (watching) {
            logf("[ewcoop] watching reads of %s's buttons\n", who == g_player2 ? "player 2" : "player 1");
            void* anim = field<void*>(who, off_entity_anim);
            char* ctrl = anim ? vfn<char* (*)(void*)>(anim, slot_anim_GetController)(anim) : nullptr;
            g_watch[0] = ctrl ? reinterpret_cast<uintptr_t>(ctrl) + 20460 : 0;  // slot A current buttons
            g_watch[1] = ctrl ? reinterpret_cast<uintptr_t>(ctrl) + 20616 : 0;  // slot B current buttons
            g_watchReads = true;
            g_watchExec = false;
            g_watchSeenCount = 0;
            g_watchHits = 0;
        } else {
            logf("[ewcoop] watch total hits: %ld\n", g_watchHits);
        }
        if (HANDLE th = CreateThread(nullptr, 0, watchThread, watching ? (LPVOID)1 : nullptr, 0, nullptr)) CloseHandle(th);
    }
    if (g_engineMP) {
        // Engine-managed 2nd player (Ctrl+Shift+P): drive the slot-1 player from the user-cmd manager. In a
        // networked session the transport fills slot 1's buffer from the joining client's usercmds, so this is
        // the host applying the remote player's input. Locally (no peer) the buffer is empty -> he just idles.
        int* np = reinterpret_cast<int*>(static_cast<char*>(self) + off_game_numPlayers);
        if (np && *np < 2) *np = 2;  // keep the count pinned so a reload spawns both slots
        void* p2 = playerAtSlot(self, 1);
        if (p2) {
            Usercmd c{};
            if (readUserCmdForPlayer(reinterpret_cast<void*>(a2), 1, &c))
                fn<PlayerSetUsercmd_t>(rva_PlayerSetUsercmd)(p2, &c, &c);
        } else {
            // A checkpoint/quick-load reload restores the world from a snapshot and never calls LoadMap,
            // so SetupPlayer(1) is skipped. Spawn slot 1 ourselves with the engine's own SetupPlayer (the
            // clamp is already NOPed). WAIT until the level has settled (the "player" model's skeleton is
            // only mounted once the world is live) or SetupPlayer dereferences a null mesh and crashes.
            static void* triedGame;
            static int tries;
            if (triedGame != self) { triedGame = self; tries = 0; }
            g_reloadT += dt;
            if (g_player1 && g_reloadT > 2.0f && tries < 5) {  // only in a live level (local player exists)
                ++tries;
                fn<void (*)(void*, int)>(rva_SetupPlayer)(self, 1);
                p2 = playerAtSlot(self, 1);
                logf("[ewcoop] engineMP: SetupPlayer(1) call %d -> slot1=%p\n", tries, p2);
            }
        }
        static float slotT;
        slotT += dt;
        if (slotT >= 3.f) {
            slotT = 0.f;
            Usercmd probe{};
            bool has = readUserCmdForPlayer(reinterpret_cast<void*>(a2), 1, &probe);
            void* s1 = playerAtSlot(self, 1);
            void* s0 = playerAtSlot(self, 0);
            Vec3 o = s1 ? originOf(s1) : Vec3{};
            int* fr = a2 ? reinterpret_cast<int*>(static_cast<char*>(reinterpret_cast<void*>(a2)) + 0x4000) : nullptr;
            logf("[ewcoop] engineMP numPlayers=%d slot0=%p slot1=%p at(%.0f %.0f %.0f) cmd=%s mgr+4000=%d,%d,%d,%d\n",
                 np ? *np : -1, s0, s1, o.x, o.y, o.z, has ? "remote" : "none",
                 fr ? fr[0] : -1, fr ? fr[1] : -1, fr ? fr[2] : -1, fr ? fr[3] : -1);
        }
    }
    if (g_player2) {
        bool netDrives = g_netStarted && g_remoteValid;  // netRun already gave him the remote's input this frame
        if (!g_bot && !g_puppet && !g_playAsP2 && !netDrives) feedPlayer2(dt, p1cmd);  // numpad driver
        if (g_playAsP2 && g_sebastian) feedIdle(g_sebastian, p1cmd);  // keep the left-behind player animating
        // The game's alt body sets the base model to "headless" (still 13 components: body + neck stump,
        // which is the flesh at the collar). "bodyless" is an empty entry (0 components), so forcing it
        // every frame leaves only Joseph's attached mesh (a separate model instance) visible.
        if (g_josephBody) {
            if (void* inst = mainModelInstance(g_player2)) {
                fn<SetModelVariant_t>(rva_SetModelVariant)(inst, "bodyless", nullptr);
                static float visT;
                visT += dt;
                if (visT >= 1.f) {
                    visT = 0.f;
                    logf("[ewcoop] base model visible components: %d\n", visibleMeshCount(inst));
                }
            }
        }
        if (pressed(VK_F7)) {  // bring player 2 in front of you
            if (void* p1 = localPlayer(self)) {
                Vec3 p = originOf(p1);
                float rr = yawOf(p1) / 57.2957795f;
                Vec3 dst{p.x + std::cos(rr) * 80.f, p.y + std::sin(rr) * 80.f, p.z + 8.f};
                placePlayer(g_player2, dst);
                logf("[ewcoop] F7: player 2 -> (%.0f %.0f %.0f)\n", dst.x, dst.y, dst.z);
            }
        }
        if (pressed(VK_F6) && held(VK_CONTROL) && !held(VK_MENU)) {  // Ctrl+Alt+F6 is the weapon-slot watch
            static bool watching;
            watching = !watching;
            if (watching) {
                void* body = playerBody(g_player2);
                // DR0 = clip model origin.x (what GetOrigin returns), DR1 = physics current origin.x (+232)
                g_watch[0] = body ? reinterpret_cast<uintptr_t>(vfn<GetVec_t>(body, slot_phys_GetOrigin)(body, 0)) : 0;
                g_watch[1] = body ? reinterpret_cast<uintptr_t>(body) + 232 : 0;
                g_watchReads = false;
                g_watchExec = false;
                g_watchSeenCount = 0;
                g_watchHits = 0;
            } else {
                logf("[ewcoop] watch total hits: %ld\n", g_watchHits);
            }
            if (HANDLE th = CreateThread(nullptr, 0, watchThread, watching ? (LPVOID)1 : nullptr, 0, nullptr)) CloseHandle(th);
        }
        static float t;
        t += dt;
        if (t >= 2.f) {  // watch where he is for a while
            t = 0.f;
            if (void* body = playerBody(g_player2)) {
                Vec3 b = bodyOrigin(body);
                logf("[ewcoop] player 2 body at (%.0f %.0f %.0f)\n", b.x, b.y, b.z);
            }
        }
    }
    if (pressed(VK_F10)) {
        if (g_puppet) stopMoving();
        g_puppet = g_joseph && !g_puppet;
        if (g_puppet) {
            g_lastPos = g_pos = originOf(g_joseph);
            g_bodyYaw = yawOf(g_joseph);
        }
        logf("[ewcoop] puppet mode %s\n", g_puppet ? "ON" : "OFF (or no Joseph: press F9)");
    }
    if (pressed(VK_F11)) {
        stopMoving();
        g_mode = (g_mode + 1) % ModeCount;
        if (g_joseph) {
            g_pos = originOf(g_joseph);
            g_bodyYaw = yawOf(g_joseph);
        }
        logf("[ewcoop] puppet movement: %s\n", kModeNames[g_mode]);
    }
    if (g_puppet) puppetStep(dt);

    if (g_netWatch) {  // periodic session/peer state so a join or host is readable in the log
        g_netTimer -= dt;
        if (g_netTimer <= 0.f) { g_netTimer = 2.f; netStatus(); }
    }

    if (pressed(VK_END) && held(VK_CONTROL)) {
        stopMoving();
        g_unhook = true;
        unhookAll();
    }
    return r;
}

static DWORD WINAPI init(LPVOID self) {
    char path[MAX_PATH];
    GetModuleFileNameA(static_cast<HMODULE>(self), path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) {
        *slash = 0;
        strcpy(g_dir, path);
        // Per-process log so two instances (host + client) don't clobber each other.
        snprintf(path, sizeof path, "%s\\ewcoop_%lu.log", g_dir, GetCurrentProcessId());
    }
    g_log = fopen(path, "w");
    QueryPerformanceFrequency(&g_freq);

    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_exeEnd = g_base + reinterpret_cast<IMAGE_NT_HEADERS*>(
        g_base + reinterpret_cast<IMAGE_DOS_HEADER*>(g_base)->e_lfanew)->OptionalHeader.SizeOfImage;
    logf("[ewcoop] loaded, game base %p\n", reinterpret_cast<void*>(g_base));
    AddVectoredExceptionHandler(1, crashLogger);
    AddVectoredExceptionHandler(1, watchHandler);
    hookDirectInput();
    installPurecallHandler();
    hookActorDamage();
    hookQueueRemove();
    hookSynchro();
    hookAnimCalls();
    // (0x82F090 turned out to be a gimmick "pressed" check, not the player's sneak: not hooked.)
    (void)&hookSneakPressed;
    hookGiveItem();
    hookVolumeTouch();
    hookRestart();
    hookCmdBuffer();
    // idGameLocal_LoadMap 0x79F2C0: any map load (menu reload, chapter select, nextmap, slowmap) deletes the level.
    // Forget every level pointer the moment it starts (a reload crashed the enemy AI on a deleted player 2).
    static const unsigned char kLoadMapHead[12] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
    logf("[ewcoop] map-load hook (forget level pointers on load): %s\n",
         installNotifyHook(0x79F2C0, kLoadMapHead, sizeof kLoadMapHead, [] {
             if (g_unhook) return;
             onLevelRestart();
             logf("[ewcoop] a map is loading: level pointers forgotten\n");
         }) ? "on" : "OFF");
    g_origBeginSave = reinterpret_cast<BeginSave_t>(installInlineHook(0x7DB860, kBeginSaveHead, sizeof kBeginSaveHead,
                                                                      reinterpret_cast<void*>(&hkBeginSave)));
    logf("[ewcoop] save hook (visiting player's campaign save protected): %s\n", g_origBeginSave ? "on" : "OFF");
    loadJosephChoice();
    g_origActorUpdate = reinterpret_cast<ActorUpdate_t>(
        installInlineHook(rva_ActorUpdate, kActorUpdatePrologue, sizeof kActorUpdatePrologue,
                          reinterpret_cast<void*>(&hkActorUpdate)));
    logf("[ewcoop] actor-update hook (shared enemy transforms): %s\n", g_origActorUpdate ? "on" : "OFF");
    g_origLaunch = reinterpret_cast<Launch_t>(installInlineHook(rva_LaunchProjectile, kLaunchPrologue,
                                                                sizeof kLaunchPrologue, reinterpret_cast<void*>(&hkLaunch)));
    logf("[ewcoop] projectile-launch hook (shot sync): %s\n", g_origLaunch ? "on" : "OFF");
    // Clear hardware breakpoints an older (reloaded) ewcoop may have left armed on the game's threads.
    if (HANDLE th = CreateThread(nullptr, 0, watchThread, nullptr, 0, nullptr)) CloseHandle(th);

    // Refuse to patch anything unless this is the exact build the addresses came from.
    void** spawnSlot = reinterpret_cast<void**>(g_base + rva_idGameLocal_vtable) + slot_SpawnEntityDef;
    if (*spawnSlot != reinterpret_cast<void*>(g_base + rva_SpawnEntityDef)) {
        logf("[ewcoop] vtable does not match build 2708258. Not hooking.\n");
        return 0;
    }
    if (!hookSlot(g_actionHook, rva_idPartner01_vtable, slot_partner_HandleAction,
                  rva_Partner01_HandleAction, reinterpret_cast<void*>(&hkHandleAction)))
        g_actionHook.slot = nullptr;
    g_origHandleAction = reinterpret_cast<HandleAction_t>(g_actionHook.orig);
    if (!hookSlot(g_damageHook, rva_idPlayer_vtable, slot_player_Damage, rva_PlayerDamage,
                  reinterpret_cast<void*>(&hkPlayerDamage)))
        g_damageHook.slot = nullptr;
    g_origDamage = reinterpret_cast<PlayerDamage_t>(g_damageHook.orig);
    if (!hookSlot(g_setHealthHook, rva_idPlayer_vtable, 89, 0x7BE370, reinterpret_cast<void*>(&hkSetHealth)))
        g_setHealthHook.slot = nullptr;
    g_origSetHealth = reinterpret_cast<SetHealth_t>(g_setHealthHook.orig);
    if (!hookSlot(g_thinkHook, rva_idPlayer_vtable, slot_player_Think, rva_PlayerThink,
                  reinterpret_cast<void*>(&hkPlayerThink)))
        g_thinkHook.slot = nullptr;
    g_origThink = reinterpret_cast<Think_t>(g_thinkHook.orig);
    if (g_thinkHook.slot) hookGetPlayer();
    if (!hookSlot(g_isLocalHook, rva_idPlayer_vtable, slot_player_IsLocal, rva_PlayerIsLocal,
                  reinterpret_cast<void*>(&hkIsLocal)))
        g_isLocalHook.slot = nullptr;
    g_origIsLocal = reinterpret_cast<IsLocal_t>(g_isLocalHook.orig);
    logf("[ewcoop] AI aggro hook: %s (enemies target the nearest player)\n", hookAiAggro() ? "on" : "OFF");
    armRenderHook();
    logf("[ewcoop] render-world hook (world->screen): %s\n", g_renderHookOn ? "on" : "OFF");
    if (!hookSlot(g_runFrameHook, rva_idGameLocal_vtable, slot_RunFrame, rva_RunFrame,
                  reinterpret_cast<void*>(&hkRunFrame)))
        return 0;
    g_origRunFrame = reinterpret_cast<RunFrame_t>(g_runFrameHook.orig);
    logf("[ewcoop] hooked. F9 spawn/bring Joseph, F10 puppet, F11 anim/slide, numpad moves, Ctrl+End unhook\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        if (HANDLE t = CreateThread(nullptr, 0, init, inst, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
