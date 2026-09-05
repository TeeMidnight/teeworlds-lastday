#include <game/server/weapons/weapon_melee.h>

#include <generated/protocol.h>
#include <generated/server_data.h>

// Hammer: melee hit against nearby hittable entities plus resource harvesting.
class CWeaponHammer : public CWeaponMelee
{
public:
	const char *Name() const override { return "hammer"; }
	int SnapStyle() const override { return WEAPON_HAMMER; }

	// damage comes from the hammer weapon data
	int Damage() const override { return g_pData->m_Weapons.m_aId[SnapStyle()].m_Damage; }

	// the hammer is the harvesting tool: its swing picks up resources
	bool CanPickResource() const override { return true; }
};

REGISTER_WEAPON(CWeaponHammer)
