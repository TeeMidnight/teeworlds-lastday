/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/weapon.h>

#include <game/server/entities/character.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <generated/server_data.h>

// Shotgun: spread burst of pellets, full auto. Ammo can override the pellet
// count (e.g. a slug shell that fires exactly one pellet).
class CWeaponShotgun : public IWeaponInterface
{
public:
	const char *Name() const override { return "shotgun"; }
	int SnapStyle() const override { return WEAPON_SHOTGUN; }
	bool FullAuto() const override { return true; }
	int FireDelay() const override { return 500; }
	int DefaultAmmo() const override { return 10; }
	int MaxAmmo() const override { return 10; }

	int OnFire(CCharacter *pChr, vec2 Direction, const SAmmoEffect &Ammo) override
	{
		CGameWorld *pWorld = pChr->GameWorld();
		const int ClientID = pChr->GetCID();
		const vec2 ProjStartPos = pChr->GetPos() + Direction * pChr->GetProximityRadius() * 0.75f;
		const int Damage = Ammo.m_Damage ? Ammo.m_Damage : g_pData->m_Weapons.m_aId[SnapStyle()].m_Damage;
		const int Span = (int) (pChr->Server()->TickSpeed() * pChr->GameServer()->Tuning()->m_ShotgunLifetime * Ammo.m_Lifetime * pChr->RangeScale());

		const int BuiltInPellets = 5;
		int PelletCount = Ammo.m_Projectiles > 0 ? Ammo.m_Projectiles : BuiltInPellets;
		if(PelletCount < 1)
			PelletCount = 1;

		for(int i = 0; i < PelletCount; ++i)
		{
			// spread the pellets evenly over the same arc the built-in shotgun
			// uses; a single pellet flies straight
			const float Offset = PelletCount > 1
				? -0.185f + 0.370f * ((float) i / (float) (PelletCount - 1))
				: 0.0f;
			float a = angle(Direction) + Offset;
			float v = PelletCount > 1
				? 1.0f - absolute((float) i / (float) (PelletCount - 1) - 0.5f) * 2.0f * (1.0f - pChr->GameServer()->Tuning()->m_ShotgunSpeeddiff)
				: 1.0f;
			float Speed = v * Ammo.m_Speed;
			new CProjectile(pWorld, SnapStyle(), ClientID, ProjStartPos,
				vec2(cosf(a), sinf(a)) * Speed,
				Span, Damage, false, 0, -1, SnapStyle());
		}

		pWorld->CreateSound(pChr->GetPos(), SOUND_SHOTGUN_FIRE);
		return 0;
	}
};

REGISTER_WEAPON(CWeaponShotgun)
