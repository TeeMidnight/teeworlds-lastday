/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_GAMEWORLD_H
#define GAME_SERVER_GAMEWORLD_H

#include <game/gamecore.h>
#include <game/layers.h>
#include "eventhandler.h"

class CEntity;
class CCharacter;

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

public:
	class CGameContext *GameServer() { return m_pGameServer; }
	class CConfig *Config() { return m_pConfig; }
	class IServer *Server() { return m_pServer; }
	class CCollision *Collision() { return &m_Collision; }

	CEventHandler m_Events;
	CWorldCore m_Core;

	unsigned m_WorldID;
	unsigned WorldID() const { return m_WorldID; }

	// canonical name of the map backing this world (e.g. "Connector" or the
	// generated "<floor>_<seed>"); used as the persistence key of the world
	// saves in the database
	char m_aMapName[64];

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

	char m_aEntrances[2][32];
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
	void SpawnResources(const char *pJsonData);

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
	CEntity *ClosestEntity(vec2 Pos, float Radius, int Type, CEntity *pNotThis);
	CEntity *ClosestFlagEntity(vec2 Pos, float Radius, int Flag, CEntity *pNotThis);

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
