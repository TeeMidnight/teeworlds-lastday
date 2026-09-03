#include <game/server/weapon.h>

#include <engine/server.h>

#include <game/server/entities/character.h>
#include <game/server/gameworld.h>
#include <generated/server_data.h>

// Ninja: a permanent weapon (obtained as an item, e.g. through crafting) that
// dashes/swings on fire. The swing state machine is driven every tick through
// OnTick. The per-character state (m_Ninja, m_lpHitObjects) stays on
// CCharacter; CWeaponNinja is a friend so it can drive it directly.
class CWeaponNinja : public IWeaponInterface
{
public:
	const char *Name() const override { return "ninja"; }
	int SnapStyle() const override { return WEAPON_NINJA; }
	int FireDelay() const override { return 800; }
	int DefaultAmmo() const override { return -1; } // no ammo needed
	int MaxAmmo() const override { return 10; }

	int OnFire(CCharacter *pChr, vec2 Direction, int AmmoDamage) override
	{
		(void) AmmoDamage;
		// start a new swing/dash
		pChr->m_lpHitObjects.clear();
		pChr->m_lpHitObjects.hint_size(4);
		pChr->m_Ninja.m_ActivationDir = Direction;
		pChr->m_Ninja.m_CurrentMoveTime = g_pData->m_Weapons.m_Ninja.m_Movetime * pChr->Server()->TickSpeed() / 1000;
		pChr->m_Ninja.m_OldVelAmount = length(pChr->m_Core.m_Vel);
		pChr->GameWorld()->CreateSound(pChr->GetPos(), SOUND_NINJA_FIRE);
		return 0;
	}

	void OnTick(CCharacter *pChr) override
	{
		// nothing to do when we are not swinging
		if(pChr->m_Ninja.m_CurrentMoveTime <= 0)
			return;

		pChr->m_Ninja.m_CurrentMoveTime--;

		if(pChr->m_Ninja.m_CurrentMoveTime == 0)
		{
			// swing over: restore the velocity the player had before dashing
			pChr->m_Core.m_Vel = pChr->m_Ninja.m_ActivationDir * pChr->m_Ninja.m_OldVelAmount;
			return;
		}

		// moving: dash forward and hit everything along the way
		pChr->m_Core.m_Vel = pChr->m_Ninja.m_ActivationDir * g_pData->m_Weapons.m_Ninja.m_Velocity;
		vec2 OldPos = pChr->GetPos();
		pChr->GameWorld()->Collision()->MoveBox(&pChr->m_Core.m_Pos, &pChr->m_Core.m_Vel, vec2(pChr->GetProximityRadius(), pChr->GetProximityRadius()), 0.f);

		// reset velocity so the client doesn't predict stuff
		pChr->m_Core.m_Vel = vec2(0.f, 0.f);

		// check if we hit anything along the way
		const float Radius = pChr->GetProximityRadius() * 2.0f;
		const vec2 Center = OldPos + (pChr->GetPos() - OldPos) * 0.5f;
		array<CEntity *> lpEnts;
		lpEnts.hint_size(8);
		const int Num = pChr->GameWorld()->FindFlagEntities(Center, Radius, lpEnts, CGameWorld::ENTFLAG_HITABLE);

		for(int i = 0; i < Num; ++i)
		{
			if(lpEnts[i] == pChr)
				continue;

			// make sure we haven't hit this object before
			bool AlreadyHit = false;
			for(int j = 0; j < pChr->m_lpHitObjects.size(); j++)
			{
				if(pChr->m_lpHitObjects[j] == lpEnts[i])
				{
					AlreadyHit = true;
					break;
				}
			}
			if(AlreadyHit)
				continue;

			// check so we are sufficiently close
			if(distance(lpEnts[i]->GetPos(), pChr->GetPos()) > Radius)
				continue;

			// hit a player, give him damage and stuffs...
			pChr->GameWorld()->CreateSound(lpEnts[i]->GetPos(), SOUND_NINJA_HIT);
			pChr->m_lpHitObjects.add(lpEnts[i]);

			// set his velocity to fast upward (for now)
			static_cast<CHitableEntity *>(lpEnts[i])->TakeHit(vec2(0, -10.0f), pChr->m_Ninja.m_ActivationDir * -1, g_pData->m_Weapons.m_Ninja.m_pBase->m_Damage, pChr, WEAPON_NINJA);
		}
	}
};

REGISTER_WEAPON(CWeaponNinja)
