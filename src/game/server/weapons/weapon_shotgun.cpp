#include <game/server/weapon.h>

#include <game/server/entities/character.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <generated/server_data.h>

// Shotgun: spread burst of pellets, full auto.
class CWeaponShotgun : public IWeaponInterface
{
public:
	const char *Name() const override { return "shotgun"; }
	int SnapStyle() const override { return WEAPON_SHOTGUN; }
	bool FullAuto() const override { return true; }
	int FireDelay() const override { return 500; }
	int DefaultAmmo() const override { return 10; }
	int MaxAmmo() const override { return 10; }

	int OnFire(CCharacter *pChr, vec2 Direction, int AmmoDamage) override
	{
		CGameWorld *pWorld = pChr->GameWorld();
		const int ClientID = pChr->GetCID();
		const vec2 ProjStartPos = pChr->GetPos() + Direction * pChr->GetProximityRadius() * 0.75f;
		const int Damage = AmmoDamage ? AmmoDamage : g_pData->m_Weapons.m_aId[SnapStyle()].m_Damage;

		int ShotSpread = 2;

		for(int i = -ShotSpread; i <= ShotSpread; ++i)
		{
			float Spreading[] = {-0.185f, -0.070f, 0, 0.070f, 0.185f};
			float a = angle(Direction);
			a += Spreading[i + 2];
			float v = 1 - (absolute(i) / (float) ShotSpread);
			float Speed = mix((float) pChr->GameServer()->Tuning()->m_ShotgunSpeeddiff, 1.0f, v);
			new CProjectile(pWorld, SnapStyle(), ClientID, ProjStartPos,
				vec2(cosf(a), sinf(a)) * Speed,
				(int) (pChr->Server()->TickSpeed() * pChr->GameServer()->Tuning()->m_ShotgunLifetime),
				Damage, false, 0, -1, SnapStyle());
		}

		pWorld->CreateSound(pChr->GetPos(), SOUND_SHOTGUN_FIRE);
		return 0;
	}
};

REGISTER_WEAPON(CWeaponShotgun)
