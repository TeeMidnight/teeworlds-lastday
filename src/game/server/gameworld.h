/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_GAMEWORLD_H
#define GAME_SERVER_GAMEWORLD_H

#include <game/gamecore.h>
#include <game/layers.h>
#include "eventhandler.h"

class CEntity;
class CCharacter;
class CResourceEntity;
struct _json_value;
typedef struct _json_value json_value;
struct SNpcDef;

/*
	Class: Game World
		Tracks all entities in the game. Propagates tick and
		snap calls to all entities.
*/
class CGameWorld
{
public:
	enum
	{
		ENTTYPE_PROJECTILE = 0,
		ENTTYPE_LASER,
		ENTTYPE_PICKUP,
		ENTTYPE_CHARACTER,
		ENTTYPE_FLAG,

		ENTTYPE_RIWALL,
		ENTTYPE_RESOURCE,
		ENTTYPE_DROPPEDPICKUP,
		ENTTYPE_DRILL,
		ENTTYPE_BOTENTITY,
		// generic scripted prop (data/entities/*.json + data/scripts/entities/*.py)
		ENTTYPE_SCRIPTED,
		NUM_ENTTYPES,

		ENTFLAG_HITABLE = 1,
		ENTFLAG_CHILD = 2,
	};

private:
	void RemoveEntities();

	array<CEntity *> m_alpEntityLists[NUM_ENTTYPES];
	array<CEntity *> m_lpFlagEntityList;

	class CGameContext *m_pGameServer;
	class CConfig *m_pConfig;
	class IServer *m_pServer;

	CLayers m_Layers;
	CCollision m_Collision;

	// One live ring neighbour of this world, with the offset (world units) that
	// maps its local x onto this world's axis. The entity queries use these so a
	// hit, a pickup or an explosion can cross the shared boundary.
	struct SRingNeighbour
	{
		CGameWorld *m_pWorld;
		vec2 m_Offset;
	};
	int CollectRingNeighbours(SRingNeighbour *pOut, int Max);

public:
	class CGameContext *GameServer() const { return m_pGameServer; }
	class CConfig *Config() { return m_pConfig; }
	class IServer *Server() { return m_pServer; }
	class CCollision *Collision() { return &m_Collision; }

	// the one global bot manager; it owns every world's bots and their
	// per-client tee id mapping
	class CBotManager *BotManager();
	class CBotManager *GetBotManager() const;
	bool HasBotManager() const;

	// Bot layer configuration of this world. The manager owns the bots; a world
	// only records whether it wants any and, for a content world, the configured
	// NPCs whose fixed identity it respawns.
	bool m_BotConfigured;
	bool m_BotNpcRole;
	array<const SNpcDef *> m_lBotDefinedNpcs;
	bool IsNpcWorld() const { return m_BotNpcRole; }
	bool BotConfigured() const { return m_BotConfigured; }

	CEventHandler m_Events;
	CWorldCore m_Core;

	unsigned m_WorldID;
	unsigned WorldID() const { return m_WorldID; }

	// canonical name of the map backing this world (e.g. "Connector" or the
	// generated "<floor>_<seed>"); used as the persistence key of the world
	// saves in the database
	char m_aMapName[64];

	// Ring geometry of a generated instance, read from the world json. Disabled
	// (the default, and every hand authored map) means the instance is a world
	// of its own; enabled means its left and right edges lead into the
	// neighbouring instances of the floor's ring.
	bool m_Ring;
	int m_RingSeed;     // this instance's index, 1..m_RingSize
	int m_RingSize;
	int m_SharedMargin; // tiles shared with each neighbour
	int m_OffsetX;      // global x of this instance's local x = 0
	int m_Stride;       // distance between two neighbouring instances
	int m_Period;       // circumference of the whole ring

	// Ring: the live neighbours' players are bound into this world's core under
	// their real client id, translated onto this world's axis, so a hook or a
	// player collision in this world sees them. A hook's drag applied to a proxy
	// is forwarded to the real core at the end of the tick.
	CCharacterCore m_aRingProxyCores[MAX_CLIENTS];
	CGameWorld *m_apRingProxyOwner[MAX_CLIENTS];

	// Ring: a bot of a live neighbour is borrowed into this world's core under
	// its globally unique private tee id, so a hook or a collision on this side
	// reaches the bot as well. Player ids and bot ids never overlap, so a world
	// can hold both kinds of proxy at once. The cores are heap allocated:
	// CWorldCore keeps a pointer to them and the proxy list may grow.
	struct SBotRingProxy
	{
		CCharacterCore *m_pCore;
		CGameWorld *m_pOwner;
		int m_TeeID;
		bool m_Seen;
	};
	array<SBotRingProxy> m_lBotRingProxies;
	int FindBotRingProxy(int TeeID);

	bool IsRing() const { return m_Ring; }
	// x (world units) at which walking right / left leaves this instance
	float RingRightTriggerX() const;
	float RingLeftTriggerX() const;
	// The map name of the neighbour in Direction (+1 right, -1 left) and the
	// arrival x for a character leaving at LocalX. False when this is not a ring.
	bool RingNeighbour(int Direction, char *pMapName, int MapNameSize, float LocalX, float *pArrivalX) const;
	// bind / update the neighbour proxies (call before the worlds tick)
	void RefreshRingProxies();
	// release the proxies that no longer have an owner and pass any hook drag on
	void ForwardRingProxyDrag();
	// The live ring neighbour worlds of this world, with the offset (world
	// units) that maps each neighbour's local x onto this world's axis. Returns
	// the number written (at most Max).
	int RingNeighbours(CGameWorld **ppWorlds, vec2 *pOffsets, int Max);

	// persist the whole state of this world (dropped item pickups today,
	// and any future world attributes added later) into the world_saves row
	// of m_aMapName. Called when the world is unloaded and when the server
	// shuts down; never while the world is running.
	void SaveToDatabase();
	// restore the world state from its world_saves row. Called when the
	// world is loaded; does not write to the database.
	void RestoreFromDatabase();

	CGameWorld(CGameContext *pGameServer);
	~CGameWorld();

	array<vec2> m_alSpawnPoints[3];
	void InitCollision(class IMap *pMap);

	// entrance regions defined in the map info (MapVersion json) of the
	// generated worlds. When a character enters one of these tile regions
	// it is teleported to the target map.
	struct CEntranceInfo
	{
		int m_StartX, m_StartY; // tile coordinates, inclusive
		int m_EndX, m_EndY;
		char m_aTargetMap[64];
	};
	array<CEntranceInfo> m_lEntrances;
	void ParseEntrances(const char *pJsonData);
	void ParseEntrances(const json_value &rEntrances);
	void SpawnResources(const char *pJsonData);
	void SpawnResources(const json_value &rResources);
	// apply the world geometry object (wrap / ring) read from the generated
	// world metadata or, for older maps, from an embedded json map item
	void ApplyWorldJson(const json_value &rWorld);

	typedef array<CEntity *>::range TypeRange;
	TypeRange DoTypeRange(int Type);

	class CFlagCheck
	{
	public:
		int m_ConditionFlag;
		CFlagCheck() {};
		CFlagCheck(int Flag) { m_ConditionFlag = Flag; }
		bool operator()(CEntity *&pEntity) const;
	};

	typedef conditional_range<CEntity *, CFlagCheck> FlagRange;
	FlagRange DoFlagRange(int Flag);
	/*
		Function: find_entities
			Finds entities close to a position and returns them in a list.

		Arguments:
			pos - Position.
			radius - How close the entities have to be.
			ents - Pointer to a list that should be filled with the pointers
				to the entities.
			max - Number of entities that fits into the ents array.
			type - Type of the entities to find.

		Returns:
			Number of entities found and added to the ents array.
	*/
	int FindEntities(vec2 Pos, float Radius, array<CEntity *> &lpEnts, int Type);
	int FindFlagEntities(vec2 Pos, float Radius, array<CEntity *> &lpEnts, int Flag);

	/*
		Function: closest_CEntity
			Finds the closest CEntity of a type to a specific point.

		Arguments:
			pos - The center position.
			radius - How far off the CEntity is allowed to be
			type - Type of the entities to find.
			notthis - Entity to ignore

		Returns:
			Returns a pointer to the closest CEntity or NULL if no CEntity is close enough.
	*/
	CEntity *ClosestEntity(vec2 Pos, float Radius, int Type, CEntity *pNotThis, bool AcrossRing = false);

	// The entity with this snapshot id in this world, or null. Scripted props
	// address each other through the id handed to their spawn callback.
	CEntity *FindEntityByID(int ID);
	CEntity *ClosestFlagEntity(vec2 Pos, float Radius, int Flag, CEntity *pNotThis);

	// The closest harvestable resource to Pos, including nodes owned by a live
	// ring neighbour. Respawning nodes are invisible, so they never win the
	// lookup and cannot hide a live node behind them.
	CResourceEntity *ClosestResource(vec2 Pos, float Radius);

	/*
		Function: interserct_CEntity
			Finds the closest CEntity that intersects the line.

		Arguments:
			pos0 - Start position
			pos2 - End position
			radius - How for from the line the CEntity is allowed to be.
			new_pos - Intersection position
			type - Type of the entities to find.
			notthis - Entity to ignore intersecting with

		Returns:
			Returns a pointer to the closest hit or NULL of there is no intersection.
	*/
	CEntity *IntersectEntity(vec2 Pos0, vec2 Pos1, float Radius, vec2 &NewPos, int Type, CEntity *pNotThis = 0);
	CEntity *IntersectFlagEntity(vec2 Pos0, vec2 Pos1, float Radius, vec2 &NewPos, int Flag, CEntity *pNotThis = 0);

	// The position of an entity as seen in this world's frame: an entity of a
	// live ring neighbour is shifted by the boundary offset, everything else is
	// its own position. Callers acting on a cross-boundary hit must use this
	// instead of GetPos().
	vec2 PosInFrame(CEntity *pEnt);

	/*
		Function: insert_entity
			Adds an entity to the world.

		Arguments:
			entity - Entity to add
	*/
	void InsertEntity(CEntity *pEntity);

	/*
		Function: remove_entity
			Removes an entity from the world.

		Arguments:
			entity - Entity to remove
	*/
	void RemoveEntity(CEntity *pEntity);

	/*
		Function: destroy_entity
			Destroys an entity in the world.

		Arguments:
			entity - Entity to destroy
	*/
	void DestroyEntity(CEntity *pEntity);

	/*
		Function: snap
			Calls snap on all the entities in the world to create
			the snapshot.

		Arguments:
			snapping_client - ID of the client which snapshot
			is being created.
	*/
	void Snap(int SnappingClient);

	void PostSnap();

	/*
		Function: tick
			Calls tick on all the entities in the world to progress
			the world to the next tick.

	*/
	void Tick();

	// helper functions
	void CreateDamage(vec2 Pos, int Id, vec2 Source, int HealthAmount, int ArmorAmount, bool Self);
	void CreateExplosion(vec2 Pos, class CEntity *pOwner, int Weapon, int MaxDamage);
	void CreateHammerHit(vec2 Pos);
	void CreatePlayerSpawn(vec2 Pos);
	void CreateDeath(vec2 Pos, int Who);
	void CreateSound(vec2 Pos, int Sound, int64 Mask = -1);
};

#endif
