/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
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

	int OnFire(CCharacter *pChr, vec2 Direction, const SAmmoEffect &Ammo) override
	{
		CGameWorld *pWorld = pChr->GameWorld();
		const int ClientID = pChr->GetCID();
		const vec2 ChrPos = pChr->GetPos();
		const int Damage = Ammo.m_Damage ? Ammo.m_Damage : g_pData->m_Weapons.m_aId[SnapStyle()].m_Damage;
		const float Reach = pChr->GameServer()->Tuning()->m_LaserReach * Ammo.m_Speed;

		new CLaser(pWorld, ChrPos, Direction, Reach, ClientID, Damage);
		pWorld->CreateSound(ChrPos, SOUND_LASER_FIRE);
		return 0;
	}
};

REGISTER_WEAPON(CWeaponLaser)
