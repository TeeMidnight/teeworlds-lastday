/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_ENTITIES_HEALTH_DROP_H
#define GAME_SERVER_ENTITIES_HEALTH_DROP_H

#include <game/server/entity.h>

/*
	Class: CHealthDropEntity
		A health pickup the jar spat out. It falls to the ground like a small
		physics object and then heals exactly like a normal health pickup: a tee
		touching it gains one point of health and the pickup is gone (no respawn).
		At full health it stays on the ground for the next tee.
*/
class CHealthDropEntity : public CEntity
{
public:
	CHealthDropEntity(CGameWorld *pGameWorld, vec2 Pos, vec2 Velocity);

	void Tick() override;
	void TickPaused() override;
	void Snap(int SnappingClient) override;

private:
	enum
	{
		DROP_BOX = 10,
		PICKUP_RADIUS = 20,
	};

	bool IsGrounded();

	vec2 m_Vel;
	// short grace period so the drop has time to fall out of the jar before
	// the owner standing inside it can collect it
	int m_NoPickupTick;
};

#endif // GAME_SERVER_ENTITIES_HEALTH_DROP_H
