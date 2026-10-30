/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_ENTITIES_DROPPED_PICKUP_H
#define GAME_SERVER_ENTITIES_DROPPED_PICKUP_H

#include <game/item_id.h>
#include <game/server/entity.h>

// A small physics pickup in a world. One class covers every payload:
//
//   KIND_ITEM     - a player-dropped item stack; picked up into the inventory.
//   KIND_HEALTH   - a health pickup dropped by a jar; heals one heart.
//   KIND_CURRENCY - a stack of money (G) dropped by a monster; picked up
//                   straight onto the account ledger. It is rendered as a
//                   weapon projectile so a pile of coins reads as a scatter of
//                   bullets rather than as an item.
//
// They share gravity, collision, the pickup grace period and the snap object.
// Only the collection effect, snap sprite and persistence flag differ.
class CDroppedPickup : public CEntity
{
public:
	enum EKind
	{
		KIND_ITEM = 0,
		KIND_HEALTH,
		KIND_CURRENCY,
	};

	// item stack
	CDroppedPickup(CGameWorld *pGameWorld, vec2 Pos, vec2 Direction, const CItemID &ItemId, int Count);
	// health pickup (Health is normally 1)
	CDroppedPickup(CGameWorld *pGameWorld, vec2 Pos, vec2 Direction, int Health);
	// money drop; the tag keeps this overload distinct from the health one
	CDroppedPickup(CGameWorld *pGameWorld, vec2 Pos, vec2 Direction, int64 Amount, EKind CurrencyTag);

	virtual void Tick();
	virtual void TickPaused();
	virtual void Snap(int SnappingClient);
	// ring: a dropped item or money crosses the boundary like a player does; a
	// health pickup is transient and stays with its jar's world
	virtual bool RingCrosses() const override { return m_Kind != KIND_HEALTH; }

	EKind Kind() const { return m_Kind; }
	bool IsHealthDrop() const { return m_Kind == KIND_HEALTH; }
	bool IsCurrencyDrop() const { return m_Kind == KIND_CURRENCY; }
	// health drops are transient props, not world content to save
	bool IsPersistent() const { return m_Kind == KIND_ITEM || m_Kind == KIND_CURRENCY; }

	// the dropped item's id / stack size (zero for a health or money pickup)
	const CItemID &ItemId() const { return m_ItemId; }
	int Count() const { return m_Count; }
	int Health() const { return m_Health; }
	// the amount of G carried by a money pickup (0 otherwise)
	int64 Amount() const { return m_Amount; }

	enum
	{
		// box extents passed to MoveBox (a box of this size is ~2/3 tile
		// wide), so the item fits into one-tile corridors
		DROP_BOX = 10,
		// the client renders the pickup roughly this large
		DROP_PHYS_SIZE = 14,
		HEALTH_PHYS_SIZE = 20,
	};

private:
	EKind m_Kind;
	CItemID m_ItemId;
	int m_Count;
	int m_Health;
	int64 m_Amount;
	// money drops use one of the projectile sprites of the game's weapons
	int m_VisualType;
	vec2 m_Vel;
	// ticks to wait before the pickup can be collected again (gives a thrown
	// item time to fly away from its owner)
	int m_NoPickupTick;

	bool IsGrounded();
};

#endif // GAME_SERVER_ENTITIES_DROPPED_PICKUP_H
