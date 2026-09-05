#include <game/server/weapons/weapon_melee.h>

#include <generated/protocol.h>
#include <generated/server_data.h>

// Sword: a melee weapon that renders with the ninja style (WEAPON_NINJA).
// It swings like the hammer but is a pure combat weapon: it deals more damage
// than the hammer (hammer damage + 2) and cannot harvest resources.
class CWeaponSword : public CWeaponMelee
{
public:
	const char *Name() const override { return "sword"; }
	int SnapStyle() const override { return WEAPON_NINJA; }

	// the sword cuts harder than the hammer: hammer damage + 2
	int Damage() const override { return g_pData->m_Weapons.m_aId[WEAPON_HAMMER].m_Damage + 2; }

	// a sword is a pure combat weapon: it cannot harvest resources
	bool CanPickResource() const override { return false; }
};

REGISTER_WEAPON(CWeaponSword)
