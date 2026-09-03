#include <game/server/weapon.h>

#include <game/server/entities/character.h>
#include <game/server/entities/laser.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <generated/server_data.h>

// Laser: hitscan beam with bouncing, full auto.
class CWeaponLaser : public IWeaponInterface
{
public:
	const char *Name() const override { return "laser"; }
	int SnapStyle() const override { return WEAPON_LASER; }
	bool FullAuto() const override { return true; }
	int FireDelay() const override { return 800; }
	int DefaultAmmo() const override { return 10; }
	int MaxAmmo() const override { return 10; }

	int OnFire(CCharacter *pChr, vec2 Direction, int AmmoDamage) override
	{
		CGameWorld *pWorld = pChr->GameWorld();
		const int ClientID = pChr->GetCID();
		const vec2 ChrPos = pChr->GetPos();
		const int Damage = AmmoDamage ? AmmoDamage : g_pData->m_Weapons.m_aId[SnapStyle()].m_Damage;

		new CLaser(pWorld, ChrPos, Direction, pChr->GameServer()->Tuning()->m_LaserReach, ClientID, Damage);
		pWorld->CreateSound(ChrPos, SOUND_LASER_FIRE);
		return 0;
	}
};

REGISTER_WEAPON(CWeaponLaser)
