/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/entities/character.h>
#include <game/server/entities/resource.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>
#include <game/server/weapons/weapon_melee.h>

// Shared melee swing used by every melee weapon (hammer, hand, sword, ...):
// hit every hittable entity in front of the character once and, when
// CanPickResource() returns true, harvest every resource in reach. The reload
// wait depends on whether the swing connected anything.
int CWeaponMelee::OnFire(CCharacter *pChr, vec2 Direction, const SAmmoEffect &Ammo)
{
	(void) Ammo;
	CGameWorld *pWorld = pChr->GameWorld();
	const vec2 ChrPos = pChr->GetPos();
	const vec2 ProjStartPos = ChrPos + Direction * pChr->GetProximityRadius() * 0.75f;

	pWorld->CreateSound(ChrPos, SOUND_HAMMER_FIRE);

	// the weapon reported when a hit kills someone: the snap style (hammer,
	// sword/ninja), or the hammer for the invisible hand (style -1)
	const int HitStyle = SnapStyle() >= 0 ? SnapStyle() : WEAPON_HAMMER;
	const int Dmg = Damage();

	int Hits = 0;

	// melee hit
	array<CEntity *> lpEnts;
	lpEnts.hint_size(8);
	const int Num = pWorld->FindFlagEntities(ProjStartPos, pChr->GetProximityRadius() * 0.5f * pChr->RangeScale(), lpEnts, CGameWorld::ENTFLAG_HITABLE);
	for(int i = 0; i < Num; ++i)
	{
		// every ENTFLAG_HITABLE entity is a CHitableEntity, not necessarily a
		// CCharacter: a bot is hittable too, so cast to the common base
		CHitableEntity *pTarget = static_cast<CHitableEntity *>(lpEnts[i]);

		if((pTarget == pChr) || pWorld->Collision()->IntersectLine(ProjStartPos, pWorld->PosInFrame(pTarget), NULL, NULL))
			continue;

		if(SwingHitEffect())
		{
			// set his velocity to fast upward (for now)
			if(length(pWorld->PosInFrame(pTarget) - ProjStartPos) > 0.0f)
				pWorld->CreateHammerHit(pWorld->PosInFrame(pTarget) - normalize(pWorld->PosInFrame(pTarget) - ProjStartPos) * pChr->GetProximityRadius() * 0.5f);
			else
				pWorld->CreateHammerHit(ProjStartPos);
		}

		vec2 Dir;
		if(length(pWorld->PosInFrame(pTarget) - ChrPos) > 0.0f)
			Dir = normalize(pWorld->PosInFrame(pTarget) - ChrPos);
		else
			Dir = vec2(0.f, -1.f);

		pTarget->TakeHit(vec2(0.f, -1.f) + normalize(Dir + vec2(0.f, -1.1f)) * 10.0f, Dir * -1, Dmg, pChr, HitStyle);
		Hits++;
	}

	// harvest resources (default 1 hardness per hit); only weapons that may
	// pick resources (CanPickResource) reach this code
	if(CanPickResource())
	{
		array<CEntity *> lpResEnts;
		const int NumRes = pWorld->FindEntities(ProjStartPos, (pChr->GetProximityRadius() * 0.5f + 20.0f) * pChr->RangeScale(), lpResEnts, CGameWorld::ENTTYPE_RESOURCE);
		for(int i = 0; i < NumRes; ++i)
		{
			CResourceEntity *pRes = static_cast<CResourceEntity *>(lpResEnts[i]);
			if(pRes->IsRespawning())
				continue;
			if(pWorld->Collision()->IntersectLine(ProjStartPos, pWorld->PosInFrame(pRes), NULL, NULL))
				continue;

			// The last hit destroys the node, so bank the drop first: when the
			// inventory is full the node keeps its final hardness instead of
			// being consumed for an item that no one gets.
			const bool Depleted = pRes->RemainingHardness() <= 1;
			if(Depleted)
			{
				CPlayer *pPlayer = pChr->GetPlayer();
				if(!pPlayer || !pChr->GameServer()->Item()->AddItem(pPlayer->GetCID(), pRes->Item(), 1))
					continue;
				pRes->TakeHit(1);
				pWorld->CreateSound(pWorld->PosInFrame(pRes), SOUND_PICKUP_ARMOR);
			}
			else
			{
				pRes->TakeHit(1);
				pWorld->CreateHammerHit(pWorld->PosInFrame(pRes));
			}
			Hits++;
		}
	}

	// if we hit anything, we have to wait for the reload
	return Hits ? pChr->Server()->TickSpeed() / 3 : 0;
}
