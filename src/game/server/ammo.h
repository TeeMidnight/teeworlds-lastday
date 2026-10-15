/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_AMMO_H
#define GAME_SERVER_AMMO_H

// The effect one round of ammunition applies to the weapon that fires it.
// Weapons read what they understand and ignore the rest, so a content pack can
// give two ammo items for the same weapon very different behaviour.
struct SAmmoEffect
{
	// damage dealt by the round (0 = use the weapon's own default damage)
	int m_Damage;
	// projectile count override (0 = weapon default; shotgun uses this to make
	// e.g. a slug shell fire a single pellet)
	int m_Projectiles;
	// speed / lifetime multipliers (1.0 = weapon default)
	float m_Speed;
	float m_Lifetime;
	// free form effect tag a weapon may interpret (0 = none)
	int m_Effect;

	SAmmoEffect() :
		m_Damage(0),
		m_Projectiles(0),
		m_Speed(1.0f),
		m_Lifetime(1.0f),
		m_Effect(0)
	{
	}
};

#endif // GAME_SERVER_AMMO_H
