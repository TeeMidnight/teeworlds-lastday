#include <game/server/weapon.h>

#include <game/server/entities/character.h>
#include <game/server/entities/resource.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/item.h>
#include <game/server/player.h>
#include <generated/server_data.h>

// Hammer: melee hit against nearby hittable entities plus resource harvesting.
class CWeaponHammer : public IWeaponInterface
{
public:
	const char *Name() const override { return "hammer"; }
	int SnapStyle() const override { return WEAPON_HAMMER; }
	// the hammer is always available: unlimited ammo
	int FireDelay() const override { return 125; }
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

		// melee hit
		array<CEntity *> lpEnts;
		lpEnts.hint_size(8);
		const int Num = pWorld->FindFlagEntities(ProjStartPos, pChr->GetProximityRadius() * 0.5f, lpEnts, CGameWorld::ENTFLAG_HITABLE);
		for(int i = 0; i < Num; ++i)
		{
			CCharacter *pTarget = static_cast<CCharacter *>(lpEnts[i]);

			if((pTarget == pChr) || pWorld->Collision()->IntersectLine(ProjStartPos, pTarget->GetPos(), NULL, NULL))
				continue;

			// set his velocity to fast upward (for now)
			if(length(pTarget->GetPos() - ProjStartPos) > 0.0f)
				pWorld->CreateHammerHit(pTarget->GetPos() - normalize(pTarget->GetPos() - ProjStartPos) * pChr->GetProximityRadius() * 0.5f);
			else
				pWorld->CreateHammerHit(ProjStartPos);

			vec2 Dir;
			if(length(pTarget->GetPos() - ChrPos) > 0.0f)
				Dir = normalize(pTarget->GetPos() - ChrPos);
			else
				Dir = vec2(0.f, -1.f);

			pTarget->TakeHit(vec2(0.f, -1.f) + normalize(Dir + vec2(0.f, -1.1f)) * 10.0f, Dir * -1, g_pData->m_Weapons.m_aId[SnapStyle()].m_Damage, pChr, SnapStyle());
			Hits++;
		}

		// harvest resources with the hammer (default 1 hardness per hit)
		array<CEntity *> lpResEnts;
		const int NumRes = pWorld->FindEntities(ProjStartPos, pChr->GetProximityRadius() * 0.5f + 20.0f, lpResEnts, CGameWorld::ENTTYPE_RESOURCE);
		for(int i = 0; i < NumRes; ++i)
		{
			CResourceEntity *pRes = static_cast<CResourceEntity *>(lpResEnts[i]);
			if(pRes->IsRespawning())
				continue;
			if(pWorld->Collision()->IntersectLine(ProjStartPos, pRes->GetPos(), NULL, NULL))
				continue;

			// remember the resource id before the hit: the entity is
			// destroyed when it gets depleted
			const bool Depleted = pRes->RemainingHardness() <= 1;
			char aResId[32];
			if(Depleted)
				str_copy(aResId, pRes->ResId(), sizeof(aResId));

			pRes->TakeHit(1);

			if(Depleted)
			{
				CPlayer *pPlayer = pChr->GetPlayer();
				if(pPlayer && !pChr->GameServer()->Item()->AddItem(pPlayer->GetCID(), aResId, 1))
					continue;
				pWorld->CreateSound(pRes->GetPos(), SOUND_PICKUP_ARMOR);
			}
			else
			{
				pWorld->CreateHammerHit(pRes->GetPos());
			}
			Hits++;
		}

		// if we hit anything, we have to wait for the reload
		return Hits ? pChr->Server()->TickSpeed() / 3 : 0;
	}
};

REGISTER_WEAPON(CWeaponHammer)
