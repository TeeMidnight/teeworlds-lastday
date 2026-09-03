#ifndef GAME_SERVER_WEAPON_H
#define GAME_SERVER_WEAPON_H

#include <base/system.h>
#include <base/vmath.h>

class CCharacter;

// Weapon ID: str_quickhash() of the (lower-case) weapon name, an unsigned value.
// The weapon count is not limited by the network protocol: any custom weapon
// can be registered.
inline unsigned WeaponID(const char *pName)
{
	return str_quickhash(pName);
}

// Weapon interface: every weapon lives in its own .cpp, inherits this class and
// is registered automatically through a static global variable (REGISTER_WEAPON
// at the bottom of the file) - no explicit include/registration elsewhere.
class IWeaponInterface
{
public:
	virtual ~IWeaponInterface() {}

	// weapon name (its str_quickhash is the weapon ID)
	virtual const char *Name() const = 0;
	// network snap style: which built-in weapon the client uses to render this
	// weapon (WEAPON_GUN etc.)
	virtual int SnapStyle() const = 0;

	// fire behaviour: pChr is the shooter. AmmoDamage is the damage of the
	// ammo item that was consumed from the player's inventory (0 when the
	// weapon needs no ammo, or when the ammo carries no damage of its own).
	// returns the reload wait in ticks (0 = use FireDelay())
	virtual int OnFire(CCharacter *pChr, vec2 Direction, int AmmoDamage) = 0;

	// called on every game tick for every character holding this weapon
	// (persistent state, cooldowns, ...)
	virtual void OnTick(CCharacter *pChr) {}

	// called when the weapon is switched to
	virtual void OnWeaponSwitch(CCharacter *pChr) {}

	virtual bool FullAuto() const { return false; }
	virtual int FireDelay() const { return 0; }
	virtual int DefaultAmmo() const { return 0; }
	virtual int MaxAmmo() const { return 10; }

	// unique identifier
	unsigned ID() const { return WeaponID(Name()); }
};

class CWeaponRegistrar
{
public:
	CWeaponRegistrar(IWeaponInterface *pWeapon);
};

#define REGISTER_WEAPON(WeaponClass) \
	static WeaponClass s_##WeaponClass##Instance; \
	static CWeaponRegistrar s_##WeaponClass##Registrar(&s_##WeaponClass##Instance);

#endif // GAME_SERVER_WEAPON_H
