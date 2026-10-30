/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_SCRIPTING_SCRIPTED_WEAPON_H
#define GAME_SERVER_SCRIPTING_SCRIPTED_WEAPON_H

#include <game/server/weapon.h>

struct SScriptContext;
class CCharacter;

// A weapon whose fire/tick/switch behaviour lives in a PocketPy module under
// data/scripts/weapons. The class only carries the metadata the rest of the
// game needs (name, snap style, ammo defaults) and translates the engine calls
// into SScriptContext callbacks. It is owned by CWeaponManager and is the only
// implementation of the weapon, unless a native class of the same name exists.
class CScriptedWeapon : public IWeaponInterface
{
public:
	CScriptedWeapon(const char *pName, const char *pScript, int SnapStyle,
		bool FullAuto, int FireDelay, int DefaultAmmo, int MaxAmmo);

	const char *Name() const override { return m_aName; }
	int SnapStyle() const override { return m_SnapStyle; }
	bool FullAuto() const override { return m_FullAuto; }
	int FireDelay() const override { return m_FireDelay; }
	int DefaultAmmo() const override { return m_DefaultAmmo; }
	int MaxAmmo() const override { return m_MaxAmmo; }

	int OnFire(CCharacter *pChr, vec2 Direction, const SAmmoEffect &Ammo) override;
	void OnTick(CCharacter *pChr) override;
	void OnWeaponSwitch(CCharacter *pChr) override;

	const char *Script() const { return m_aScript; }
	int Module() const { return m_Module; }

	// Loads/reloads the Python module and verifies that it implements on_fire.
	// Returns false when the script is missing or failed to compile; the caller
	// then discards the scripted weapon.
	bool LoadScript();
	void ReloadScript();

private:
	char m_aName[64];
	char m_aScript[IO_MAX_PATH_LENGTH];
	int m_SnapStyle;
	bool m_FullAuto;
	int m_FireDelay;
	int m_DefaultAmmo;
	int m_MaxAmmo;
	int m_Module;

	// fills the common part of a callback context (player, position, tick,
	// weapon name and the small JSON block with data-driven defaults)
	void FillContext(SScriptContext &Ctx, CCharacter *pChr, const char *pEvent) const;
	// returns the server_data damage for this weapon, used by the Python
	// scripts as the fallback when the consumed ammo carries no damage
	int DefaultDamage() const;
};

#endif // GAME_SERVER_SCRIPTING_SCRIPTED_WEAPON_H
