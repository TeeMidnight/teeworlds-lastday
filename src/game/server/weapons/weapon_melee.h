#ifndef GAME_SERVER_WEAPONS_WEAPON_MELEE_H
#define GAME_SERVER_WEAPONS_WEAPON_MELEE_H

#include <game/server/weapon.h>

// Base class for every melee weapon (hammer, hand, sword, ...). One swing
// hits every hittable entity in front of the character and - only for weapons
// that may harvest (CanPickResource) - every resource in reach. Derived
// classes just supply the weapon name, the snap style and the damage; the
// shared swing behaviour lives in CWeaponMelee::OnFire (weapon_melee.cpp).
class CWeaponMelee : public IWeaponInterface
{
public:
	// melee weapons are always available: unlimited ammo (hand overrides the
	// slower punch delay)
	int FireDelay() const override { return 125; }
	int DefaultAmmo() const override { return -1; }
	int MaxAmmo() const override { return 10; }

	// damage one swing deals to characters. every melee weapon decides its
	// own value (hammer: weapon data, hand: 1, sword: hammer damage + 2)
	virtual int Damage() const = 0;

	// whether this weapon can harvest resources. the shared OnFire only picks
	// up resources when this returns true (hammer = harvesting tool, the bare
	// hand and the sword cannot)
	virtual bool CanPickResource() const { return false; }

	// whether a successful swing shows the melee hit spark effect (hammer,
	// sword); the invisible bare hand does not
	virtual bool SwingHitEffect() const { return true; }

	int OnFire(CCharacter *pChr, vec2 Direction, int AmmoDamage) override;
};

#endif // GAME_SERVER_WEAPONS_WEAPON_MELEE_H
