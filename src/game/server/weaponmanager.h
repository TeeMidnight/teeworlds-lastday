#ifndef GAME_SERVER_WEAPONMANAGER_H
#define GAME_SERVER_WEAPONMANAGER_H

#include <base/tl/array.h>
#include <base/tl/hashtable.h>
#include <base/vmath.h>

class CCharacter;
class IWeaponInterface;

// Manages all weapons. Weapons are collected automatically through the static
// global registrar (REGISTER_WEAPON in weapon.h), keyed by str_quickhash(weapon
// name); any number of weapons is supported (not limited to the 6 protocol
// weapons, the visual style is decided by SnapStyle).
// Lookup by weapon id is O(1) through a hash table; a parallel array keeps the
// registration order for iteration.
// Global singleton access: WeaponManager().
class CWeaponManager
{
	bool m_Initialized;
	// O(1) lookup by weapon id (str_quickhash of the weapon name)
	hash_table<unsigned, IWeaponInterface *, 8> m_Weapons;
	// registration order, used for iteration (GetWeaponByIndex / NumWeapons)
	array<IWeaponInterface *> m_apWeaponList;

	void Init();

public:
	CWeaponManager() : m_Initialized(false) {}

	// called by CWeaponRegistrar (appends to the static registry, may be
	// called repeatedly)
	static void RegisterWeapon(IWeaponInterface *pWeapon);

	IWeaponInterface *GetWeapon(unsigned WeaponID);
	IWeaponInterface *GetWeaponByName(const char *pName);
	IWeaponInterface *GetWeaponByIndex(int Index); // iteration, 0-based
	int NumWeapons() const { return m_apWeaponList.size(); }
	void OutputRegisteredWeapons();

	// convenience attribute lookups
	int SnapStyle(unsigned WeaponID);
	bool IsFullAuto(unsigned WeaponID);

	// fire: returns the reload wait in ticks. AmmoDamage is the damage of the
	// ammo item consumed from the player's inventory (0 = weapon default).
	int FireWeapon(CCharacter *pChr, vec2 Direction, unsigned WeaponID, int AmmoDamage);
	// every tick: calls OnTick of the currently held weapon
	void TickWeapon(CCharacter *pChr);
	// weapon switch: calls OnWeaponSwitch of the currently held weapon
	void OnWeaponSwitch(CCharacter *pChr);
};

CWeaponManager *WeaponManager();

#endif // GAME_SERVER_WEAPONMANAGER_H
