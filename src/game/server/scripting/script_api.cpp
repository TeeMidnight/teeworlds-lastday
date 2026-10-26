/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/scripting/script_runtime.h>

#include <base/system.h>

#include <engine/server.h>

#include <game/localization.h>

#include <game/server/entities/character.h>
#include <game/server/entities/drill.h>
#include <game/server/entities/laser.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/item.h>
#include <game/server/player.h>
#include <game/server/scripting/buff_system.h>

#include <pybind11/embed.h>

namespace py = pybind11;

namespace
{
CGameContext *Game()
{
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	return pRuntime ? pRuntime->GameServer() : nullptr;
}

CPlayer *Player(int ClientID)
{
	CGameContext *pGame = Game();
	if(!pGame || ClientID < 0 || ClientID >= MAX_CLIENTS)
		return nullptr;
	return pGame->m_apPlayers[ClientID];
}

CCharacter *Character(int ClientID)
{
	CGameContext *pGame = Game();
	return pGame ? pGame->GetPlayerChar(ClientID) : nullptr;
}

// World of the callback that is currently running (or the main world).
CGameWorld *ActiveWorld()
{
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	return pRuntime ? pRuntime->ActiveWorld() : nullptr;
}

bool ValidPos(float x, float y)
{
	return absolute(x) < 1.0e7f && absolute(y) < 1.0e7f;
}

py::tuple Vec2Tuple(float x, float y)
{
	py::tuple Result(2);
	Result[0] = py::cast(x);
	Result[1] = py::cast(y);
	return Result;
}

int CreateProjectileScript(int Owner, float x, float y, float vx, float vy, int Span, int Damage, bool Explosive, int SoundImpact, int Weapon)
{
	CCharacter *pChr = Character(Owner);
	if(!pChr || !ValidPos(x, y))
		return -1;
	CProjectile *pProjectile = new CProjectile(pChr->GameWorld(), Weapon, Owner, vec2(x, y), vec2(vx, vy),
		Span, Damage, Explosive, 0.0f, SoundImpact, Weapon);
	return pProjectile->GetID();
}

int CreateLaserScript(int Owner, float x, float y, float dx, float dy, float Reach, int Damage)
{
	CCharacter *pChr = Character(Owner);
	if(!pChr || !ValidPos(x, y))
		return -1;
	const vec2 Direction(dx, dy);
	if(length(Direction) < 0.0001f)
		return -1;
	CLaser *pLaser = new CLaser(pChr->GameWorld(), vec2(x, y), normalize(Direction), Reach, Owner, Damage);
	return pLaser->GetID();
}
} // namespace

PYBIND11_EMBEDDED_MODULE(lastday, m)
{
	m.def("log", [](const std::string &Message) {
		dbg_msg("script", "%s", Message.c_str());
	});

	m.def("chat", [](int ClientID, const std::string &Message) {
		CGameContext *pGame = Game();
		if(!pGame || ClientID < 0 || ClientID >= MAX_CLIENTS)
			return false;
		pGame->SendChat(-1, CHAT_WHISPER, ClientID, Message.c_str());
		return true;
	});

	// Same as chat(), but the message is looked up in the recipient's language
	// using Context. Scripts should use this for player-visible text.
	m.def("chat_localized", [](int ClientID, const std::string &Message, const std::string &Context) {
		CGameContext *pGame = Game();
		if(!pGame || ClientID < 0 || ClientID >= MAX_CLIENTS)
			return false;
		CLanguageScope LanguageScope(pGame->LanguageOf(ClientID));
		pGame->SendChat(-1, CHAT_WHISPER, ClientID, Localize(Message.c_str(), Context.c_str()));
		return true;
	});

	m.def("player_heal", [](int ClientID, int Amount) {
		CCharacter *pChr = Character(ClientID);
		if(!pChr || Amount <= 0)
			return 0;
		const int Before = pChr->Health();
		pChr->IncreaseHealth(Amount);
		return pChr->Health() - Before;
	});

	m.def("player_armor", [](int ClientID, int Amount) {
		CCharacter *pChr = Character(ClientID);
		if(!pChr || Amount <= 0)
			return 0;
		const int Before = pChr->Armor();
		pChr->IncreaseArmor(Amount);
		return pChr->Armor() - Before;
	});

	m.def("player_health", [](int ClientID) {
		CCharacter *pChr = Character(ClientID);
		return pChr ? pChr->Health() : 0;
	});

	m.def("player_armor_value", [](int ClientID) {
		CCharacter *pChr = Character(ClientID);
		return pChr ? pChr->Armor() : 0;
	});

	m.def("player_forget", [](int ClientID, int Delta) {
		CPlayer *pPlayer = Player(ClientID);
		if(!pPlayer)
			return 0;
		const int NewForgetting = clamp(pPlayer->m_Status.m_Forgetting + Delta, 0, 100);
		const int Applied = NewForgetting - pPlayer->m_Status.m_Forgetting;
		pPlayer->m_Status.m_Forgetting = NewForgetting;
		return Applied;
	});

	m.def("player_damage", [](int ClientID, int Amount, int FromClientID, int Weapon) {
		CCharacter *pChr = Character(ClientID);
		if(!pChr || Amount <= 0)
			return false;
		return pChr->TakeDamage(vec2(0.0f, -1.0f), pChr->GetPos(), Amount, FromClientID, Weapon);
	});

	m.def("player_pos", [](int ClientID) {
		CCharacter *pChr = Character(ClientID);
		if(!pChr)
			return Vec2Tuple(0.0f, 0.0f);
		return Vec2Tuple(pChr->GetPos().x, pChr->GetPos().y);
	});

	m.def("player_alive", [](int ClientID) {
		CCharacter *pChr = Character(ClientID);
		return pChr && pChr->IsAlive();
	});

	m.def("player_max_health", [](int ClientID) {
		CCharacter *pChr = Character(ClientID);
		return pChr ? pChr->MaxHealth() : 0;
	});

	m.def("player_range_scale", [](int ClientID) {
		CCharacter *pChr = Character(ClientID);
		return pChr ? pChr->RangeScale() : 1.0f;
	});

	m.def("player_speed_scale", [](int ClientID) {
		CCharacter *pChr = Character(ClientID);
		return pChr ? pChr->SpeedScale() : 1.0f;
	});

	m.def("player_team", [](int ClientID) {
		CPlayer *pPlayer = Player(ClientID);
		return pPlayer ? pPlayer->GetTeam() : TEAM_SPECTATORS;
	});

	m.def("player_name", [](int ClientID) -> std::string {
		CGameContext *pGame = Game();
		if(!pGame || ClientID < 0 || ClientID >= MAX_CLIENTS || !pGame->m_apPlayers[ClientID])
			return "";
		const char *pName = pGame->Server()->ClientName(ClientID);
		return pName ? pName : "";
	});

	m.def("player_velocity", [](int ClientID) {
		CCharacter *pChr = Character(ClientID);
		if(!pChr)
			return Vec2Tuple(0.0f, 0.0f);
		const vec2 Vel = pChr->Velocity();
		return Vec2Tuple(Vel.x, Vel.y);
	});

	m.def("player_grounded", [](int ClientID) {
		CCharacter *pChr = Character(ClientID);
		return pChr && pChr->IsGrounded();
	});

	m.def("player_aim_direction", [](int ClientID) {
		CCharacter *pChr = Character(ClientID);
		if(!pChr)
			return Vec2Tuple(0.0f, 0.0f);
		const vec2 Dir = pChr->AimDirection();
		return Vec2Tuple(Dir.x, Dir.y);
	});

	m.def("player_teleport", [](int ClientID, float x, float y) {
		CCharacter *pChr = Character(ClientID);
		if(!pChr || !ValidPos(x, y))
			return false;
		pChr->MoveTo(pChr->GameWorld(), vec2(x, y), false);
		return true;
	});

	// the item id in the active weapon slot ("" when the slot is empty)
	m.def("player_weapon", [](int ClientID) -> std::string {
		CCharacter *pChr = Character(ClientID);
		if(!pChr)
			return "";
		const CItemID Weapon = pChr->GetActiveWeapon();
		const char *pName = Weapon.SourceName();
		return pName ? pName : "";
	});

	// the usable weapon id (a weapon manager id, matching context weapon params)
	m.def("player_weapon_id", [](int ClientID) {
		CCharacter *pChr = Character(ClientID);
		return pChr ? (int) pChr->GetUsableWeapon() : -1;
	});

	m.def("player_set_weapon", [](int ClientID, int Slot) {
		CCharacter *pChr = Character(ClientID);
		if(!pChr || Slot < 0 || Slot >= NUM_WEAPONS)
			return false;
		pChr->SetWeapon(Slot);
		return true;
	});

	m.def("item_count", [](int ClientID, const std::string &ItemId) {
		CGameContext *pGame = Game();
		if(!pGame)
			return 0;
		return pGame->Item()->GetItemCount(ClientID, CItemID::FromName(ItemId.c_str()));
	});

	m.def("item_give", [](int ClientID, const std::string &ItemId, int Count) {
		CGameContext *pGame = Game();
		return pGame && pGame->Item()->AddItem(ClientID, ItemId.c_str(), Count, true);
	});

	m.def("item_take", [](int ClientID, const std::string &ItemId, int Count) {
		CGameContext *pGame = Game();
		return pGame && pGame->Item()->RemoveItem(ClientID, CItemID::FromName(ItemId.c_str()), Count);
	});

	// Placement metadata. Returns (has_data, ...); item scripts use the values
	// from the item definition instead of hard-coding them.
	m.def("item_drill", [](const std::string &ItemId) {
		py::tuple Result(3);
		CGameContext *pGame = Game();
		CItemID Id;
		if(!pGame || !CItemID::Parse(ItemId.c_str(), &Id) || Id.IsZero())
		{
			Result[0] = py::cast(false);
			Result[1] = py::cast(0);
			Result[2] = py::cast(0);
			return Result;
		}
		const CItemSystem::SDrill Drill = pGame->Item()->GetDrill(Id);
		Result[0] = py::cast(Drill.m_HasDrill);
		Result[1] = py::cast(Drill.m_MaxLength);
		Result[2] = py::cast(Drill.m_Power);
		return Result;
	});

	m.def("item_health_jar", [](const std::string &ItemId) {
		py::tuple Result(4);
		CGameContext *pGame = Game();
		CItemID Id;
		if(!pGame || !CItemID::Parse(ItemId.c_str(), &Id) || Id.IsZero())
		{
			Result[0] = py::cast(false);
			Result[1] = py::cast(0);
			Result[2] = py::cast(0);
			Result[3] = py::cast(0);
			return Result;
		}
		const CItemSystem::SHealthJar Def = pGame->Item()->GetHealthJar(Id);
		Result[0] = py::cast(Def.m_HasJar);
		Result[1] = py::cast(Def.m_Cooldown);
		Result[2] = py::cast(Def.m_Range);
		Result[3] = py::cast(Def.m_Health);
		return Result;
	});

	// Constructor-style host surface. The Python Projectile/Laser classes in
	// _context.py call these; the old procedural names are kept for scripts that
	// predate the class wrappers.
	m.def("create_projectile", [](int Owner, float x, float y, float vx, float vy, int Span, int Damage, bool Explosive, int SoundImpact, int Weapon) {
		return CreateProjectileScript(Owner, x, y, vx, vy, Span, Damage, Explosive, SoundImpact, Weapon);
	});
	m.def("projectile_spawn", [](int Owner, float x, float y, float vx, float vy, int Span, int Damage, bool Explosive, int SoundImpact, int Weapon) {
		return CreateProjectileScript(Owner, x, y, vx, vy, Span, Damage, Explosive, SoundImpact, Weapon);
	});

	m.def("create_laser", [](int Owner, float x, float y, float dx, float dy, float Reach, int Damage) {
		return CreateLaserScript(Owner, x, y, dx, dy, Reach, Damage);
	});
	m.def("laser_spawn", [](int Owner, float x, float y, float dx, float dy, float Reach, int Damage) {
		return CreateLaserScript(Owner, x, y, dx, dy, Reach, Damage);
	});

	m.def("sound", [](float x, float y, int Sound) {
		CGameContext *pGame = Game();
		if(!pGame || !pGame->m_apPlayers[0])
			return false;
		CGameWorld *pWorld = pGame->m_apPlayers[0]->GameWorld();
		if(!pWorld)
			return false;
		pWorld->CreateSound(vec2(x, y), Sound);
		return true;
	});

	m.def("hammer_hit", [](float x, float y) {
		CGameContext *pGame = Game();
		if(!pGame || !pGame->m_apPlayers[0])
			return false;
		CGameWorld *pWorld = pGame->m_apPlayers[0]->GameWorld();
		if(!pWorld)
			return false;
		pWorld->CreateHammerHit(vec2(x, y));
		return true;
	});

	m.def("random_int", []() { return random_int(); });

	m.def("random_int_range", [](int Min, int Max) {
		if(Max <= Min)
			return Min;
		return Min + (int) (random_int() % (unsigned) (Max - Min + 1));
	});

	// ---- server clock, worlds and snapshot ids ----------------------------
	m.def("tick", []() {
		CGameContext *pGame = Game();
		return pGame ? pGame->Server()->Tick() : 0;
	});

	m.def("tick_speed", []() {
		CGameContext *pGame = Game();
		return pGame ? pGame->Server()->TickSpeed() : 0;
	});

	// The world the current callback runs in (0 outside a callback when no
	// world is loaded yet).
	m.def("active_world", []() -> unsigned {
		CScriptRuntime *pRuntime = CScriptRuntime::Get();
		CGameWorld *pWorld = pRuntime ? pRuntime->ActiveWorld() : nullptr;
		return pWorld ? pWorld->WorldID() : 0;
	});

	m.def("world_name", [](unsigned WorldID) -> std::string {
		CGameContext *pGame = Game();
		const char *pName = pGame ? pGame->Server()->GetMapName(WorldID) : "";
		return pName ? pName : "";
	});

	// "Depths" for every Depths_<seed> instance, the map name otherwise.
	m.def("floor_name", [](unsigned WorldID) -> std::string {
		CGameContext *pGame = Game();
		const char *pName = pGame ? pGame->Server()->GetMapFloorName(WorldID) : "";
		return pName ? pName : "";
	});

	m.def("world_kind", [](unsigned WorldID) {
		CGameContext *pGame = Game();
		return pGame ? pGame->Server()->GetMapWorldKind(WorldID) : -1;
	});

	// ---- placement primitives -------------------------------------------
	m.def("world_test_box", [](float X, float Y, float Width, float Height) -> bool {
		CGameWorld *pWorld = ActiveWorld();
		if(!pWorld || Width <= 0.0f || Height <= 0.0f)
			return false;
		return pWorld->Collision()->TestBox(vec2(X, Y), vec2(Width, Height)) != 0;
	});

	// (found, floor_y): the first solid surface below (X, Y), snapped to a tile
	// boundary. found is false when there is no floor within MaxDrop.
	m.def("world_floor_y", [](float X, float Y, float MaxDrop) {
		py::tuple Result(2);
		CGameWorld *pWorld = ActiveWorld();
		vec2 Hit;
		bool Found = false;
		int FloorY = 0;
		if(pWorld && MaxDrop > 0.0f && pWorld->Collision()->IntersectLine(vec2(X, Y), vec2(X, Y + MaxDrop), &Hit, nullptr))
		{
			Found = true;
			FloorY = round_to_int(Hit.y / 32.0f) * 32;
		}
		Result[0] = py::cast(Found);
		Result[1] = py::cast(FloorY);
		return Result;
	});

	m.def("drill_near", [](float X, float Y, float Radius) -> bool {
		CGameWorld *pWorld = ActiveWorld();
		if(!pWorld)
			return false;
		const float Reach = Radius > 0.0f ? Radius : 40.0f;
		for(CGameWorld::TypeRange r = pWorld->DoTypeRange(CGameWorld::ENTTYPE_DRILL); !r.empty(); r.pop_front())
			if(distance(r.front()->GetPos(), vec2(X, Y)) < Reach)
				return true;
		return false;
	});

	m.def("drill_spawn", [](const std::string &ItemId, float X, float Y, int MaxLength, int Power, int Owner) -> int {
		CGameWorld *pWorld = ActiveWorld();
		CGameContext *pGame = Game();
		if(!pWorld || !pGame || !ValidPos(X, Y))
			return -1;
		if(Owner < 0 || Owner >= MAX_CLIENTS || !pGame->m_apPlayers[Owner])
			return -1;
		CItemID Id;
		if(!CItemID::Parse(ItemId.c_str(), &Id) || Id.IsZero())
			return -1;
		const Uuid OwnerAccount = pGame->m_apPlayers[Owner]->m_AccountUuid;
		CDrillEntity *pDrill = new CDrillEntity(pWorld, vec2(X, Y), Id, MaxLength, Power, Owner, OwnerAccount);
		return pDrill ? pDrill->GetID() : -1;
	});

	// Snapshot ids are pooled per floor. snap_new_id()/snap_free_id() use the
	// callback's world; the _for variants are for console or delayed code that
	// names the world explicitly. Freeing takes the world because the same
	// numeric id may be live on two different floors.
	m.def("snap_new_id", []() -> int {
		CScriptRuntime *pRuntime = CScriptRuntime::Get();
		CGameWorld *pWorld = pRuntime ? pRuntime->ActiveWorld() : nullptr;
		if(!pWorld)
			return -1;
		return pWorld->Server()->SnapNewID(pWorld->WorldID());
	});

	m.def("snap_new_id_for", [](unsigned WorldID) -> int {
		CGameContext *pGame = Game();
		if(!pGame || !pGame->WorldByID(WorldID))
			return -1;
		return pGame->Server()->SnapNewID(WorldID);
	});

	m.def("snap_free_id", [](int ID) -> bool {
		CScriptRuntime *pRuntime = CScriptRuntime::Get();
		CGameWorld *pWorld = pRuntime ? pRuntime->ActiveWorld() : nullptr;
		if(!pWorld)
			return false;
		return pWorld->Server()->SnapFreeID(ID, pWorld->WorldID());
	});

	m.def("snap_free_id_for", [](int ID, unsigned WorldID) -> bool {
		CGameContext *pGame = Game();
		if(!pGame)
			return false;
		return pGame->Server()->SnapFreeID(ID, WorldID);
	});

	m.def("tuning", [](const std::string &Name) -> float {
		CGameContext *pGame = Game();
		if(!pGame)
			return 0.0f;
		if(Name == "gun_lifetime")
			return pGame->Tuning()->m_GunLifetime;
		if(Name == "shotgun_lifetime")
			return pGame->Tuning()->m_ShotgunLifetime;
		if(Name == "grenade_lifetime")
			return pGame->Tuning()->m_GrenadeLifetime;
		if(Name == "laser_reach")
			return pGame->Tuning()->m_LaserReach;
		if(Name == "shotgun_speeddiff")
			return pGame->Tuning()->m_ShotgunSpeeddiff;
		return 0.0f;
	});

	m.def("buff_add", [](int ClientID, const std::string &BuffId, int DurationTicks, int Stacks, int Source) {
		CGameContext *pGame = Game();
		return pGame && pGame->Buffs() && pGame->Buffs()->Add(ClientID, BuffId.c_str(), DurationTicks, Stacks, Source);
	});

	m.def("buff_remove", [](int ClientID, const std::string &BuffId) {
		CGameContext *pGame = Game();
		return pGame && pGame->Buffs() && pGame->Buffs()->Remove(ClientID, BuffId.c_str());
	});

	m.def("buff_stacks", [](int ClientID, const std::string &BuffId) {
		CGameContext *pGame = Game();
		return pGame && pGame->Buffs() ? pGame->Buffs()->Stacks(ClientID, BuffId.c_str()) : 0;
	});
}
