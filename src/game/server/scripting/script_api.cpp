/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/scripting/script_runtime.h>

#include <base/system.h>

#include <game/server/entities/character.h>
#include <game/server/entities/laser.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
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
	m.def("entity_spawn", [](const std::string &, float, float, const std::string &) { return -1; });
	m.def("entity_destroy", [](int) { return false; });
	m.def("entity_pos", [](int) { return Vec2Tuple(0.0f, 0.0f); });
	m.def("entity_health", [](int) { return 0; });
	m.def("entity_damage", [](int, int, int, int) { return false; });
	m.def("entity_set_visual", [](int, const std::string &) { return false; });
	m.def("bot_set_input", [](int, int, bool, bool, float, float) { return false; });
}
