/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_WEAPONMANAGER_H
#define GAME_SERVER_WEAPONMANAGER_H

#include <base/system.h>
#include <base/tl/array.h>
#include <base/tl/hashtable.h>
#include <base/vmath.h>
#include <game/server/ammo.h>

class CCharacter;
class CScriptedWeapon;
class IStorage;
class IWeaponInterface;

// Manages all weapons. Native weapons are collected automatically through the
// static global registrar (REGISTER_WEAPON in weapon.h); scripted weapons are
// loaded from data/weapons/*.json. Both are keyed by str_quickhash(weapon name);
// any number of weapons is supported (not limited to the 6 protocol weapons,
// the visual style is decided by SnapStyle).
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

	// scripted weapons (data/weapons/*.json + data/scripts/weapons/*.py).
	// They are owned here and consulted before the native static registry.
	// Except for ninja (and any future native weapon) the script is the only
	// implementation: a missing/broken script disables that weapon.
	hash_table<unsigned, CScriptedWeapon *, 8> m_ScriptedWeapons;
	array<CScriptedWeapon *> m_apScriptedWeaponList;

	void Init();
	void LoadScriptedWeapon(IStorage *pStorage, const char *pDefaultName, const char *pPath);
	static int ListScriptedWeaponsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);

public:
	CWeaponManager() : m_Initialized(false) {}
	~CWeaponManager() { ClearScriptedWeapons(); }

	// called by CWeaponRegistrar (appends to the static registry, may be
	// called repeatedly)
	static void RegisterWeapon(IWeaponInterface *pWeapon);

	IWeaponInterface *GetWeapon(unsigned WeaponID);
	IWeaponInterface *GetWeaponByName(const char *pName);
	IWeaponInterface *GetWeaponByIndex(int Index); // iteration, 0-based
	int NumWeapons() const { return m_apWeaponList.size(); }
	void OutputRegisteredWeapons();

	// Loads every data/weapons/*.json and compiles its script. A missing/broken
	// script is logged and skipped (the weapon is then unavailable unless a
	// native class of that name exists). Call after CScriptRuntime::Init().
	void LoadScriptedWeapons(IStorage *pStorage);
	// Re-reads the JSON definitions and re-executes the Python modules.
	void ReloadScripts();
	// Deletes all scripted overrides (the native registry is untouched).
	void ClearScriptedWeapons();
	int NumScriptedWeapons() const { return m_apScriptedWeaponList.size(); }
	CScriptedWeapon *GetScriptedWeaponByIndex(int Index) const;

	// convenience attribute lookups
	int SnapStyle(unsigned WeaponID);
	bool IsFullAuto(unsigned WeaponID);

	// fire: returns the reload wait in ticks. Ammo carries the effect of the
	// ammo item consumed from the player's inventory.
	int FireWeapon(CCharacter *pChr, vec2 Direction, unsigned WeaponID, const SAmmoEffect &Ammo);
	// every tick: calls OnTick of the currently held weapon
	void TickWeapon(CCharacter *pChr);
	// weapon switch: calls OnWeaponSwitch of the currently held weapon
	void OnWeaponSwitch(CCharacter *pChr);
};

CWeaponManager *WeaponManager();

#endif // GAME_SERVER_WEAPONMANAGER_H
