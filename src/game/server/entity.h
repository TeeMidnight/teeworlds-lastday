/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_ENTITY_H
#define GAME_SERVER_ENTITY_H

#include <base/vmath.h>

#include "alloc.h"
#include "gameworld.h"

// Damage source of an entity without a client slot. The value carries the team
// so friendly fire still resolves; CCharacter::TakeDamage already reads these
// negative values out of its From argument.
enum
{
	PLAYER_TEAM_BLUE = -2,
	PLAYER_TEAM_RED = -1
};

/*
	Class: Entity
		Basic entity class.
*/
class CEntity
{
	MACRO_ALLOC_HEAP()

private:
	/* Friend classes */
	friend class CGameWorld; // for entity list handling

	/* Identity */
	class CGameWorld *m_pGameWorld;

	CEntity *m_pPrevTypeEntity;
	CEntity *m_pNextTypeEntity;

	int m_ID;
	int m_ObjType;
	int m_ObjFlag;

	/*
		Variable: m_ProximityRadius
			Contains the physical size of the entity.
	*/
	float m_ProximityRadius;

	/* State */
	bool m_MarkedForDestroy;

protected:
	/* State */

	/*
		Variable: m_Pos
			Contains the current posititon of the entity.
	*/
	vec2 m_Pos;

public:
	/* Constructor */
	CEntity(CGameWorld *pGameWorld, int Objtype, int ObjFlag, vec2 Pos, int ProximityRadius = 0);

	/* Destructor */
	virtual ~CEntity();

	void SetGameWorld(class CGameWorld *pGameWorld) { m_pGameWorld = pGameWorld; }

	// Ring: shift this entity, and whatever it renders from, into a neighbouring
	// instance's frame for the duration of one mirrored Snap call. The default
	// moves m_Pos; a projectile renders from its launch point in m_Pos, so the
	// default already moves the whole flight. Entities that render from other
	// fields (a character or bot core, a laser's from point) override it.
	virtual void ShiftForSnap(vec2 Delta) { m_Pos += Delta; }
	// Ring: whether this entity follows the boundary transfer when it crosses a
	// trigger. Off by default; movable entities opt in.
	virtual bool RingCrosses() const { return false; }

	// Ring crossings. RingPos is where the entity really is: a projectile keeps
	// its launch point in m_Pos rather than its current point, so it overrides
	// this. OnRingTransfer rebases the entity onto the neighbour's axis when it
	// crosses, keeping the same global column.
	virtual vec2 RingPos() { return m_Pos; }
	virtual void OnRingTransfer(float ArrivalX) { m_Pos.x = ArrivalX; }

	/* Objects */
	class CGameWorld *GameWorld() { return m_pGameWorld; }
	class CConfig *Config() { return m_pGameWorld->Config(); }
	class CGameContext *GameServer() { return m_pGameWorld->GameServer(); }
	class IServer *Server() { return m_pGameWorld->Server(); }

	int ObjType() const { return m_ObjType; }
	int ObjFlag() const { return m_ObjFlag; }

	/* Getters */
	int GetID() const { return m_ID; }
	const vec2 &GetPos() const { return m_Pos; }

	// The client id to credit for a hit this entity lands, or a negative
	// PLAYER_TEAM_* sentinel when it has no client slot (a bot). -1 keeps the
	// historical "no attacker / world" meaning.
	virtual int GetHitAttacker() { return -1; }
	float GetProximityRadius() const { return m_ProximityRadius; }
	bool IsMarkedForDestroy() const { return m_MarkedForDestroy; }

	/* Setters */
	void MarkForDestroy() { m_MarkedForDestroy = true; }

	/* Other functions */

	/*
		Function: Destroy
			Destroys the entity.
	*/
	virtual void Destroy() { delete this; }

	/*
		Function: Reset
			Called when the game resets the map. Puts the entity
			back to its starting state or perhaps destroys it.
	*/
	virtual void Reset() {}

	/*
		Function: Tick
			Called to progress the entity to the next tick. Updates
			and moves the entity to its new state and position.
	*/
	virtual void Tick() {}

	/*
		Function: TickDefered
			Called after all entities Tick() function has been called.
	*/
	virtual void TickDefered() {}

	/*
		Function: TickPaused
			Called when the game is paused, to freeze the state and position of the entity.
	*/
	virtual void TickPaused() {}

	/*
		Function: Snap
			Called when a new snapshot is being generated for a specific
			client.

		Arguments:
			SnappingClient - ID of the client which snapshot is
				being generated. Could be -1 to create a complete
				snapshot of everything in the game for demo
				recording.
	*/
	virtual void Snap(int SnappingClient) {}

	virtual void PostSnap() {}

	/*
		Function: networkclipped(int snapping_client)
			Performs a series of test to see if a client can see the
			entity.

		Arguments:
			SnappingClient - ID of the client which snapshot is
				being generated. Could be -1 to create a complete
				snapshot of everything in the game for demo
				recording.

		Returns:
			Non-zero if the entity doesn't have to be in the snapshot.
	*/
	int NetworkClipped(int SnappingClient);
	int NetworkClipped(int SnappingClient, vec2 CheckPos);
	int NetworkClippedLine(int SnappingClient, vec2 From, vec2 To);

	bool GameLayerClipped(vec2 CheckPos);
};

class CHitableEntity : public CEntity
{
public:
	/* Constructor */
	CHitableEntity(CGameWorld *pGameWorld, int Objtype, int ObjFlag, vec2 Pos, int ProximityRadius = 0) :
		CEntity(pGameWorld, Objtype, ObjFlag | CGameWorld::ENTFLAG_HITABLE, Pos, ProximityRadius)
	{
	}
	virtual bool TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) = 0;
};

class CChildEntity : public CEntity
{
protected:
	int m_Owner;

public:
	/* Constructor */
	CChildEntity(CGameWorld *pGameWorld, int Objtype, int ObjFlag, vec2 Pos, int ProximityRadius = 0) :
		CEntity(pGameWorld, Objtype, ObjFlag | CGameWorld::ENTFLAG_CHILD, Pos, ProximityRadius)
	{
	}

	int GetOwner() const { return m_Owner; }
};
#endif
