#pragma once
// Engine addresses for The Evil Within, Steam build 2708258
// (EvilWithin.exe SHA-256 743df7a1...e67437). All values are RVAs from the
// image base; found in IDA on a memory dump of the running game.
#include <cstdint>

namespace game {

// idGameLocal vtable (.rdata). Slots are 8 bytes each.
constexpr uintptr_t rva_idGameLocal_vtable = 0x15D0268;
constexpr int slot_RunFrame        = 13;   // 0x7A3EB0  (this, a2, a3, char a4)
constexpr int slot_CreateEntityDef = 119;  // 0x79B5A0  CreateEntityFromDef(decl, spawnParams)
constexpr int slot_SpawnEntityDef  = 120;  // 0x7A8400  (this, decl, int idx = -1) -> idEntity*
constexpr uintptr_t rva_RunFrame       = 0x7A3EB0;
constexpr uintptr_t rva_SpawnEntityDef = 0x7A8400;

// Global "current game" pointer: set/restored around every game call (sub_7AFBF0/7AFC20).
constexpr uintptr_t rva_gameLocal = 0x9AB3AF0;

// Resource list of idDeclEntityDef and its lookup, as used by idGameLocal::SetupPlayer (0x8EDD60):
//   decl = FindEntityDef(&entityDefList, name, 0, 0, 0, 0, 0, 0, 0)
constexpr uintptr_t rva_entityDefList = 0x9A627A0;
constexpr uintptr_t rva_FindEntityDef = 0x39F780;

// idPlayer::Spawn (0x8906E0) refuses entityNumber >= 1 (MAX_PLAYERS = 1):
//   0x8906F9 cmp edx, 1 / 0x8906FC jge 0x8911A8 -> Error("entityNum > MAX_PLAYERS for player...")
constexpr uintptr_t rva_PlayerSpawn_maxPlayersJge = 0x8906FC;
constexpr unsigned char kMaxPlayersJge[6] = {0x0F, 0x8D, 0xA6, 0x0A, 0x00, 0x00};
// Tail of idPlayer::Spawn: 0x9186A0(*(gameLocalSub+0x82040), player) sets
// records[player->entityNumber].flag (1320-byte records sized for 1 player). Only caller is Spawn.
constexpr uintptr_t rva_MarkPlayerRecord = 0x9186A0;
constexpr unsigned char kMarkPlayerRecord[4] = {0x48, 0x63, 0x42, 0x10};  // movsxd rax, [rdx+10h]

// idEntity::UpdateVisuals (0x876450): vtable +344 copies the physics transform to the render
// entity (0x876290), then UpdateSound. Partner Teleport calls it right after SetOrigin.
constexpr uintptr_t rva_UpdateVisuals = 0x876450;

// Players are placed by a body controller, not the entity physics: idPlayer's visual transform
// (vtbl +328 = 0x7BC060) reads *(ent+0x2A8) -> vfunc +0x60 -> +19392, an idPhysics-like object
// (+0x90 GetOrigin, +0x98 GetAxis; +0x60 SetOrigin assumed from the idPhysics layout).
constexpr uintptr_t off_entity_anim = 0x2A8;
constexpr int slot_anim_GetController = 0x60 / 8;
constexpr uintptr_t off_controller_body = 19392;

// ---- Player input ----
// RunFrame's 2nd argument is the idUserCmdMgr: usercmd_t cmdBuffer[128] at +0, a second buffer at
// +0x2000, writeFrame at +0x4000, readFrame at +0x4004. RunFrame takes cmd (writeFrame-1)%128 and
// gives it to player 0 only via 0x88F440(player, &cmd, &cmd2), which stores it in the controller.
constexpr uintptr_t rva_PlayerSetUsercmd = 0x88F440;

// usercmd_t, from the reflection data (size 0x40)
struct Usercmd {
    int32_t buttons;        // 0x00 BUTTON_* bits
    int16_t buttons2;       // 0x04
    int8_t forwardmove;     // 0x06
    int8_t rightmove;       // 0x07
    int8_t upmove;          // 0x08
    bool inhibited;         // 0x09
    bool cameraInhibited;   // 0x0A
    uint8_t pad0b;
    int16_t angles[3];      // 0x0C
    uint16_t pad12;
    int32_t clientGameFrame;// 0x14
    float pos[3];           // 0x18
    int16_t pitch, yaw, roll; // 0x24
    uint8_t predictionStateBits; // 0x2A
    uint8_t vehicleThrottle;     // 0x2B
    int32_t serverGameTime; // 0x2C
    uint16_t fireCount;     // 0x30
    int16_t fireAngles[2];  // 0x32
    int16_t firePos[3];     // 0x36
    uint8_t speed;          // 0x3C
    uint8_t pad3d[3];
};
static_assert(sizeof(Usercmd) == 0x40, "usercmd_t layout");
enum : int32_t {  // usercmdButton_t
    BUTTON_ATTACK1 = 1, BUTTON_ATTACK2 = 2, BUTTON_USE = 4, BUTTON_CANCEL = 8, BUTTON_THROW = 16,
    BUTTON_ZOOM = 32, BUTTON_SPRINT = 64, BUTTON_LANTERN = 128, BUTTON_KNIFE = 256, BUTTON_SNEAK = 512,
    BUTTON_WALK = 1024, BUTTON_CHANGEWEAPON = 2048, BUTTON_WEAP_RELOAD = 4096,
};
using PlayerSetUsercmd_t = void (*)(void* player, const Usercmd* cmd, const Usercmd* cmd2);

// idPlayer::Damage = idPlayer vtable slot 80 (0x7ED120): (this, inflictor, attacker, damageDecl, a5,
// float* a6, location) -> damage dealt. Calls attacker->vtbl[+616](this, inflictor, decl, &amount).
constexpr int slot_player_Damage = 80;
constexpr uintptr_t rva_PlayerDamage = 0x7ED120;
using PlayerDamage_t = double (*)(void* self, void* inflictor, void* attacker, void* decl, void* a5, float* a6, void* loc);

// ---- "Player 0" shortcuts ----
// idGameLocal::GetPlayer(idx) = 0x79D070: idx != 0 -> null; else CastToPlayer(*(*(gl+0x68)+0x28)).
// Lots of player logic asks GetPlayer(0) instead of using `this`, e.g. the sneak-pressed check
// 0x82F090. While player 2 thinks, we make GetPlayer(0) return player 2.
constexpr uintptr_t rva_GetPlayer = 0x79D070;
constexpr unsigned char kGetPlayerHead[12] = {0x48, 0x8B, 0xC1, 0x85, 0xD2, 0x75, 0x11, 0x48, 0x8B, 0x40, 0x68, 0x48};
constexpr uintptr_t rva_CastToPlayer = 0xDAC4C0;
// idPlayer::Think = idPlayer vtable slot 19 (0x7F6B30); the only slot that reaches the actor update.
constexpr int slot_player_Think = 19;
constexpr uintptr_t rva_PlayerThink = 0x7F6B30;
using Think_t = int64_t (*)(void* self);

// ---- Sneak ----
// Sneak state machine 0x7F8910 sets player+4350 (IsSneaking 0x7F38C0). It needs CanSneak() =
// idPlayer vtbl +2168 (0x7B9590): +4791==0, +4780==0, anim node *(u16)(+6872) active, node
// *(u16)(+6874) inactive, vfunc +72 < 35, +3976!=1, +4348==0, +3984==5.
constexpr uintptr_t off_player_sneaking = 4350;
constexpr int slot_player_CanSneak = 2168 / 8;
constexpr int slot_player_vfunc72 = 72 / 8;
constexpr uintptr_t rva_IsNodeActive = 0x908CD0;  // (morpheme net = player+1280, u16 node) -> bool
using IsNodeActive_t = bool (*)(void* net, uint16_t node);

// idPlayer::IsLocalPlayer = vtbl slot 228 (+1824, 0x88FF40): asks the session whether the entity
// number is a local client. idPlayer slot 14 (0x891E10) only runs the controller's input step
// (idPresentablePlayer vtbl +0x218 = 0x862650: weapons, aiming, UpdateSneak...) when it's true.
constexpr int slot_player_IsLocal = 228;
constexpr uintptr_t rva_PlayerIsLocal = 0x88FF40;
using IsLocal_t = bool (*)(void* self, void* a2);

// ---- Body parts / costumes ----
// idPlayer vtbl slot 162 (+0x510) = 0x7BE610(player, bool on, const char* md6): the game's alternate body
// switch. On: main model head variant -> "headless" ("bodyless" if a pl/pl0020.md6 part is attached), and
// the given .md6 is added through the attachment collection (player vtbl +896; add 0x915810, find 0x916C30).
constexpr int slot_player_AltBody = 162;
using AltBody_t = void* (*)(void* player, bool on, const char* md6);
// Main model instance: 0x85E5E0(*(player+0x2A8)->vfunc(+48)). Variant select 0x199790(inst, head, gear)
// picks entries from the .md6 decl's "heads" and "gear" lists.
constexpr uintptr_t rva_ModelInstance = 0x85E5E0;
constexpr uintptr_t rva_SetModelVariant = 0x199790;
using ModelInstance_t = void* (*)(void* x);
using SetModelVariant_t = void (*)(void* inst, const char* head, const char* gear);

// The meshkit (character body parts) lives in the .md6: the model instance holds the Md6Decl pointer at
// +2552 (decimal; 0x9F8) with a "heads" list and a "gear" list; each entry (56 bytes) names a set of mesh
// components (component index array at entry+40, count u16 at entry+48; name char* at entry+8). The model
// instance holds one visibility byte per component at +2560 (decimal) (count = Md6Decl.skeleton(+112) +216).
// 0x198160(inst) zeroes every component flag and 0x199A20(inst, out, kit, name) sets the named entry's
// flags back to 1. Variant names seen in use: "normal", "headless", "bodyless". Blanking the base model
// this way leaves the attached models (a separate idTreeAnimator) untouched.
constexpr uintptr_t rva_ClearModelMeshes = 0x198160;
constexpr uintptr_t off_model_Md6Decl = 2552;       // *(inst+2552), decimal
constexpr uintptr_t off_model_meshVisible = 2560;   // *(inst+2560), decimal
using ClearModelMeshes_t = void (*)(void* inst);

// Attachment collection: idActor vfunc +896 (index 112) returns it. add 0x915810(collection, outId, md6,
// tagId, tagName, override, a7); find 0x917130(coll, outId) -> entry*; activate 0x917E20(coll, &id);
// detach 0x917460(coll, &id, a3). AddAttachment rebinds the child's skeleton to the parent's by bone name
// (0x198530), so a same-skeleton character mesh rides the parent's animations.
constexpr int slot_actor_Attachments = 896 / 8;
constexpr uintptr_t rva_AddAttachment = 0x915810;
constexpr uintptr_t rva_FindAttachment = 0x917130;
constexpr uintptr_t rva_ActivateAttachment = 0x917E20;
constexpr uintptr_t rva_DetachAttachment = 0x917460;

// Console command system (idCmdSystemLocal). The object's +8 is the command-list head, which is the same
// storage as the AddCommand global qword_9B13EC0 -> the object is at 0x9B13EB8. (0x1D6A5F0 is the *cvar*
// system, used as the executor's fallback.) 0xCC50E0(cmdSystem, text, 1) tokenizes + dispatches one command
// (-> 0xCC5110 walks the list and calls the handler). Retail stubbed many handlers, but startServer/connect
// are real, so this is how we can start/join a network game and set cvars.
constexpr uintptr_t rva_cmdSystem = 0x9B13EB8;
constexpr uintptr_t rva_cmdList = 0x9B13EC0;  // *(this) = head command node {next, name, fn, desc, ...}
constexpr uintptr_t rva_cmdExecuteString = 0xCC50E0;
using ExecuteString_t = void (*)(void* cmdSystem, const char* text, char a3);

// Multiplayer session. The idSessionLocal object used by startServer is the global at rva_session (its first
// qword is the vtable). 0x333C60 = idSessionLocal::CreatePartyLobby (a vtable method); 0x3299A0 =
// idSession::AddBot(session, out, name, a4). The lobby flow is STATE_PARTY_LOBBY_HOST -> ... -> game, driven
// by the session state machine (no console entry), which is why startServer alone reports "Not host of TYPE_PARTY".
constexpr uintptr_t rva_session = 0x9A532C0;
constexpr uintptr_t rva_CreatePartyLobby = 0x333C60;
constexpr uintptr_t rva_AddBot = 0x3299A0;
// startServer drives: build params (0x431A0 = idUUID4-ish ctor, writes flags to +164) then CreateMatch
// (session vtbl slot 12 = 0x333AF0). CreateMatch's "create a new party" branch needs (params+164)&0x40 set
// (the stock startServer passes 2, so it assumes a party already exists -> "Not host of TYPE_PARTY"). With the
// bit set it calls CreatePartyLobby(session, params). 0x43480 destroys the params object.
constexpr uintptr_t rva_InitLobbyParams = 0x431A0;
constexpr uintptr_t rva_CreateMatch = 0x333AF0;
constexpr uintptr_t rva_DestroyLobbyParams = 0x43480;
constexpr uintptr_t rva_cvarLobbyStr1 = 0x9A53228;  // char* used as a lobby param string
constexpr uintptr_t rva_cvarLobbyStr2 = 0x9A53188;  // char* used as a lobby param string
// net_port idCVar object (registered default "27015"). idCVar layout: name@+48, valueString@+56.
// The direct (non-Steam) backend binds this UDP port; the friend's `connect <ip>` uses it.
constexpr uintptr_t rva_cvarNetPort = 0x9A4B870;
constexpr uintptr_t off_cvar_valueString = 56;
constexpr uintptr_t off_cvar_valueInt = 40;  // idCVar int value (the flag/bool/integer cvars read this)
// net_usePlatformBackend idCVar (0x212D750); its int value at +40 is dword_212D778, which the direct
// backend selection reads. cmdSystem is often null so runConsole can't set it -- write the int directly.
constexpr uintptr_t rva_cvarNetUsePlatformBackend = 0x212D750;
// idRenderWorldLocal::Render(this, x, y, w, h, ...) at 0x286FD0 -- has the embedded idRenderView. Hook it to
// capture the live render-world object, then read the camera each frame for world->screen projection.
// Offsets on the render-world object: viewMatrix @+0x4B0, projection @+0x284, MVP (proj*view) @+0x62C,
// vieworg @+0x57C, fovX @+0x26C, fovY @+0x270, render viewport w@+0x7B0 h@+0x7B2 (words).
constexpr uintptr_t rva_RenderWorld_Render = 0x286FD0;
constexpr uintptr_t off_render_MVP = 0x62C;         // 4x4 (16 floats), world->clip
constexpr uintptr_t off_render_viewMatrix = 0x4B0;  // 4x4 (16 floats), world->view
constexpr uintptr_t off_render_viewportW = 0x7B0;   // int16
constexpr uintptr_t off_render_viewportH = 0x7B2;   // int16
constexpr uintptr_t off_render_fovX = 0x26C;        // float (radians)
constexpr uintptr_t off_render_fovY = 0x270;        // float// net_ip idCVar (default "localhost"). "localhost" makes UDP_OpenSocket (0xCD75E0) bind INADDR_ANY (all
// interfaces), so a virtual-LAN adapter (Radmin/ZeroTier) is reachable. qword at rva_netIpString holds the
// current bind address string.
constexpr uintptr_t rva_netIpString = 0x9B14468;
// Engine print chain: sub_CC1570(fmt,...) -> sub_CC1680(level, fmt, va) which walks a listener list
// (head qword_9AFB410) calling listener->vtbl[1](listener, str). sub_CC15D0(listener) registers one
// (links at head if listener+8 == 0). Listener layout: vtbl@0, next@8, pass1@16, flag@17, minLevel@20.
// net_verbose (idCVar 0x9A4BC00, int value at +40 = 0x9A4BC28) gates the NET trace lines.
constexpr uintptr_t rva_PrintListenerAdd = 0xCC15D0;
constexpr uintptr_t rva_cvarNetVerbose = 0x9A4BC00;
// TEW's idGameLocal::SetupPlayer(index) (0x8EDD60) CLAMPS its index to 0 (`if (a2>=1) a2=0`, bytes
// 33 F6 83 FD 01 0F 4D EE at 0x8EDD88), so the engine can only create player slot 0. LoadMap
// (sub_8EC940) loops `for (j=0; j<*(gameLocal+0xAB38); ++j) SetupPlayer(j)` and writes player j into the
// entity manager slot array *(entMgr+40+j*8). NOP the clamp + set numPlayers>1 and the engine creates a
// real second player (entityNumber 1) at map load -- the base for engine-managed co-op.
constexpr uintptr_t rva_SetupPlayer = 0x8EDD60;  // idGameLocal::SetupPlayer(this, index): spawns player into slot `index`
constexpr uintptr_t rva_SetupPlayerClamp = 0x8EDD88;
constexpr uintptr_t off_game_numPlayers = 0xAB38;
constexpr uintptr_t rva_lobbyParamConst = 0x21416EC;  // int read for the params
// 0x32AE50 = AddLocalUsersToLobby(lobby, all): CreateLobbyUserFromLocalUser (0x329F80) + insert (0x329D20).
// CreatePartyLobby does NOT add the local user, so the lobby has no state==2 user and SendMembersToLobby sends
// nothing -> the game lobby never becomes ready. We call this after creating the party lobby.
constexpr uintptr_t rva_AddLocalUsersToLobby = 0x32AE50;
constexpr uintptr_t off_session_partyLobby = 88;  // party lobby object embedded in the session
// idSessionLocal::StartMatch (0x339190) is the removed front-end's "Start Game": it requires state==4
// (STATE_GAME_LOBBY_HOST) and calls StartLoading (0x338F60) -> STATE_LOADING -> the map loads. No peers needed.
constexpr uintptr_t rva_StartMatch = 0x339190;
constexpr uintptr_t rva_StartLoading = 0x338F60;
// idGameLocal::LoadMap (0x79F2C0, idGameLocal vtbl slot 212) takes no args: it reads the map name from an
// idStr member on gameLocal where the string ptr is at +583768 and len at +583760.
constexpr uintptr_t rva_LoadMap = 0x79F2C0;
constexpr uintptr_t off_game_mapIdStr = 583760;  // idStr {int len; char* data; ...}; data at +8

// AI target acquisition. Every enemy's "detect target select" (0x8DDB80 / 0xA87A90 / 0xA9E6D0, per class)
// asks 0x8AB6D0 for the primary target and stores it in the enemy's target field. 0x8AB6D0 is literally
// GetPlayer(0):
//   8AB6D0: mov rcx, cs:qword_9AB3AF0 ; xor edx, edx ; jmp GetPlayer
// so only player 1 is ever returned -> enemies ignore a spawned player 2. The same select routines call
// 0x8AB4F0(this = the enemy) for the secondary/partner target, so its `this` identifies the enemy
// currently thinking. We replace 0x8AB6D0 and return whichever player is nearest that enemy.
constexpr uintptr_t rva_AI_PlayerGetter = 0x8AB6D0;   // () -> idPlayer*   (GetPlayer(0))
constexpr uintptr_t rva_AI_TargetContext = 0x8AB4F0;  // (enemy) -> idEntity*
using AiPlayerGetter_t = void* (*)();
using AiTargetContext_t = void* (*)(void* self);

// Single-instance check at startup (0x346410): hObject = CreateMutexA(null, false, "The Evil Within");
// ERROR_ALREADY_EXISTS -> "There is another instance ... running". The handle is stored here; its only other
// user (restart, 0x346CB0) checks for null before CloseHandle, so closing it and zeroing it is safe.
constexpr uintptr_t rva_instanceMutex = 0x9A50640;

// idLib::Error (0xCC1200) copies its message here before throwing idException (drop to menu).
constexpr uintptr_t rva_lastErrorText = 0x9AF7A40;

// Class vtables (RTTI), used to check a pointer really is what we think before touching it.
constexpr uintptr_t rva_idPlayer_vtable    = 0x15D4B78;
constexpr uintptr_t rva_idPartner01_vtable = 0x16B5D28;  // Joseph

// idGameLocal layout (from SetupPlayer 0x8EDD60): *(gameLocal+0x68) + 0x28 + 8*n = player n entity.
constexpr uintptr_t off_gameLocal_entMgr = 0x68;
constexpr uintptr_t off_entMgr_players   = 0x28;

// idEntity layout
// idPhysics vtbl: +0x60 SetOrigin, +0x68 SetAxis (seen in Teleport 0x9C15A0); +0x90 GetOrigin(id),
// +0x98 GetAxis(id) (0xA2A010 feeds +0x98's result to the matrix-to-angles helper 0xCC34B0).
constexpr uintptr_t off_entity_physics = 0x2B8;
constexpr int slot_phys_GetOrigin = 0x90 / 8;
constexpr int slot_phys_GetAxis   = 0x98 / 8;

// idPartnerBase::Teleport(origin, angles, bool toIdle, bool a5): vtable +0x718 (0x9C15A0 for Joseph).
// Same call idTarget_TeleportPartner (0xA2A010) uses.
constexpr int slot_partner_Teleport = 0x718 / 8;

// Partner action handler, idPartner01 vtable slot 270 (0xC3E3A0): runs AI-chosen actions
// (move to goal, attack, open door...). Returns 0 = still running, 1 = done, -1 = failed.
constexpr int slot_partner_HandleAction = 270;
constexpr uintptr_t rva_Partner01_HandleAction = 0xC3E3A0;

// Morpheme animation network embedded in the partner at +0x500 (set up in 0xC40440).
constexpr uintptr_t off_partner_morpheme   = 0x500;
constexpr uintptr_t off_partner_MoveSpeed  = 4704;  // u16 control param id; 0.0 = walk, 1.0 = run
constexpr uintptr_t off_partner_reqMove    = 4776;  // i32 request id "Move"
constexpr uintptr_t off_partner_reqStop    = 4780;  // i32 request id "Stop"
constexpr uintptr_t off_partner_facing     = 1720;  // object* used for body facing (0x851EB0)
constexpr uintptr_t rva_SetFloatParam = 0x9094A0;   // (net, u16 id, float)
constexpr uintptr_t rva_SendRequest   = 0x907BA0;   // (net, i32 id, bool)
constexpr uintptr_t rva_AnglesToMat3  = 0xCF25E0;   // (const Angles*, out 48 bytes) -> out
constexpr uintptr_t rva_AnglesToQuat  = 0xCF2750;   // (const Angles*, out 16 bytes) -> out
constexpr uintptr_t rva_SetFacing     = 0x851EB0;   // (facing obj, const quat*)
constexpr int slot_phys_SetAxis = 0x68 / 8;         // (phys, const mat*, int id = -1)
constexpr int slot_phys_SetOrigin = 0x60 / 8;       // (phys, const Vec3*, int id = -1)
constexpr uintptr_t rva_SetFacingOrigin = 0x852020; // (facing obj, const Vec3*), Teleport's 2nd call

// Partner AI actions. ExecuteAction = idPartner01 vtable slot 267 (0xC3CBF0), called by the AI as
// ExecuteAction(partner, &action, 2, 0); it copies the action into the queue.
constexpr int slot_partner_ExecuteAction = 267;
constexpr uintptr_t rva_PartnerActionData_vtable = 0x162BE40;  // idTarget_PartnerActions::ActionData
constexpr uintptr_t rva_idStr_ctor = 0x1000;                     // (idStr*, const char*, 0)
constexpr int action_WalkTo = 34;  // 0x22: Move_Speed 0, walk to position (0x9B9B40)
constexpr int action_RunTo  = 35;  // 0x23: Move_Speed 1, run to position

struct Vec3 { float x, y, z; };
struct Angles { float pitch, yaw, roll; };

// idActionDataBase + idTarget_PartnerActions::ActionData, from the game's reflection data (size 0x68).
struct ActionData {
    void* vtbl;             // 0x00
    float waitTime;         // 0x08
    Vec3 position;          // 0x0C
    Angles deltaAngles;     // 0x18
    int targetSpawnId;      // 0x24  idEntityPtr: 0xFFFF / -1 = none
    int targetSerial;       // 0x28
    int pad2c;              // 0x2C
    unsigned char editString[0x28];  // 0x30 idStr
    float editValue;        // 0x58
    int pad5c;              // 0x5C
    int actionType;         // 0x60
    int pad64;              // 0x64
};
static_assert(sizeof(ActionData) == 0x68, "ActionData layout");
struct Mat3 { Vec3 rows[3]; };

using RunFrame_t       = int64_t (*)(void* self, int64_t a2, int64_t a3, char a4);
using GetVec_t         = const Vec3* (*)(void* phys, int id);
using GetAxis_t        = const Mat3* (*)(void* phys, int id);
using Teleport_t       = void (*)(void* partner, const Vec3* origin, const Angles* angles, char toIdle, char a5);
using HandleAction_t   = int64_t (*)(void* partner, void* action, int64_t a3, uint32_t a4, char a5, char a6);
using SetFloatParam_t  = int64_t (*)(void* net, uint16_t id, float v);
using SendRequest_t    = bool (*)(void* net, int id, char a3);
using AnglesTo_t       = void* (*)(const Angles* a, void* out);
using SetFacing_t      = void (*)(void* facing, const void* quat);
using SetAxis_t        = void (*)(void* phys, const void* mat, int id);
using SetOrigin_t      = void (*)(void* phys, const Vec3* o, int id);
using ExecuteAction_t  = int64_t (*)(void* partner, ActionData* action, int priority, int a4);
using IdStrCtor_t      = void (*)(void* str, const char* s, int64_t a3);
using SetFacingOrigin_t = void (*)(void* facing, const Vec3* o);
using SpawnEntityDef_t = void* (*)(void* self, void* decl, int idx);
using FindEntityDef_t  = void* (*)(void* list, const char* name, uint8_t, uint8_t, uint8_t,
                                   int, char, char, char);

}  // namespace game
