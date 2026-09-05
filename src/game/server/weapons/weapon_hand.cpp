#include <game/server/weapons/weapon_melee.h>

// Hand: the fallback weapon. It is used when the player attacks from an empty
// loadout slot (i.e. has no weapon equipped there). SnapStyle -1 tells the
// client that no weapon is shown. A weak melee punch, always available.
class CWeaponHand : public CWeaponMelee
{
public:
	const char *Name() const override { return "hand"; }
	int SnapStyle() const override { return -1; }
	// punch is slower than the other melee weapons
	int FireDelay() const override { return 500; }

	// a bare fist punch deals minimal damage
	int Damage() const override { return 1; }

	// fists can't harvest resources and don't show a hit spark effect
	bool CanPickResource() const override { return false; }
	bool SwingHitEffect() const override { return false; }
};

REGISTER_WEAPON(CWeaponHand)
