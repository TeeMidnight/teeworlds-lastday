#include <game/server/item.h>

#include <base/system.h>
#include <engine/shared/jsonparser.h>
#include <engine/storage.h>

#include <game/server/gamecontext.h>

// ---------------------------------------------------------------
// CItemSystem::CInventory (inventory / item loading lives here; use effects
// and crafting live in item_use.cpp / item_craft.cpp)
// ---------------------------------------------------------------
CItemSystem::CInventory::CInventory()
{
	mem_zero(m_aItems, sizeof(m_aItems));
}

int CItemSystem::CInventory::Find(const char *pResId) const
{
	for(int i = 0; i < MAX_ITEMS; i++)
		if(m_aItems[i].m_aResId[0] != '\0' && str_comp(m_aItems[i].m_aResId, pResId) == 0)
			return i;
	return -1;
}

int CItemSystem::CInventory::FindEmpty() const
{
	for(int i = 0; i < MAX_ITEMS; i++)
		if(m_aItems[i].m_aResId[0] == '\0')
			return i;
	return -1;
}

int CItemSystem::CInventory::Get(const char *pResId) const
{
	const int Index = Find(pResId);
	return Index >= 0 ? m_aItems[Index].m_Count : 0;
}

void CItemSystem::CInventory::ClearSlot(int Index)
{
	if(Index >= 0 && Index < MAX_ITEMS)
	{
		m_aItems[Index].m_aResId[0] = '\0';
		m_aItems[Index].m_Count = 0;
	}
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
	str_copy(Item.m_aResId, pResId, sizeof(Item.m_aResId));
	str_copy(Item.m_aName, (*pJson)["name"], sizeof(Item.m_aName));
	const char *pDesc = (*pJson)["desc"];
	if(!pDesc[0])
		pDesc = (*pJson)["Desc"];
	str_copy(Item.m_aDesc, pDesc, sizeof(Item.m_aDesc));

	// item_type: a single string or an array of strings
	Item.m_NumTypes = 0;
	const json_value &Types = (*pJson)["item_type"];
	if(Types.type == json_string && ((const char *) Types)[0])
	{
		str_copy(Item.m_aTypes[Item.m_NumTypes++], (const char *) Types, sizeof(Item.m_aTypes[0]));
	}
	else if(Types.type == json_array)
	{
		for(unsigned i = 0; i < Types.u.array.length && Item.m_NumTypes < (int) SItemDef::MAX_TYPES; i++)
		{
			const json_value &Type = Types[i];
			if(Type.type == json_string && ((const char *) Type)[0])
				str_copy(Item.m_aTypes[Item.m_NumTypes++], (const char *) Type, sizeof(Item.m_aTypes[0]));
		}
	}

	// ammo_for: which weapons this item can be used as ammo for (a single
	// string or an array of strings). an item with at least one entry is an
	// ammo item.
	Item.m_NumAmmoFor = 0;
	const json_value &AmmoFor = (*pJson)["ammo_for"];
	if(AmmoFor.type == json_string && ((const char *) AmmoFor)[0])
	{
		str_copy(Item.m_aAmmoFor[Item.m_NumAmmoFor++], (const char *) AmmoFor, sizeof(Item.m_aAmmoFor[0]));
	}
	else if(AmmoFor.type == json_array)
	{
		for(unsigned i = 0; i < AmmoFor.u.array.length && Item.m_NumAmmoFor < (int) SItemDef::MAX_AMMO_FOR; i++)
		{
			const json_value &Weapon = AmmoFor[i];
			if(Weapon.type == json_string && ((const char *) Weapon)[0])
				str_copy(Item.m_aAmmoFor[Item.m_NumAmmoFor++], (const char *) Weapon, sizeof(Item.m_aAmmoFor[0]));
		}
	}

	// damage dealt when used as ammo (0 = use the weapon's default)
	Item.m_Damage = 0;
	const json_value &Damage = (*pJson)["damage"];
	if(Damage.type == json_integer || Damage.type == json_double)
		Item.m_Damage = (int) (json_int_t) Damage;

	// use effect: an optional "use" object declares what happens when the
	// player uses the item from the inventory menu (consumes one)
	const json_value &Use = (*pJson)["use"];
	if(Use.type == json_object)
	{
		Item.m_Use.m_HasUse = true;
		Item.m_Use.m_Health = 0;
		Item.m_Use.m_Sanity = 0;
		const json_value &Health = Use["health"];
		if(Health.type == json_integer || Health.type == json_double)
			Item.m_Use.m_Health = (int) (json_int_t) Health;
		const json_value &Sanity = Use["sanity"];
		if(Sanity.type == json_integer || Sanity.type == json_double)
			Item.m_Use.m_Sanity = (int) (json_int_t) Sanity;
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
		Index = Inventory.FindEmpty();
		if(Index < 0)
		{
			// no free slot: don't insert; tell the player unless silent
			if(!SilentFail)
				m_pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("No inventory space!", "Item Pickup"));
			return false;
		}
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
	for(int i = 0; i < CInventory::MAX_ITEMS; i++)
	{
		if(Inventory.IsEmpty(i))
			continue;
		if(HasItemType(Inventory.m_aItems[i].m_aResId, Need.m_aType))
			Total += Inventory.m_aItems[i].m_Count;
	}
	return Total;
}

bool CItemSystem::HasItem(int ClientID, const char *pResId) const
{
	return GetItemCount(ClientID, pResId) > 0;
}

int CItemSystem::GetItemCount(int ClientID, const char *pResId) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return 0;
	return m_aInventories[ClientID].Get(pResId);
}

bool CItemSystem::RemoveItem(int ClientID, const char *pResId, int Count)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || Count <= 0)
		return false;

	CInventory &Inventory = m_aInventories[ClientID];
	const int Index = Inventory.Find(pResId);
	if(Index < 0 || Inventory.m_aItems[Index].m_Count < Count)
		return false;

	Inventory.m_aItems[Index].m_Count -= Count;

	// the stack is gone: clear the slot instead of moving the others
	if(Inventory.m_aItems[Index].m_Count <= 0)
		Inventory.ClearSlot(Index);
	return true;
}

bool CItemSystem::HasItemHash(int ClientID, unsigned Hash) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return false;
	const CInventory &Inventory = m_aInventories[ClientID];
	for(int i = 0; i < CInventory::MAX_ITEMS; i++)
	{
		if(Inventory.IsEmpty(i))
			continue;
		if(str_quickhash(Inventory.m_aItems[i].m_aResId) == Hash)
			return true;
	}
	return false;
}

const char *CItemSystem::GetResIdByHash(int ClientID, unsigned Hash) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return nullptr;
	const CInventory &Inventory = m_aInventories[ClientID];
	for(int i = 0; i < CInventory::MAX_ITEMS; i++)
	{
		if(Inventory.IsEmpty(i))
			continue;
		if(str_quickhash(Inventory.m_aItems[i].m_aResId) == Hash)
			return Inventory.m_aItems[i].m_aResId;
	}
	return nullptr;
}

// callback used by WeaponNeedsAmmo to scan every item definition
struct SAmmoScanData
{
	const char *m_pWeaponName;
	bool m_Found;
};

void CItemSystem::AmmoScanCallback(SItemDef &Item, void *pUser)
{
	SAmmoScanData *pData = static_cast<SAmmoScanData *>(pUser);
	for(int k = 0; k < Item.m_NumAmmoFor; k++)
		if(str_comp(Item.m_aAmmoFor[k], pData->m_pWeaponName) == 0)
			pData->m_Found = true;
}

bool CItemSystem::WeaponNeedsAmmo(const char *pWeaponName)
{
	// a weapon needs ammo when at least one item definition is usable as its
	// ammo (declared through "ammo_for")
	SAmmoScanData Data = {pWeaponName, false};
	m_Items.for_each(AmmoScanCallback, &Data);
	return Data.m_Found;
}

int CItemSystem::GetAmmoCountForWeapon(int ClientID, const char *pWeaponName) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return 0;

	const CInventory &Inventory = m_aInventories[ClientID];
	int Total = 0;
	for(int i = 0; i < CInventory::MAX_ITEMS; i++)
	{
		if(Inventory.IsEmpty(i))
			continue;
		const SItemDef *pItem = m_Items.get(str_quickhash(Inventory.m_aItems[i].m_aResId));
		if(!pItem)
			continue;
		for(int k = 0; k < pItem->m_NumAmmoFor; k++)
			if(str_comp(pItem->m_aAmmoFor[k], pWeaponName) == 0)
			{
				Total += Inventory.m_aItems[i].m_Count;
				break;
			}
	}
	return Total;
}

int CItemSystem::ConsumeAmmoForWeapon(int ClientID, const char *pWeaponName)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return 0;

	CInventory &Inventory = m_aInventories[ClientID];
	for(int i = 0; i < CInventory::MAX_ITEMS; i++)
	{
		if(Inventory.IsEmpty(i))
			continue;
		const SItemDef *pItem = m_Items.get(str_quickhash(Inventory.m_aItems[i].m_aResId));
		if(!pItem)
			continue;
		for(int k = 0; k < pItem->m_NumAmmoFor; k++)
		{
			if(str_comp(pItem->m_aAmmoFor[k], pWeaponName) == 0 && Inventory.m_aItems[i].m_Count > 0)
			{
				// consume one round; RemoveItem clears the slot when the
				// stack reaches zero (it does not move other slots)
				const int Damage = pItem->m_Damage;
				RemoveItem(ClientID, Inventory.m_aItems[i].m_aResId, 1);
				return Damage;
			}
		}
	}
	return 0;
}

// callback used by AddAmmoForWeapon to find the first ammo item for a weapon
struct SAmmoAddData
{
	const char *m_pWeaponName;
	char m_aResId[32];
	bool m_Found;
};

void CItemSystem::AmmoAddCallback(CItemSystem::SItemDef &Item, void *pUser)
{
	SAmmoAddData *pData = static_cast<SAmmoAddData *>(pUser);
	if(pData->m_Found)
		return;
	for(int k = 0; k < Item.m_NumAmmoFor; k++)
	{
		if(str_comp(Item.m_aAmmoFor[k], pData->m_pWeaponName) == 0)
		{
			str_copy(pData->m_aResId, Item.m_aResId, sizeof(pData->m_aResId));
			pData->m_Found = true;
			return;
		}
	}
}

void CItemSystem::AddAmmoForWeapon(int ClientID, const char *pWeaponName, int Count)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || Count <= 0)
		return;

	// add to the first ammo item definition usable by this weapon
	SAmmoAddData Data = {pWeaponName, {0}, false};
	m_Items.for_each(AmmoAddCallback, &Data);
	if(Data.m_Found)
		AddItem(ClientID, Data.m_aResId, Count, true);
}

// ---------------------------------------------------------------
// craft loading
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
	str_copy(Craft.m_aResultItemId, (const char *) Result["item_id"], sizeof(Craft.m_aResultItemId));
	const json_value &ResultCount = Result["count"];
	if(ResultCount.type == json_integer || ResultCount.type == json_double)
		Craft.m_ResultCount = (int) (json_int_t) ResultCount;

	// one or more ingredients in "needed"
	const json_value &Needed = (*pJson)["needed"];
	const int NumIng = (Needed.type == json_array) ? (int) Needed.u.array.length : ((Needed.type == json_object) ? 1 : 0);
	for(int i = 0; i < NumIng && Craft.m_NumNeeded < (int) (sizeof(Craft.m_aNeeded) / sizeof(Craft.m_aNeeded[0])); i++)
	{
		const json_value &Ing = (Needed.type == json_array) ? Needed[i] : Needed;
		SIngredient &Need = Craft.m_aNeeded[Craft.m_NumNeeded];
		// by default match an exact item id
		str_copy(Need.m_aItemId, (const char *) Ing["item_id"], sizeof(Need.m_aItemId));
		Need.m_aType[0] = '\0';
		Need.m_MatchByType = false;
		// if a "type" is given, match by item_type instead of an exact id
		const json_value &TypeVal = Ing["type"];
		if(TypeVal.type == json_string && ((const char *) TypeVal)[0])
		{
			Need.m_MatchByType = true;
			str_copy(Need.m_aType, (const char *) TypeVal, sizeof(Need.m_aType));
		}
		Need.m_Count = 1;
		const json_value &Count = Ing["count"];
		if(Count.type == json_integer || Count.type == json_double)
			Need.m_Count = (int) (json_int_t) Count;
		const json_value &Tool = Ing["tool"];
		Need.m_IsTool = (Tool.type == json_boolean) && Tool.u.boolean != 0;
		Craft.m_NumNeeded++;
	}

	m_Crafts.set(str_quickhash(pCraftId), Craft);
	dbg_msg("craft", "loaded craft '%s' -> %s x%d (%d ingredient(s))",
		pCraftId, Craft.m_aResultItemId, Craft.m_ResultCount, Craft.m_NumNeeded);
}
