#include <game/server/item.h>

#include <base/system.h>
#include <engine/shared/jsonparser.h>
#include <engine/storage.h>

#include <game/server/gamecontext.h>

// ---------------------------------------------------------------
// CItemSystem::CInventory
// ---------------------------------------------------------------
CItemSystem::CInventory::CInventory()
{
	m_NumItems = 0;
	mem_zero(m_aItems, sizeof(m_aItems));
}

int CItemSystem::CInventory::Find(const char *pResId) const
{
	for(int i = 0; i < m_NumItems; i++)
		if(str_comp(m_aItems[i].m_aResId, pResId) == 0)
			return i;
	return -1;
}

int CItemSystem::CInventory::Get(const char *pResId) const
{
	const int Index = Find(pResId);
	return Index >= 0 ? m_aItems[Index].m_Count : 0;
}

// ---------------------------------------------------------------
// CItemSystem
// ---------------------------------------------------------------
int CItemSystem::ListItemsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	CItemSystem *pSelf = static_cast<CItemSystem *>(pUser);
	if(IsDir)
		return 0;

	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp(pFilename + Len - 5, ".json") != 0)
		return 0;

	// the res_id is the file name without the ".json" suffix
	char aResId[64];
	str_copy(aResId, pFilename, sizeof(aResId));
	aResId[Len - 5] = '\0';

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "items/%s", pFilename);
	pSelf->LoadItem(aResId, aPath);
	return 0;
}

void CItemSystem::LoadItem(const char *pResId, const char *pFilePath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pFilePath, m_pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
		return;

	SItemDef Item;
	str_copy(Item.m_aName, (*pJson)["name"], sizeof(Item.m_aName));
	const char *pDesc = (*pJson)["desc"];
	if(!pDesc[0])
		pDesc = (*pJson)["Desc"];
	str_copy(Item.m_aDesc, pDesc, sizeof(Item.m_aDesc));

	// item_type: a single string or an array of strings
	Item.m_NumTypes = 0;
	const json_value &Types = (*pJson)["item_type"];
	if(Types.type == json_string && ((const char *)Types)[0])
	{
		str_copy(Item.m_aTypes[Item.m_NumTypes++], (const char *)Types, sizeof(Item.m_aTypes[0]));
	}
	else if(Types.type == json_array)
	{
		for(unsigned i = 0; i < Types.u.array.length && Item.m_NumTypes < (int)SItemDef::MAX_TYPES; i++)
		{
			const json_value &Type = Types[i];
			if(Type.type == json_string && ((const char *)Type)[0])
				str_copy(Item.m_aTypes[Item.m_NumTypes++], (const char *)Type, sizeof(Item.m_aTypes[0]));
		}
	}

	m_Items.set(str_quickhash(pResId), Item);
	dbg_msg("items", "loaded item '%s' ('%s')", pResId, Item.m_aName);
}

void CItemSystem::Init(CGameContext *pGameServer, IStorage *pStorage)
{
	m_pGameServer = pGameServer;
	Load(pStorage);
}

void CItemSystem::Load(IStorage *pStorage)
{
	m_pStorage = pStorage;
	m_Items.clear();
	m_Crafts.clear();
	m_pStorage->ListDirectory(IStorage::TYPE_ALL, "items", ListItemsCallback, this);
	m_pStorage->ListDirectory(IStorage::TYPE_ALL, "craft", ListCraftsCallback, this);
}

bool CItemSystem::AddItem(int ClientID, const char *pResId, int Count, bool SilentFail)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return false;

	CInventory &Inventory = m_aInventories[ClientID];
	int Index = Inventory.Find(pResId);
	if(Index < 0)
	{
		if(Inventory.m_NumItems >= CInventory::MAX_ITEMS)
		{
			// no free slot: don't insert; tell the player unless silent
			if(!SilentFail)
				m_pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("No inventory space!", "Item Pickup"));
			return false;
		}
		Index = Inventory.m_NumItems++;
		str_copy(Inventory.m_aItems[Index].m_aResId, pResId, sizeof(Inventory.m_aItems[Index].m_aResId));
		Inventory.m_aItems[Index].m_Count = 0;
	}
	Inventory.m_aItems[Index].m_Count += Count;

	// broadcast the pickup to the player
	const char *pName = GetName(pResId);
	char aMsg[128];
	str_format(aMsg, sizeof(aMsg), Localize("You got: %s x%d (%d)", "Item Pickup"),
		Localize(pName, "Item Name"), Count, Inventory.m_aItems[Index].m_Count);
	m_pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, aMsg);
	return true;
}

const char *CItemSystem::GetName(const char *pResId) const
{
	const SItemDef *pItem = m_Items.get(str_quickhash(pResId));
	return pItem ? pItem->m_aName : pResId;
}

const char *CItemSystem::GetDesc(const char *pResId) const
{
	const SItemDef *pItem = m_Items.get(str_quickhash(pResId));
	return pItem ? pItem->m_aDesc : "";
}

bool CItemSystem::HasItemType(const char *pResId, const char *pType) const
{
	const SItemDef *pItem = m_Items.get(str_quickhash(pResId));
	if(!pItem)
		return false;
	for(int i = 0; i < pItem->m_NumTypes; i++)
		if(str_comp(pItem->m_aTypes[i], pType) == 0)
			return true;
	return false;
}

int CItemSystem::GetIngredientCount(int ClientID, const SIngredient &Need) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return 0;

	const CInventory &Inventory = m_aInventories[ClientID];
	if(!Need.m_MatchByType)
		return Inventory.Get(Need.m_aItemId);

	int Total = 0;
	for(int i = 0; i < Inventory.m_NumItems; i++)
		if(HasItemType(Inventory.m_aItems[i].m_aResId, Need.m_aType))
			Total += Inventory.m_aItems[i].m_Count;
	return Total;
}

// ---------------------------------------------------------------
// crafting
// ---------------------------------------------------------------
int CItemSystem::ListCraftsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	CItemSystem *pSelf = static_cast<CItemSystem *>(pUser);
	if(IsDir)
		return 0;

	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp(pFilename + Len - 5, ".json") != 0)
		return 0;

	// the craft_id is the file name without the ".json" suffix
	char aCraftId[64];
	str_copy(aCraftId, pFilename, sizeof(aCraftId));
	aCraftId[Len - 5] = '\0';

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "craft/%s", pFilename);
	pSelf->LoadCraft(aCraftId, aPath);
	return 0;
}

void CItemSystem::LoadCraft(const char *pCraftId, const char *pFilePath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pFilePath, m_pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
		return;

	SCraftDef Craft;
	str_copy(Craft.m_aCraftId, pCraftId, sizeof(Craft.m_aCraftId));
	Craft.m_ResultCount = 1;
	Craft.m_NumNeeded = 0;

	const json_value &Result = (*pJson)["result"];
	str_copy(Craft.m_aResultItemId, (const char *)Result["item_id"], sizeof(Craft.m_aResultItemId));
	const json_value &ResultCount = Result["count"];
	if(ResultCount.type == json_integer || ResultCount.type == json_double)
		Craft.m_ResultCount = (int)(json_int_t)ResultCount;

	// one or more ingredients in "needed"
	const json_value &Needed = (*pJson)["needed"];
	const int NumIng = (Needed.type == json_array) ? (int)Needed.u.array.length : ((Needed.type == json_object) ? 1 : 0);
	for(int i = 0; i < NumIng && Craft.m_NumNeeded < (int)(sizeof(Craft.m_aNeeded) / sizeof(Craft.m_aNeeded[0])); i++)
	{
		const json_value &Ing = (Needed.type == json_array) ? Needed[i] : Needed;
		SIngredient &Need = Craft.m_aNeeded[Craft.m_NumNeeded];
		// by default match an exact item id
		str_copy(Need.m_aItemId, (const char *)Ing["item_id"], sizeof(Need.m_aItemId));
		Need.m_aType[0] = '\0';
		Need.m_MatchByType = false;
		// if a "type" is given, match by item_type instead of an exact id
		const json_value &TypeVal = Ing["type"];
		if(TypeVal.type == json_string && ((const char *)TypeVal)[0])
		{
			Need.m_MatchByType = true;
			str_copy(Need.m_aType, (const char *)TypeVal, sizeof(Need.m_aType));
		}
		Need.m_Count = 1;
		const json_value &Count = Ing["count"];
		if(Count.type == json_integer || Count.type == json_double)
			Need.m_Count = (int)(json_int_t)Count;
		const json_value &Tool = Ing["tool"];
		Need.m_IsTool = (Tool.type == json_boolean) && Tool.u.boolean != 0;
		Craft.m_NumNeeded++;
	}

	m_Crafts.set(str_quickhash(pCraftId), Craft);
	dbg_msg("craft", "loaded craft '%s' -> %s x%d (%d ingredient(s))",
		pCraftId, Craft.m_aResultItemId, Craft.m_ResultCount, Craft.m_NumNeeded);
}

void CItemSystem::ForEachCraft(FCraftCallback pfnFunc, void *pUser)
{
	m_Crafts.for_each((hash_table<unsigned, SCraftDef, 8>::foreach_function)pfnFunc, pUser);
}

const CItemSystem::SCraftDef *CItemSystem::GetCraft(const char *pCraftId) const
{
	return m_Crafts.get(str_quickhash(pCraftId));
}

bool CItemSystem::ReserveIngredients(int ClientID, const SCraftDef *pCraft, int *pTake) const
{
	if(!pCraft || ClientID < 0 || ClientID >= MAX_CLIENTS)
		return false;

	const CInventory &Inventory = m_aInventories[ClientID];

	// available (still unreserved) quantity per inventory slot
	int aAvail[CInventory::MAX_ITEMS];
	for(int i = 0; i < Inventory.m_NumItems; i++)
		aAvail[i] = Inventory.m_aItems[i].m_Count;

	// try to satisfy each ingredient against the same shared availability, so a
	// single physical item can never be used by two different materials
	for(int i = 0; i < pCraft->m_NumNeeded; i++)
	{
		const SIngredient &Need = pCraft->m_aNeeded[i];

		if(Need.m_IsTool)
		{
			// a tool must simply be owned; it is never consumed
			bool Found = false;
			for(int k = 0; k < Inventory.m_NumItems && !Found; k++)
			{
				if(aAvail[k] <= 0)
					continue;
				if(Need.m_MatchByType)
					Found = HasItemType(Inventory.m_aItems[k].m_aResId, Need.m_aType);
				else
					Found = str_comp(Inventory.m_aItems[k].m_aResId, Need.m_aItemId) == 0;
			}
			if(!Found)
				return false;
			continue;
		}

		int Remaining = Need.m_Count;
		for(int k = 0; k < Inventory.m_NumItems && Remaining > 0; k++)
		{
			if(aAvail[k] <= 0)
				continue;
			if(Need.m_MatchByType)
			{
				if(!HasItemType(Inventory.m_aItems[k].m_aResId, Need.m_aType))
					continue;
			}
			else
			{
				if(str_comp(Inventory.m_aItems[k].m_aResId, Need.m_aItemId) != 0)
					continue;
			}
			const int Take = minimum(Remaining, aAvail[k]);
			aAvail[k] -= Take;
			Remaining -= Take;
		}
		if(Remaining > 0)
			return false;
	}

	// report what to remove per slot
	for(int k = 0; k < Inventory.m_NumItems; k++)
		pTake[k] = Inventory.m_aItems[k].m_Count - aAvail[k];
	return true;
}

bool CItemSystem::HasIngredients(int ClientID, const SCraftDef *pCraft) const
{
	int aTake[CInventory::MAX_ITEMS];
	return ReserveIngredients(ClientID, pCraft, aTake);
}

CItemSystem::ECraftResult CItemSystem::Craft(int ClientID, const char *pCraftId)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return CRAFT_NO_MATERIALS;

	SCraftDef *pCraft = m_Crafts.get(str_quickhash(pCraftId));
	if(!pCraft)
		return CRAFT_NO_MATERIALS;

	// separate the failure reasons: missing materials vs. no room for the result
	if(!HasIngredients(ClientID, pCraft))
		return CRAFT_NO_MATERIALS;

	CInventory &Inventory = m_aInventories[ClientID];
	if(Inventory.Find(pCraft->m_aResultItemId) < 0 &&
		Inventory.m_NumItems >= CInventory::MAX_ITEMS)
		return CRAFT_NO_SPACE;

	// consume the reserved non-tool ingredients; tools are kept
	int aTake[CInventory::MAX_ITEMS] = {0};
	if(!ReserveIngredients(ClientID, pCraft, aTake))
		return CRAFT_NO_MATERIALS;
	for(int k = 0; k < Inventory.m_NumItems; k++)
		if(aTake[k] > 0)
			Inventory.m_aItems[k].m_Count -= aTake[k];

	// the slot was already checked above, so a silent add cannot fail again
	AddItem(ClientID, pCraft->m_aResultItemId, pCraft->m_ResultCount, true);
	return CRAFT_OK;
}
