/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_CONTAINER_H
#define GAME_SERVER_CONTAINER_H

#include <base/system.h>
#include <base/tl/array.h>
#include <base/tl/hashtable.h>
#include <game/item_id.h>

class CContainerEntity;
class CGameContext;
class IStorage;

// A container holds one stack per item kind. The capacity is the number of
// different item kinds it can hold, not the number of items: a crate with one
// slot can hold a thousand stone blocks, but no second material.
enum
{
	MAX_CONTAINER_SLOTS = 4,
};

// A container kind loaded from data/containers.json. Kinds are content, not
// code: a map structure names the kind it wants and the server places that
// kind of container there.
struct SContainerDef
{
	char m_aKind[32];
	char m_aName[64];
	int m_Capacity;
	// client pickup sprite used as the icon in the middle of the container
	int m_Pickup;
};

// One weighted entry of a loot table. The item id is resolved when the table
// loads, so an entry naming an item that does not exist is skipped with a log
// instead of producing an unrenderable stack.
struct SContainerLootEntry
{
	CItemID m_Id;
	char m_aItem[64];
	int m_CountMin;
	int m_CountMax;
	int m_Weight;
	// 0 common, 1 uncommon, 2 rare, 3 legendary. Purely descriptive; the
	// weight is what actually balances the table.
	int m_Rarity;
};

struct SContainerLootTable
{
	char m_aId[32];
	int m_RollsMin;
	// at most MAX_CONTAINER_SLOTS distinct stacks are ever produced; the
	// container capacity caps the roll count again
	int m_RollsMax;
	array<SContainerLootEntry> m_lEntries;
};

// Loads data/containers.json plus one loot table per file under data/loot/ and
// owns both for the process lifetime. The mapping from a structure's container
// to a loot table lives in the structure definition, not in the container kind.
class CContainerSystem
{
public:
	~CContainerSystem();

	void Init(CGameContext *pGameServer, IStorage *pStorage);
	void Load(IStorage *pStorage);

	const SContainerDef *Find(const char *pKind) const;
	int NumDefs() const { return m_lDefs.size(); }
	const SContainerDef *DefAt(int Index) const;

	const SContainerLootTable *FindLootTable(const char *pId) const;
	int NumLootTables() const { return m_lLootTables.size(); }

	// Fill a freshly spawned container from its loot table. Deterministic for a
	// given seed, capped by the container capacity, and always marks the
	// container as initialized so a later world save can tell an emptied
	// container from one that has never been opened.
	void FillLoot(CContainerEntity *pContainer, unsigned Seed) const;

private:
	CGameContext *m_pGameServer = nullptr;
	IStorage *m_pStorage = nullptr;

	array<SContainerDef *> m_lDefs;
	hash_table<unsigned, SContainerDef *, 8> m_Defs;

	array<SContainerLootTable *> m_lLootTables;
	hash_table<unsigned, SContainerLootTable *, 8> m_LootTables;

	void LoadLoot(IStorage *pStorage);
	static int ListLootFilesCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	void LoadLootTable(const char *pDefaultId, const char *pFilePath);
};

#endif // GAME_SERVER_CONTAINER_H
