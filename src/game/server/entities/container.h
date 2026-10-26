/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_ENTITIES_CONTAINER_H
#define GAME_SERVER_ENTITIES_CONTAINER_H

#include <game/item_id.h>
#include <game/server/container.h>
#include <game/server/entity.h>

// A storage container standing in the world: a crate, a chest or a vault. Each
// slot holds one item stack, so the capacity is the number of *different* item
// kinds it can store, not the number of items. Definitions live in
// data/containers.json. A generated map structure names the kind it spawns and
// the loot table it wants that container to use.
//
// The entity is drawn with the standard snapshot primitives (a laser box and a
// pickup icon in the middle), so unmodified clients can see it.
class CContainerEntity : public CEntity
{
public:
	enum
	{
		// enough beams for the box, its lid and the lock: every snapshot item
		// needs its own id
		NUM_BEAMS = 6,
		// aiming and snapping radius of the prop
		PHYS_SIZE = 24,
	};

	struct SSlot
	{
		CItemID m_Id;
		int m_Count;
	};

	CContainerEntity(CGameWorld *pGameWorld, vec2 Pos, const char *pKind, const char *pName,
		const char *pLootTable, int Capacity, int Pickup, int StorageID);
	~CContainerEntity() override;

	void Snap(int SnappingClient) override;

	const char *Kind() const { return m_aKind; }
	const char *Name() const { return m_aName; }
	const char *LootTable() const { return m_aLootTable; }
	int Pickup() const { return m_Pickup; }
	int Capacity() const { return m_Capacity; }
	// stable id within the map instance, used as the world-save key
	int StorageID() const { return m_StorageID; }

	// Whether the initial loot has been rolled (or restored from a save). A
	// container is saved even when empty once this is set, so an emptied
	// container never refills.
	bool Initialized() const { return m_LootInitialized; }
	void SetInitialized(bool Initialized) { m_LootInitialized = Initialized; }

	int UsedSlots() const;
	int FindSlot(const CItemID &Id) const;
	int FindEmptySlot() const;
	const SSlot &Slot(int Index) const;
	int TotalCount() const;

	// whether this container can accept the given stack: a matching slot or a
	// free one. The count itself is unbounded.
	bool CanStore(const CItemID &Id) const;
	// add a stack; false when the item kind does not fit
	bool Store(const CItemID &Id, int Count);
	// remove up to Count items from a slot; returns how many were removed
	int Take(int Slot, int Count);
	// restore a stack from a world save; slots are filled in order
	bool RestoreSlot(const CItemID &Id, int Count);

private:
	void SnapBeam(int ID, vec2 From, vec2 To, int SnappingClient);

	char m_aKind[32];
	char m_aName[64];
	char m_aLootTable[32];
	int m_Capacity;
	int m_Pickup;
	int m_StorageID;
	bool m_LootInitialized;

	SSlot m_aSlots[MAX_CONTAINER_SLOTS];
	int m_aBeamIDs[NUM_BEAMS];
};

#endif // GAME_SERVER_ENTITIES_CONTAINER_H
