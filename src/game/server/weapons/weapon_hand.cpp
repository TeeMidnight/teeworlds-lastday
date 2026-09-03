#include <game/server/weapon.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>

// Hand: the fallback weapon. It is used when the player attacks from an empty
// loadout slot (i.e. has no weapon equipped there). SnapStyle -1 tells the
// client that no weapon is shown. A weak melee punch, always available.
class CWeaponHand : public IWeaponInterface
{
public:
	const char *Name() const override { return "hand"; }
	int SnapStyle() const override { return -1; }
	// punch is always available: unlimited ammo
	int FireDelay() const override { return 500; }
	int DefaultAmmo() const override { return -1; }
	int MaxAmmo() const override { return 10; }

	int OnFire(CCharacter *pChr, vec2 Direction, int AmmoDamage) override
	{
		(void) AmmoDamage;
		CGameWorld *pWorld = pChr->GameWorld();
		const vec2 ChrPos = pChr->GetPos();
		const vec2 ProjStartPos = ChrPos + Direction * pChr->GetProximityRadius() * 0.75f;

		pWorld->CreateSound(ChrPos, SOUND_HAMMER_FIRE);

		int Hits = 0;

		// short-range melee punch
		array<CEntity *> lpEnts;
		lpEnts.hint_size(8);
		const int Num = pWorld->FindFlagEntities(ProjStartPos, pChr->GetProximityRadius() * 0.5f, lpEnts, CGameWorld::ENTFLAG_HITABLE);
		for(int i = 0; i < Num; ++i)
		{
			CCharacter *pTarget = static_cast<CCharacter *>(lpEnts[i]);

			if((pTarget == pChr) || pWorld->Collision()->IntersectLine(ProjStartPos, pTarget->GetPos(), NULL, NULL))
				continue;

			// weak knockback, minimal damage
			vec2 Dir;
			if(length(pTarget->GetPos() - ChrPos) > 0.0f)
				Dir = normalize(pTarget->GetPos() - ChrPos);
			else
				Dir = vec2(0.f, -1.f);

			pTarget->TakeHit(vec2(0.f, -1.f) + normalize(Dir + vec2(0.f, -1.1f)) * 10.0f, Dir * -1, 1, pChr, WEAPON_HAMMER);
			Hits++;
		}

		// if we hit anything, we have to wait for the reload
		return Hits ? pChr->Server()->TickSpeed() / 3 : 0;
	}
};

REGISTER_WEAPON(CWeaponHand)
