/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_ENTITIES_FIRE_PARTICLE_H
#define GAME_SERVER_ENTITIES_FIRE_PARTICLE_H

#include <base/tl/timer.h>
#include <game/server/entity.h>

// A fire puff. It moves with its own velocity, is pulled down by gravity and
// stops when it hits solid ground, then lingers for a short fade before it is
// removed. The stock 0.7 client draws no sprite for a hammer projectile but
// still emits its bullet-trail particle, so the entity snaps as a
// WEAPON_HAMMER projectile while the server owns the swept hit test.
//
// A stationary puff (zero velocity, zero gravity - the campfire's flame) uses
// a point hit test instead: the projectile scan is only meaningful for a
// moving segment.
class CFireParticle : public CChildEntity
{
public:
	CFireParticle(CGameWorld *pGameWorld, vec2 Pos, vec2 Velocity, int Owner, int Span, int Damage, int Weapon, int IgnoreEntityID, float Gravity);

	virtual void Reset();
	virtual void Tick();
	virtual void TickPaused();
	virtual void Snap(int SnappingClient);

	// A flying puff continues into the neighbouring instance at the seam; a
	// stationary flame (the campfire) stays where it was placed.
	virtual bool RingCrosses() const override { return !m_Landed && length(m_Vel) > 0.0001f; }

	// The campfire must not burn its own prop: the id is resolved every tick so
	// a destroyed target cannot leave a dangling pointer.
	void SetIgnoreEntity(int EntityID) { m_IgnoreEntityID = EntityID; }

	// A client slot can be reused by the next player: drop the reference when
	// its owner leaves, exactly like a projectile.
	void LoseOwner();

private:
	CHitableEntity *FindPointTarget(CEntity *pNotThis, CEntity *pIgnore);

	vec2 m_Vel;
	tick_timer m_LifeSpan;
	tick_timer m_FadeSpan;
	int m_StartTick;
	int m_OwnerTeam;
	int m_Damage;
	int m_Weapon;
	int m_IgnoreEntityID;
	float m_Gravity;
	bool m_Landed;
};

#endif
