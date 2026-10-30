/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>
#include <base/tl/array.h>
#include <engine/server.h>
#include <game/server/entities/character.h>
#include <game/server/entities/resource.h>
#include <game/server/entity.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>
#include <game/server/scripting/script_runtime.h>
#include <game/server/scripting/scripted_weapon.h>
#include <generated/protocol.h>
#include <generated/server_data.h>

#include <pybind11/embed.h>

namespace py = pybind11;

// Server-data damage for this scripted weapon, used by the scripts as the
// fallback when the consumed ammo carries no damage. Sword and hand have no
// meaningful entry of their own: the sword renders as ninja but hits like the
// hammer (hammer damage + 2) and the bare hand always deals 1.
static int ScriptedWeaponBaseDamage(const char *pName, int SnapStyle)
{
	if(str_comp(pName, "hand") == 0)
		return 1;
	if(str_comp(pName, "sword") == 0)
		return g_pData->m_Weapons.m_aId[WEAPON_HAMMER].m_Damage + 2;
	if(SnapStyle >= 0 && SnapStyle < g_pData->m_Weapons.m_NumId)
		return g_pData->m_Weapons.m_aId[SnapStyle].m_Damage;
	return 0;
}

static CGameContext *ScriptGame()
{
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	return pRuntime ? pRuntime->GameServer() : nullptr;
}

static CCharacter *ScriptCharacter(int ClientID)
{
	CGameContext *pGame = ScriptGame();
	return pGame ? pGame->GetPlayerChar(ClientID) : nullptr;
}

CScriptedWeapon::CScriptedWeapon(const char *pName, const char *pScript, int SnapStyle,
	bool FullAuto, int FireDelay, int DefaultAmmo, int MaxAmmo) :
	m_SnapStyle(SnapStyle),
	m_FullAuto(FullAuto),
	m_FireDelay(FireDelay),
	m_DefaultAmmo(DefaultAmmo),
	m_MaxAmmo(MaxAmmo),
	m_Module(-1)
{
	str_copy(m_aName, pName ? pName : "", sizeof(m_aName));
	str_copy(m_aScript, pScript ? pScript : "", sizeof(m_aScript));
}

int CScriptedWeapon::DefaultDamage() const
{
	return ScriptedWeaponBaseDamage(m_aName, m_SnapStyle);
}

void CScriptedWeapon::FillContext(SScriptContext &Ctx, CCharacter *pChr, const char *pEvent) const
{
	Ctx.m_pGameServer = pChr->GameServer();
	Ctx.m_ClientID = pChr->GetCID();
	Ctx.m_EntityID = pChr->GetID();
	Ctx.m_WorldID = pChr->GameWorld()->WorldID();
	Ctx.m_Pos = pChr->GetPos();
	Ctx.m_Dir = pChr->AimDirection();
	str_copy(Ctx.m_aWeapon, m_aName, sizeof(Ctx.m_aWeapon));
	str_copy(Ctx.m_aEvent, pEvent, sizeof(Ctx.m_aEvent));
	Ctx.m_Now = pChr->Server()->Tick();
	Ctx.m_TickSpeed = pChr->Server()->TickSpeed();
	Ctx.m_RangeScale = pChr->RangeScale();

	// Values the scripts cannot ask the engine for: the data-driven damage
	// fallback, the snap style used for hit/kill messages and the projectile
	// launch offset (the native weapons use proximity_radius * 0.75).
	char aExtra[192];
	str_format(aExtra, sizeof(aExtra),
		"{\"default_damage\":%d,\"snap_style\":%d,\"proximity_radius\":%.3f,\"fire_delay\":%d,\"max_ammo\":%d}",
		DefaultDamage(), m_SnapStyle, pChr->GetProximityRadius(), m_FireDelay, m_MaxAmmo);
	Ctx.m_Extra = aExtra;
}

bool CScriptedWeapon::LoadScript()
{
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	if(!pRuntime || !pRuntime->Ready())
	{
		m_Module = -1;
		return false;
	}

	m_Module = pRuntime->LoadModule(m_aScript);
	if(m_Module < 0)
		return false;

	// A weapon module without on_fire would silently do nothing on every shot;
	// treat it like a failed load so the weapon is not registered.
	if(!pRuntime->HasFunction(m_Module, "on_fire"))
	{
		dbg_msg("weapon", "scripted weapon '%s': '%s' has no on_fire(), weapon disabled", m_aName, m_aScript);
		m_Module = -1;
		return false;
	}

	return true;
}

void CScriptedWeapon::ReloadScript()
{
	LoadScript();
}

int CScriptedWeapon::OnFire(CCharacter *pChr, vec2 Direction, const SAmmoEffect &Ammo)
{
	if(!pChr || m_Module < 0)
		return m_FireDelay * (pChr ? pChr->Server()->TickSpeed() : 50) / 1000;

	SScriptContext Ctx;
	FillContext(Ctx, pChr, "weapon_fire");
	Ctx.m_Dir = Direction;
	Ctx.m_AmmoDamage = Ammo.m_Damage;
	Ctx.m_AmmoProjectiles = Ammo.m_Projectiles;
	Ctx.m_AmmoSpeed = Ammo.m_Speed;
	Ctx.m_AmmoLifetime = Ammo.m_Lifetime;
	Ctx.m_AmmoEffect = Ammo.m_Effect;

	if(!CScriptRuntime::Get()->Call(m_Module, "on_fire", Ctx))
	{
		dbg_msg("weapon", "script error in '%s' on_fire: %s", m_aName, CScriptRuntime::Get()->LastError());
		return m_FireDelay * pChr->Server()->TickSpeed() / 1000;
	}

	if(Ctx.m_ReloadTicks > 0)
		return Ctx.m_ReloadTicks;
	return m_FireDelay * pChr->Server()->TickSpeed() / 1000;
}

void CScriptedWeapon::OnTick(CCharacter *pChr)
{
	if(!pChr || m_Module < 0)
		return;
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	if(!pRuntime->HasFunction(m_Module, "on_tick"))
		return;

	SScriptContext Ctx;
	FillContext(Ctx, pChr, "weapon_tick");
	if(!pRuntime->Call(m_Module, "on_tick", Ctx))
		dbg_msg("weapon", "script error in '%s' on_tick: %s", m_aName, pRuntime->LastError());
}

void CScriptedWeapon::OnWeaponSwitch(CCharacter *pChr)
{
	if(!pChr || m_Module < 0)
		return;
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	if(!pRuntime->HasFunction(m_Module, "on_switch"))
		return;

	SScriptContext Ctx;
	FillContext(Ctx, pChr, "weapon_switch");
	if(!pRuntime->Call(m_Module, "on_switch", Ctx))
		dbg_msg("weapon", "script error in '%s' on_switch: %s", m_aName, pRuntime->LastError());
}

// ---------------------------------------------------------------------------
// lastday_weapon: the small engine helper surface the melee weapon scripts
// need. Keeping the swing in C++ guarantees the Python hammer/hand/sword share
// one implementation of the hit tests, damage attribution and resource rules.
// ---------------------------------------------------------------------------
PYBIND11_EMBEDDED_MODULE(lastday_weapon, m)
{
	m.def("melee_attack", [](int OwnerClientID, float x, float y, float dx, float dy,
				      int Damage, float RangeScale, bool CanHarvest, int HitStyle, bool SwingEffect)
	{
		CCharacter *pChr = ScriptCharacter(OwnerClientID);
		if(!pChr)
			return 0;
		CGameWorld *pWorld = pChr->GameWorld();
		if(!pWorld)
			return 0;

		const vec2 ChrPos(x, y);
		vec2 Direction(dx, dy);
		if(length(Direction) > 0.0f)
			Direction = normalize(Direction);
		const vec2 ProjStartPos = ChrPos + Direction * pChr->GetProximityRadius() * 0.75f;

		pWorld->CreateSound(ChrPos, SOUND_HAMMER_FIRE);

		// the weapon reported when a hit kills someone: the snap style (hammer,
		// sword/ninja), or the hammer for the invisible hand (style -1)
		const int Style = HitStyle >= 0 ? HitStyle : WEAPON_HAMMER;
		const float Reach = pChr->GetProximityRadius() * 0.5f * RangeScale;

		int Hits = 0;

		array<CEntity *> lpEnts;
		lpEnts.hint_size(8);
		const int Num = pWorld->FindFlagEntities(ProjStartPos, Reach, lpEnts, CGameWorld::ENTFLAG_HITABLE);
		for(int i = 0; i < Num; ++i)
		{
			CHitableEntity *pTarget = static_cast<CHitableEntity *>(lpEnts[i]);
			const vec2 TargetPos = pWorld->PosInFrame(lpEnts[i]);

			if(pTarget == pChr)
				continue;
			if(pWorld->Collision()->IntersectLine(ProjStartPos, TargetPos, NULL, NULL))
				continue;

			if(SwingEffect)
			{
				if(length(TargetPos - ProjStartPos) > 0.0f)
					pWorld->CreateHammerHit(TargetPos - normalize(TargetPos - ProjStartPos) * pChr->GetProximityRadius() * 0.5f);
				else
					pWorld->CreateHammerHit(ProjStartPos);
			}

			vec2 Dir;
			if(length(TargetPos - ChrPos) > 0.0f)
				Dir = normalize(TargetPos - ChrPos);
			else
				Dir = vec2(0.f, -1.f);

			pTarget->TakeHit(vec2(0.f, -1.f) + normalize(Dir + vec2(0.f, -1.1f)) * 10.0f, Dir * -1, Damage, pChr, Style);
			Hits++;
		}

		// harvest resources (default 1 hardness per hit); only weapons that may
		// pick resources reach this code
		if(CanHarvest)
		{
			array<CEntity *> lpResEnts;
			const int NumRes = pWorld->FindEntities(ProjStartPos, (pChr->GetProximityRadius() * 0.5f + 20.0f) * RangeScale, lpResEnts, CGameWorld::ENTTYPE_RESOURCE);
			for(int i = 0; i < NumRes; ++i)
			{
				CResourceEntity *pRes = static_cast<CResourceEntity *>(lpResEnts[i]);
				const vec2 ResPos = pWorld->PosInFrame(pRes);
				if(pRes->IsRespawning())
					continue;
				if(pWorld->Collision()->IntersectLine(ProjStartPos, ResPos, NULL, NULL))
					continue;

				// The last hit destroys the node, so bank the drop first: when
				// the inventory is full the node keeps its final hardness
				// instead of being consumed for an item that no one gets.
				const bool Depleted = pRes->RemainingHardness() <= 1;
				if(Depleted)
				{
					CPlayer *pPlayer = pChr->GetPlayer();
					if(!pPlayer || !pChr->GameServer()->Item() || !pChr->GameServer()->Item()->AddItem(pPlayer->GetCID(), pRes->Item(), 1))
						continue;
					pRes->TakeHit(1);
					pWorld->CreateSound(ResPos, SOUND_PICKUP_ARMOR);
				}
				else
				{
					pRes->TakeHit(1);
					pWorld->CreateHammerHit(ResPos);
				}
				Hits++;
			}
		}

		return Hits;
	});
}
