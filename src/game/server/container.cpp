/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/math.h>

#include <engine/shared/jsonparser.h>
#include <engine/storage.h>

#include <generated/protocol.h>

#include <game/server/entities/container.h>
#include <game/server/gamecontext.h>
#include <game/server/item.h>

#include "container.h"

// maps the "display" field to a client pickup sprite, the same mapping the
// resource nodes use
static int ContainerPickupFromName(const char *pName)
{
	if(!pName || !pName[0])
		return PICKUP_ARMOR;
	if(str_comp_nocase(pName, "health") == 0)
		return PICKUP_HEALTH;
	if(str_comp_nocase(pName, "grenade") == 0)
		return PICKUP_GRENADE;
	if(str_comp_nocase(pName, "shotgun") == 0)
		return PICKUP_SHOTGUN;
	if(str_comp_nocase(pName, "laser") == 0)
		return PICKUP_LASER;
	if(str_comp_nocase(pName, "ninja") == 0)
		return PICKUP_NINJA;
	if(str_comp_nocase(pName, "gun") == 0)
		return PICKUP_GUN;
	if(str_comp_nocase(pName, "hammer") == 0)
		return PICKUP_HAMMER;
	return PICKUP_ARMOR;
}

namespace
{
// Deterministic xorshift32, so a generated map instance always rolls the same
// first loot; the roll is persisted afterwards.
struct SLootRandom
{
	unsigned m_State;
	explicit SLootRandom(unsigned Seed) :
		m_State(Seed ? Seed : 0x9e3779b9u) {}

	unsigned Next()
	{
		m_State ^= m_State << 13;
		m_State ^= m_State >> 17;
		m_State ^= m_State << 5;
		return m_State;
	}

	int Range(int Min, int Max)
	{
		if(Max <= Min)
			return Min;
		return Min + (int) (Next() % (unsigned) (Max - Min + 1));
	}
};

int LootJsonInt(const json_value &rValue, int Default)
{
	if(rValue.type == json_integer || rValue.type == json_double)
		return (int) (json_int_t) rValue;
	return Default;
}

int RarityFromName(const char *pName)
{
	if(!pName || !pName[0])
		return 0;
	if(str_comp_nocase(pName, "uncommon") == 0)
		return 1;
	if(str_comp_nocase(pName, "rare") == 0)
		return 2;
	if(str_comp_nocase(pName, "legendary") == 0)
		return 3;
	return 0;
}

const char *RarityName(int Rarity)
{
	switch(Rarity)
	{
	case 1: return "uncommon";
	case 2: return "rare";
	case 3: return "legendary";
	default: return "common";
	}
}
} // namespace

CContainerSystem::~CContainerSystem()
{
	for(int i = 0; i < m_lDefs.size(); i++)
		delete m_lDefs[i];
	m_lDefs.clear();
	m_Defs.clear();

	for(int i = 0; i < m_lLootTables.size(); i++)
		delete m_lLootTables[i];
	m_lLootTables.clear();
	m_LootTables.clear();
}

void CContainerSystem::Init(CGameContext *pGameServer, IStorage *pStorage)
{
	m_pGameServer = pGameServer;
	m_pStorage = pStorage;
	Load(pStorage);
}

void CContainerSystem::Load(IStorage *pStorage)
{
	m_pStorage = pStorage;

	// reloading is a full replace: drop the previous definitions first so a
	// stale kind cannot survive in the hash table
	for(int i = 0; i < m_lDefs.size(); i++)
		delete m_lDefs[i];
	m_lDefs.clear();
	m_Defs.clear();

	for(int i = 0; i < m_lLootTables.size(); i++)
		delete m_lLootTables[i];
	m_lLootTables.clear();
	m_LootTables.clear();

	if(!pStorage)
		return;

	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile("containers.json", pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
	{
		dbg_msg("containers", "no data/containers.json; container entities are unavailable");
		return;
	}

	const json_value &rList = (*pJson)["containers"];
	if(rList.type != json_array)
	{
		dbg_msg("containers", "data/containers.json has no \"containers\" array");
		return;
	}

	for(unsigned i = 0; i < rList.u.array.length; i++)
	{
		const json_value &rEntry = rList[i];
		if(rEntry.type != json_object)
			continue;

		const char *pKind = rEntry["kind"];
		if(!pKind || !pKind[0])
			continue;

		SContainerDef *pDef = new SContainerDef();
		mem_zero(pDef, sizeof(*pDef));
		str_copy(pDef->m_aKind, pKind, sizeof(pDef->m_aKind));
		str_copy(pDef->m_aName, rEntry["name"], sizeof(pDef->m_aName));
		if(!pDef->m_aName[0])
			str_copy(pDef->m_aName, pKind, sizeof(pDef->m_aName));

		// capacity is the number of distinct item kinds; one is the minimum a
		// container can be useful for, and the entity has a fixed slot array
		const json_value &rCapacity = rEntry["capacity"];
		int Capacity = 1;
		if(rCapacity.type == json_integer || rCapacity.type == json_double)
			Capacity = (int) (json_int_t) rCapacity;
		pDef->m_Capacity = clamp(Capacity, 1, (int) MAX_CONTAINER_SLOTS);
		pDef->m_Pickup = ContainerPickupFromName(rEntry["display"]);

		SContainerDef **ppExisting = m_Defs.get(str_quickhash(pDef->m_aKind));
		if(ppExisting && *ppExisting)
		{
			dbg_msg("containers", "duplicate container kind '%s', keeping the first", pDef->m_aKind);
			delete pDef;
			continue;
		}

		m_lDefs.add(pDef);
		m_Defs.set(str_quickhash(pDef->m_aKind), pDef);
	}

	dbg_msg("containers", "loaded %d container kind(s)", m_lDefs.size());

	LoadLoot(pStorage);
}

const SContainerDef *CContainerSystem::Find(const char *pKind) const
{
	if(!pKind || !pKind[0])
		return nullptr;
	const SContainerDef *const *ppDef = m_Defs.get(str_quickhash(pKind));
	if(!ppDef)
		return nullptr;
	// a hash collision is possible; confirm the kind before handing it out
	if(str_comp((*ppDef)->m_aKind, pKind) != 0)
		return nullptr;
	return *ppDef;
}

const SContainerDef *CContainerSystem::DefAt(int Index) const
{
	if(Index < 0 || Index >= m_lDefs.size())
		return nullptr;
	return m_lDefs[Index];
}

int CContainerSystem::ListLootFilesCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	(void) StorageType;
	if(IsDir)
		return 0;

	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp(pFilename + Len - 5, ".json") != 0)
		return 0;
	char aDefaultId[64];
	if(Len - 5 >= (int) sizeof(aDefaultId))
		return 0;
	str_copy(aDefaultId, pFilename, sizeof(aDefaultId));
	aDefaultId[Len - 5] = '\0';

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "loot/%s", pFilename);
	static_cast<CContainerSystem *>(pUser)->LoadLootTable(aDefaultId, aPath);
	return 0;
}

void CContainerSystem::LoadLoot(IStorage *pStorage)
{
	if(!pStorage)
		return;

	pStorage->ListDirectory(IStorage::TYPE_ALL, "loot", ListLootFilesCallback, this);
	dbg_msg("loot", "loaded %d loot table(s)", m_lLootTables.size());
}

void CContainerSystem::LoadLootTable(const char *pDefaultId, const char *pFilePath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pFilePath, m_pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
	{
		dbg_msg("loot", "failed to load loot table '%s'", pFilePath);
		return;
	}

	// The file's id wins; the file name is the default, so a content author can
	// copy a table under a new name without editing it.
	const char *pId = (*pJson)["id"];
	if(!pId || !pId[0])
		pId = pDefaultId;
	if(!pId || !pId[0])
		return;

	SContainerLootTable **ppExisting = m_LootTables.get(str_quickhash(pId));
	if(ppExisting && *ppExisting)
	{
		dbg_msg("loot", "duplicate loot table '%s', keeping the first", pId);
		return;
	}

	SContainerLootTable *pTable = new SContainerLootTable();
	str_copy(pTable->m_aId, pId, sizeof(pTable->m_aId));
	pTable->m_RollsMin = clamp(LootJsonInt((*pJson)["rolls_min"], 1), 0, (int) MAX_CONTAINER_SLOTS);
	pTable->m_RollsMax = clamp(LootJsonInt((*pJson)["rolls_max"], pTable->m_RollsMin),
		pTable->m_RollsMin, (int) MAX_CONTAINER_SLOTS);

	const CItemSystem *pItems = m_pGameServer ? m_pGameServer->Item() : nullptr;
	const json_value &rEntries = (*pJson)["entries"];
	for(unsigned e = 0; e < rEntries.u.array.length; e++)
	{
		const json_value &rLoot = rEntries[e];
		if(rLoot.type != json_object)
			continue;

		const char *pItem = rLoot["item"];
		if(!pItem || !pItem[0])
			continue;
		const CItemID Id = CItemID::FromName(pItem);
		if(pItems && !pItems->IsKnownItem(Id))
		{
			dbg_msg("loot", "table '%s': unknown item '%s', entry skipped", pId, pItem);
			continue;
		}

		SContainerLootEntry &Loot = pTable->m_lEntries.emplace();
		Loot.m_Id = Id;
		str_copy(Loot.m_aItem, pItem, sizeof(Loot.m_aItem));
		Loot.m_Weight = clamp(LootJsonInt(rLoot["weight"], 1), 0, 1000000);
		if(Loot.m_Weight <= 0)
		{
			pTable->m_lEntries.remove_index(pTable->m_lEntries.size() - 1);
			continue;
		}

		Loot.m_CountMin = maximum(1, LootJsonInt(rLoot["count_min"], 1));
		Loot.m_CountMax = maximum(Loot.m_CountMin, LootJsonInt(rLoot["count_max"], Loot.m_CountMin));
		Loot.m_CountMin = minimum(Loot.m_CountMin, 10000);
		Loot.m_CountMax = minimum(Loot.m_CountMax, 10000);
		Loot.m_Rarity = RarityFromName(rLoot["rarity"]);
	}

	m_lLootTables.add(pTable);
	m_LootTables.set(str_quickhash(pTable->m_aId), pTable);
}

const SContainerLootTable *CContainerSystem::FindLootTable(const char *pId) const
{
	if(!pId || !pId[0])
		return nullptr;
	const SContainerLootTable *const *ppTable = m_LootTables.get(str_quickhash(pId));
	if(!ppTable)
		return nullptr;
	if(str_comp((*ppTable)->m_aId, pId) != 0)
		return nullptr;
	return *ppTable;
}

void CContainerSystem::FillLoot(CContainerEntity *pContainer, unsigned Seed) const
{
	if(!pContainer)
		return;

	// Mark first: an empty result is a real result, and a world save must be
	// able to tell "emptied by players" from "never rolled".
	pContainer->SetInitialized(true);
	if(pContainer->UsedSlots() > 0)
		return;

	const SContainerLootTable *pTable = FindLootTable(pContainer->LootTable());
	if(!pTable || !pTable->m_lEntries.size())
		return;

	// Roll without replacement, so a container never repeats the same item
	// kind in two slots; the capacity caps the number of rolls again.
	array<int> lEligible;
	lEligible.hint_size(pTable->m_lEntries.size());
	for(int i = 0; i < pTable->m_lEntries.size(); i++)
	{
		if(pTable->m_lEntries[i].m_Weight > 0)
			lEligible.add(i);
	}

	SLootRandom Random(Seed);
	int Rolls = Random.Range(pTable->m_RollsMin, pTable->m_RollsMax);
	Rolls = minimum(Rolls, lEligible.size());
	Rolls = minimum(Rolls, pContainer->Capacity());

	for(int r = 0; r < Rolls; r++)
	{
		int TotalWeight = 0;
		for(int e = 0; e < lEligible.size(); e++)
			TotalWeight += pTable->m_lEntries[lEligible[e]].m_Weight;
		if(TotalWeight <= 0)
			break;

		int Pick = Random.Range(0, TotalWeight - 1);
		int Chosen = -1;
		for(int e = 0; e < lEligible.size(); e++)
		{
			Pick -= pTable->m_lEntries[lEligible[e]].m_Weight;
			if(Pick < 0)
			{
				Chosen = e;
				break;
			}
		}
		if(Chosen < 0)
			break;

		const SContainerLootEntry &Loot = pTable->m_lEntries[lEligible[Chosen]];
		const int Count = Random.Range(Loot.m_CountMin, Loot.m_CountMax);
		if(!pContainer->Store(Loot.m_Id, Count))
			break;
		lEligible.remove_index(Chosen);

		dbg_msg("loot", "container %d rolled %s x%d [%s]",
			pContainer->StorageID(), Loot.m_aItem, Count, RarityName(Loot.m_Rarity));
	}
}
