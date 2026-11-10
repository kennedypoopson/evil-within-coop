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
static VHook g_runFrameHook, g_actionHook, g_damageHook, g_thinkHook, g_isLocalHook;
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
static void* hkAiPlayer() {
    void* game = *reinterpret_cast<void**>(g_base + rva_gameLocal);
    void* p1 = game ? localPlayer(game) : nullptr;
    if (g_unhook || !g_aiAggro || !g_player2 || !g_aiThis) return p1;
    Vec3 e, a, b;
    if (entityOrigin(g_aiThis, &e) && entityOrigin(p1, &a) && entityOrigin(g_player2, &b)) {
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
static void* hkPresentableLocal(void* self) {
    if (!g_unhook && g_player2 && self == presentableOf(g_player2)) return reinterpret_cast<void*>(1);
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
    logf("[ewcoop]   spawned %p, is idPlayer: %d; player slot 0 is now %p (%s)\n", ent,
         isA(ent, rva_idPlayer_vtable), p1after, p1after == p1 ? "unchanged" : "CHANGED");
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
static int64_t hkPlayerThink(void* self) {
    if (g_unhook || g_ctxSwapOff) return g_origThink(self);
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

// No friendly fire between the two players; enemies and the world still hurt both.
static double hkPlayerDamage(void* self, void* inflictor, void* attacker, void* decl, void* a5, float* a6, void* loc) {
    if (!g_unhook && g_player2 && g_player1) {
        void* other = self == g_player2 ? g_player1 : self == g_player1 ? g_player2 : nullptr;
        if (other && (attacker == other || inflictor == other)) return 0.0;
    }
    return g_origDamage(self, inflictor, attacker, decl, a5, a6, loc);
}

static void unhookAll() {
    if (g_subclassedWnd && g_origWndProc)
        SetWindowLongPtrA(g_subclassedWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
    if (g_playAsP2 && g_game) setPlayerSlot(g_game, g_sebastian);
    g_playAsP2 = false;
    if (g_damageHook.slot) writeSlot(g_damageHook.slot, g_damageHook.orig);
    if (g_thinkHook.slot) writeSlot(g_thinkHook.slot, g_thinkHook.orig);
    if (g_isLocalHook.slot) writeSlot(g_isLocalHook.slot, g_isLocalHook.orig);
    if (g_getPlayerHooked) writeCode(rva_GetPlayer, kGetPlayerHead, sizeof kGetPlayerHead);
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
static bool g_netStarted, g_netIsHost, g_netLoopback;
static uint32_t g_netSeq, g_netLastRecvSeq;
static float g_netSendT, g_netLogT, g_netAge;  // g_netAge: seconds since the newest remote packet
static uint32_t g_netRecvCount, g_netDropCount, g_netSnapCount;
#pragma pack(push, 1)
struct NetPkt {
    uint32_t magic; uint8_t version, type; uint16_t flags; uint32_t seq; uint32_t timeMs;
    float px, py, pz;        // body origin
    float vx, vy, vz;        // velocity (units/s), for extrapolation
    float pitch, yaw;        // camera view angles (degrees), controller +20524
    float health;
    int32_t buttons; int16_t buttons2; int8_t fwd, right, up; uint8_t pad;
    float bodyYaw;           // body facing (motion object rotation); separate from the camera in third person
    char weapon[48];         // equipped inventory item decl name, e.g. "weapon/pistol" ("" = none)
};
#pragma pack(pop)
static const uint32_t kNetMagic = 0x50435745;  // 'EWCP' little-endian
static const uint8_t kNetVersion = 3;          // older peers are ignored (both PCs need this build)

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

static void netStop() {
    if (g_netSock != INVALID_SOCKET) {
        closesocket(g_netSock);
        g_netSock = INVALID_SOCKET;
        SetEnvironmentVariableA("EWCOOP_SOCKET", nullptr);
    }
    g_netHavePeer = g_remoteValid = g_netStarted = g_netLoopback = false;
    g_loopCount = 0;
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
    } else {
        logf("[net] HOST: listening on UDP %d, waiting for peer (protocol v%d)...\n", boundPort, kNetVersion);
    }
    return true;
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
    strncpy(p->weapon, equippedWeapon(p1), sizeof p->weapon - 1);
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
static void applyRemote(void* gameLocal, float dt) {
    if (!g_remoteValid || !playerAtSlot(gameLocal, 0)) return;  // need a live level
    if (!g_player2) {
        logf("[net] spawning remote player\n");
        spawnSecondPlayer(gameLocal, SpawnSebastian);
        g_angCorrPitch = g_angCorrYaw = 0.f;
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
        c.buttons = r.buttons & ~kSelectButtons;
        c.buttons2 = 0;  // NEXT/PREV_QUICK_ITEM
        c.forwardmove = r.fwd; c.rightmove = r.right; c.upmove = r.up;
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
    c.clientGameFrame = ++g_p2Frame;
    fn<PlayerSetUsercmd_t>(rva_PlayerSetUsercmd)(g_player2, &c, &c);
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
    // (closed-loop angle correction removed: it ran away because the view updates a frame later and pitch
    //  is clamped. The angle diagnostics in netRun's log show how cmd, view and delta relate.)

    // 2) Position: the interpolated remote position is the target; his own movement gets him there, we correct drift.
    Vec3 target{r.px, r.py, r.pz};
    if (g_netLoopback) target.x += kLoopOffset;
    Vec3 cur = originOf(g_player2);
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
enum { kEvtDamage = 1, kEvtHasInfo = 1, kEvtHasDir = 2, kEvtHasLoc = 4 };
static const uintptr_t kDamageDeclList = 0x9A9C170;
static bool g_applyingNetDamage;  // replaying a remote hit: don't send it back
static uint32_t g_evtSeq;
static uint32_t g_evtSeen[64];
static int g_evtSeenPos;
static NetEvt g_evtQueue[32];
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
        sendto(g_netSock, (const char*)&e, sizeof e, 0, (sockaddr*)&g_peerAddr, sizeof g_peerAddr);
}

// Called by the asm thunk with the original arguments; returns the game's Damage to jump to.
extern "C" uintptr_t ewPreDamage(void* self, void* inflictor, void* attacker, void* decl, void** stackArgs) {
    uintptr_t orig = actorDamageOrig(self);
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
    .att_syntax prefix
)");

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
    g_evtSeen[g_evtSeenPos++ % 64] = e.id;
    if (g_evtQueueCount < 32) g_evtQueue[g_evtQueueCount++] = e;
}
static void applyEvents(void* gameLocal) {
    for (int i = 0; i < g_evtQueueCount; ++i) {
        NetEvt& e = g_evtQueue[i];
        e.target[63] = e.decl[63] = 0;
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
                    sendto(g_netSock, (const char*)&p, sizeof p, 0, (sockaddr*)&g_peerAddr, sizeof g_peerAddr);
                }
            }
        }
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
        for (int i = 0; i < 64; ++i) {  // drain
            alignas(8) char buf[512];
            sockaddr_in from = {}; int fl = sizeof from;
            int n = recvfrom(g_netSock, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
            if (n <= 0) break;
            if (n == (int)sizeof(NetEvt)) {  // damage event
                NetEvt ev;
                memcpy(&ev, buf, sizeof ev);
                if (ev.magic == kNetMagic && ev.version == kNetVersion && ev.type == kEvtDamage) queueEvent(ev);
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
            if (in.magic == kNetMagic && in.version == kNetVersion &&
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
    applyRemote(gameLocal, dt);
    applyEvents(gameLocal);
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
            logf("[net] weapons %s: slot0=\"%s\" slot1=\"%s\" slot2=\"%s\" slot3=\"%s\" slot4=\"%s\" slot5=\"%s\"\n",
                 i ? "p2" : "p1", weaponSlotName(who[i], 0), weaponSlotName(who[i], 1), weaponSlotName(who[i], 2),
                 weaponSlotName(who[i], 3), weaponSlotName(who[i], 4), weaponSlotName(who[i], 5));
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

static int64_t hkRunFrame(void* self, int64_t a2, int64_t a3, char a4) {
    int64_t r = g_origRunFrame(self, a2, a3, a4);
    if (g_unhook) return r;
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
            if (p && p != localPlayer(self) && isA(p, rva_idPlayer_vtable)) {
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
    if (g_player2 && f9Ctrl) {
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
            void* r = vfn<AltBody_t>(g_player2, slot_player_AltBody)(g_player2, g_josephBody, "pl/pl9000.md6");
            logf("[ewcoop] player 2 alt body pl9000 %s (returned %p, main model %p)\n",
                 g_josephBody ? "ON" : "OFF", r, inst);
        }
    }
    if (g_player2 && !isA(g_player2, rva_idPlayer_vtable)) {
        logf("[ewcoop] player 2 entity is gone\n");
        if (g_playAsP2) { setPlayerSlot(self, g_sebastian); g_playAsP2 = false; }
        g_player2 = nullptr;
        g_josephBody = false;
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
        if (held(VK_SHIFT)) {  // Ctrl+Shift+F12: kick off the multiplayer party lobby
            createPartyLobby();
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
        netStart(true, nullptr);
    }
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('O')) {
        char ip[64] = {};
        if (!GetEnvironmentVariableA("EWCOOP_HOST", ip, sizeof ip) || !ip[0]) {
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s\\ewhost.txt", g_dir);
            if (FILE* f = fopen(path, "rb")) {
                if (!fgets(ip, sizeof ip, f)) ip[0] = 0;
                fclose(f);
                for (char* q = ip; *q; ++q)
                    if (*q == '\r' || *q == '\n' || *q == ' ' || *q == '\t') { *q = 0; break; }
                if (ip[0] == '#' || ip[0] == ';') ip[0] = 0;
            }
        }
        if (!ip[0]) strcpy(ip, "127.0.0.1");
        netStart(false, ip);
    }
    if (held(VK_CONTROL) && held(VK_SHIFT) && pressed('U')) netStop();
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
                g_watch[0] = reinterpret_cast<uintptr_t>(g_player1) + 2728 + 16;
                g_watch[1] = g_player2 ? reinterpret_cast<uintptr_t>(g_player2) + 2728 + 16 : 0;
                g_watchReads = false;
                g_watchExec = false;
                g_watchSeenCount = 0;
                g_watchHits = 0;
                logf("[ewcoop] weapon-slot watch ON: DR0 = player 1 (%p), DR1 = player 2 (%p)\n", g_player1, g_player2);
            } else {
                logf("[ewcoop] weapon-slot watch OFF, hits %ld\n", g_watchHits);
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
