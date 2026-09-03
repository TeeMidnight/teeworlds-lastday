#include <game/server/weapon.h>

#include <game/server/entities/character.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <generated/server_data.h>

// Grenade: explosive projectile, full auto.
class CWeaponGrenade : public IWeaponInterface
{
public:
	const char *Name() const override { return "grenade"; }
	int SnapStyle() const override { return WEAPON_GRENADE; }
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

		new CProjectile(pWorld, SnapStyle(), ClientID, ProjStartPos, Direction,
			(int) (pChr->Server()->TickSpeed() * pChr->GameServer()->Tuning()->m_GrenadeLifetime),
			Damage, true, 0, SOUND_GRENADE_EXPLODE, SnapStyle());

		pWorld->CreateSound(pChr->GetPos(), SOUND_GRENADE_FIRE);
		return 0;
	}
};

REGISTER_WEAPON(CWeaponGrenade)
