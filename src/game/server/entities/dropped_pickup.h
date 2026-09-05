#ifndef GAME_SERVER_ENTITIES_DROPPEDPICKUP_H
#define GAME_SERVER_ENTITIES_DROPPEDPICKUP_H

#include <game/server/entity.h>

// A player-dropped item lying (or flying) in a world. It is rendered as a
// hammer pickup and behaves like a small physics object: gravity pulls it
// down, CCollision::MoveBox resolves the movement against solid tiles with a
// light bounce, and horizontal friction is very low (but not zero) so thrown
// items glide a long way and only slowly come to rest.
//
// A pickup itself carries no persistence logic: while a world is loaded the
// pickups live purely in memory, and the world's dropped items are stored
// and restored through CGameWorld (SaveToDatabase/RestoreFromDatabase),
// which will grow to cover the other persistent world attributes too.
class CDroppedPickup : public CEntity
{
public:
	CDroppedPickup(CGameWorld *pGameWorld, vec2 Pos, vec2 Direction, const char *pItemId, int Count);

	virtual void Tick();
	virtual void Snap(int SnappingClient);

	// the dropped item's res_id / stack size
	const char *ItemId() const { return m_aItemId; }
	int Count() const { return m_Count; }

	enum
	{
		// box extents passed to MoveBox (a box of this size is ~2/3 tile
		// wide), so the item fits into one-tile corridors
		DROP_BOX = 10,
		// the client renders the pickup roughly this large
		DROP_PHYS_SIZE = 14,
	};

private:
	char m_aItemId[32];
	int m_Count;
	vec2 m_Vel;
	// ticks to wait before the item can be picked up again (gives a thrown
	// item time to fly away from its owner)
	int m_NoPickupTick;

	bool IsGrounded();
};

#endif // GAME_SERVER_ENTITIES_DROPPEDPICKUP_H
