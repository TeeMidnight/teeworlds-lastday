/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/item.h>

#include <base/system.h>
#include <engine/shared/jsonparser.h>
#include <engine/storage.h>

#include <game/server/gamecontext.h>

#include <game/localization_format.h>

// ---------------------------------------------------------------
// CItemSystem::CInventory (inventory / item loading lives here; use effects
// and crafting live in item_use.cpp / item_craft.cpp)
// ---------------------------------------------------------------
CItemSystem::CInventory::CInventory()
{
	mem_zero(m_aItems, sizeof(m_aItems));
}

int CItemSystem::CInventory::Find(const CItemID &Id) const
{
	for(int i = 0; i < MAX_ITEMS; i++)
		if(m_aItems[i].m_Id == Id)
			return i;
	return -1;
}

int CItemSystem::CInventory::FindEmpty() const
{
	for(int i = 0; i < MAX_ITEMS; i++)
		if(m_aItems[i].m_Id.IsZero())
			return i;
	return -1;
}

int CItemSystem::CInventory::Get(const CItemID &Id) const
{
	const int Index = Find(Id);
	return Index >= 0 ? m_aItems[Index].m_Count : 0;
}

void CItemSystem::CInventory::ClearSlot(int Index)
{
	if(Index >= 0 && Index < MAX_ITEMS)
	{
		m_aItems[Index].m_Id = CItemID();
		m_aItems[Index].m_Count = 0;
	}
}

// ---------------------------------------------------------------
// CItemSystem
// ---------------------------------------------------------------
namespace {
// the "local" part of a namespaced id: "lastday:item/stone" -> "stone"
void LocalItemName(const char *pId, char *pOut, int OutSize)
{
	const char *pName = pId ? pId : "";
	const char *pColon = str_find(pName, ":");
	if(pColon)
		pName = pColon + 1;
	const char *pSlash = str_find(pName, "/");
	if(pSlash)
		pName = pSlash + 1;
	str_copy(pOut, pName, OutSize);
}
} // namespace

int CItemSystem::ListItemsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	CItemSystem *pSelf = static_cast<CItemSystem *>(pUser);
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
	str_format(aPath, sizeof(aPath), "items/%s", pFilename);
	pSelf->LoadItem(aDefaultId, aPath);
	return 0;
}

void CItemSystem::LoadItem(const char *pDefaultId, const char *pFilePath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pFilePath, m_pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
		return;

	// the id can be namespaced in the json ("base:stone"); the file name is the
	// default so existing content keeps working
	const char *pIdName = (*pJson)["id"];
	if(!pIdName[0])
		pIdName = pDefaultId;

	SItemDef Item;
	mem_zero(&Item, sizeof(Item));
	Item.m_Id = CItemID::FromName(pIdName);
	str_copy(Item.m_aIdString, Item.m_Id.SourceName(), sizeof(Item.m_aIdString));
	if(!Item.m_aIdString[0])
		str_copy(Item.m_aIdString, pIdName, sizeof(Item.m_aIdString));

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

	// ammo_for: which weapons this item can be used as ammo for (single string
	// or array). an item with at least one entry is an ammo item.
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

	// ammo effect: different ammo items for the same weapon can behave
	// differently (damage, projectile count, speed, lifetime, effect tag)
	auto ReadInt = [&](const char *pKey, int Default) -> int {
		const json_value &Value = (*pJson)[pKey];
		if(Value.type == json_integer || Value.type == json_double)
			return (int) (json_int_t) Value;
		return Default;
	};
	auto ReadFloat = [&](const char *pKey, float Default) -> float {
		const json_value &Value = (*pJson)[pKey];
		if(Value.type == json_integer || Value.type == json_double)
			return (float) (double) Value;
		return Default;
	};
	Item.m_AmmoEffect.m_Damage = ReadInt("damage", 0);
	Item.m_AmmoEffect.m_Projectiles = ReadInt("projectiles", 0);
	Item.m_AmmoEffect.m_Speed = ReadFloat("speed", 1.0f);
	Item.m_AmmoEffect.m_Lifetime = ReadFloat("lifetime", 1.0f);
	Item.m_AmmoEffect.m_Effect = ReadInt("effect", 0);

	// weapon mapping: an explicit "weapon" name, or the local id when the item
	// carries the "weapon" type (so "sword" maps to the sword weapon)
	const char *pWeapon = (*pJson)["weapon"];
	if(pWeapon[0])
		str_copy(Item.m_aWeapon, pWeapon, sizeof(Item.m_aWeapon));
	else
	{
		bool IsWeapon = false;
		for(int i = 0; i < Item.m_NumTypes; i++)
			if(str_comp(Item.m_aTypes[i], "weapon") == 0)
				IsWeapon = true;
		if(IsWeapon)
			LocalItemName(Item.m_aIdString, Item.m_aWeapon, sizeof(Item.m_aWeapon));
	}

	// use effect: an optional "use" object
	const json_value &Use = (*pJson)["use"];
	if(Use.type == json_object)
	{
		Item.m_Use.m_HasUse = true;
		Item.m_Use.m_Health = ReadInt("health", 0);
		// "health" lives inside the use object here, so read it directly
		const json_value &Health = Use["health"];
		if(Health.type == json_integer || Health.type == json_double)
			Item.m_Use.m_Health = (int) (json_int_t) Health;
		const json_value &Forgetting = Use["forgetting"];
		if(Forgetting.type == json_integer || Forgetting.type == json_double)
			Item.m_Use.m_Forgetting = (int) (json_int_t) Forgetting;
	}

	// accessory effect: an optional "accessory" object. only items that declare
	// one can be worn in the character's accessory loadout slots. the values are
	// deliberately clamped: content should never be able to grant an unbounded
	// health or speed bonus.
	const json_value &Accessory = (*pJson)["accessory"];
	if(Accessory.type == json_object)
	{
		auto ReadAccessoryFloat = [&](const char *pKey, float Default) -> float {
			const json_value &Value = Accessory[pKey];
			if(Value.type == json_integer || Value.type == json_double)
				return (float) (double) Value;
			return Default;
		};
		Item.m_Accessory.m_HasAccessory = true;
		Item.m_Accessory.m_Health = clamp((int) ReadAccessoryFloat("health", 0), 0, 10);
		Item.m_Accessory.m_Range = clamp(ReadAccessoryFloat("range", 0.0f), 0.0f, 1.0f);
		Item.m_Accessory.m_Speed = clamp(ReadAccessoryFloat("speed", 0.0f), 0.0f, 0.5f);
	}

	// drill placement: an optional "drill" object. the stored length is the
	// furthest the beam may bore, clamped so a data pack cannot reach across the
	// whole map.
	const json_value &Drill = (*pJson)["drill"];
	if(Drill.type == json_object)
	{
		Item.m_Drill.m_HasDrill = true;
		Item.m_Drill.m_MaxLength = 320;
		const json_value &Length = Drill["max_length"];
		if(Length.type == json_integer || Length.type == json_double)
			Item.m_Drill.m_MaxLength = clamp((int) (json_int_t) Length, 64, 2048);
	}

	// a UUID collision is astronomically unlikely, but a duplicated id must not
	// silently replace an unrelated item
	const SItemDef *pExisting = m_Items.get(Item.m_Id.Value());
	if(pExisting && str_comp(pExisting->m_aIdString, Item.m_aIdString) != 0)
	{
		dbg_msg("items", "id collision: '%s' collides with '%s', keeping the first", Item.m_aIdString, pExisting->m_aIdString);
		return;
	}
	m_Items.set(Item.m_Id.Value(), Item);
	// make the id printable even before the item system is queried
	ItemIDRegisterName(Item.m_Id, Item.m_aIdString, Item.m_aName);
	dbg_msg("items", "loaded item '%s' ('%s')", Item.m_aIdString, Item.m_aName);
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

bool CItemSystem::AddToInventory(CInventory &Inventory, const CItemID &Id, int Count, const SItemDef *pDef)
{
	(void) pDef;
	if(Id.IsZero() || Count <= 0)
		return false;

	int Index = Inventory.Find(Id);
	if(Index < 0)
	{
		Index = Inventory.FindEmpty();
		if(Index < 0)
			return false;
		Inventory.m_aItems[Index].m_Id = Id;
		Inventory.m_aItems[Index].m_Count = 0;
	}
	Inventory.m_aItems[Index].m_Count += Count;
	return true;
}

bool CItemSystem::AddItem(int ClientID, const CItemID &Id, int Count, bool SilentFail)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return false;

	// never insert a zeroed, unknown or negative stack: the id can come from map
	// data or a database row, and an unknown id is unrenderable and unmatchable
	if(Id.IsZero() || Count <= 0 || !IsKnownItem(Id))
		return false;

	CInventory &Inventory = m_aInventories[ClientID];
	const SItemDef *pDef = m_Items.get(Id.Value());
	if(!AddToInventory(Inventory, Id, Count, pDef))
	{
		if(!SilentFail)
			m_pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("No inventory space!", "Item Pickup"));
		return false;
	}
	const int Now = Inventory.Get(Id);

	const char *pName = GetName(Id);
	const std::string Msg = LocalizeFormat("Item Pickup", "You got: {} x{} ({})",
		Localize(pName, "Item Name"), Count, Now);
	m_pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Msg.c_str());
	return true;
}

bool CItemSystem::AddItem(int ClientID, const char *pId, int Count, bool SilentFail)
{
	return AddItem(ClientID, CItemID::FromName(pId), Count, SilentFail);
}

const char *CItemSystem::GetName(const CItemID &Id) const
{
	return ItemIDDisplayName(Id);
}

const char *CItemSystem::GetDesc(const CItemID &Id) const
{
	const SItemDef *pItem = m_Items.get(Id.Value());
	return pItem ? pItem->m_aDesc : "";
}

const char *CItemSystem::GetWeaponName(const CItemID &Id) const
{
	const SItemDef *pItem = m_Items.get(Id.Value());
	return pItem ? pItem->m_aWeapon : "";
}

bool CItemSystem::IsAccessory(const CItemID &Id) const
{
	const SItemDef *pItem = m_Items.get(Id.Value());
	return pItem && pItem->m_Accessory.m_HasAccessory;
}

CItemSystem::SAccessory CItemSystem::GetAccessory(const CItemID &Id) const
{
	const SItemDef *pItem = m_Items.get(Id.Value());
	return pItem ? pItem->m_Accessory : SAccessory();
}

bool CItemSystem::IsDrill(const CItemID &Id) const
{
	const SItemDef *pItem = m_Items.get(Id.Value());
	return pItem && pItem->m_Drill.m_HasDrill;
}

CItemSystem::SDrill CItemSystem::GetDrill(const CItemID &Id) const
{
	const SItemDef *pItem = m_Items.get(Id.Value());
	return pItem ? pItem->m_Drill : SDrill();
}

bool CItemSystem::HasItemType(const CItemID &Id, const char *pType) const
{
	const SItemDef *pItem = m_Items.get(Id.Value());
	if(!pItem)
		return false;
	for(int i = 0; i < pItem->m_NumTypes; i++)
		if(str_comp(pItem->m_aTypes[i], pType) == 0)
			return true;
	return false;
}

void CItemSystem::ForEachItemType(const CItemID &Id, FItemTypeCallback pfnFunc, void *pUser) const
{
	if(!pfnFunc || Id.IsZero())
		return;
	const SItemDef *pItem = m_Items.get(Id.Value());
	if(!pItem)
		return;
	for(int i = 0; i < pItem->m_NumTypes; i++)
		pfnFunc(pItem->m_aTypes[i], pUser);
}

bool CItemSystem::IsItemForgotten(int Forgetting, const CItemID &Id) const
{
	if(Forgetting < 75 || Id.IsZero())
		return false;
	return (int) (uuid_table_function::hash(Id.Value()) % 100) < (Forgetting - 75) * 4;
}

bool CItemSystem::IsRecipeForgotten(int Forgetting, const char *pCraftId) const
{
	if(Forgetting < 50 || !pCraftId || !pCraftId[0])
		return false;
	return (int) (str_quickhash(pCraftId) % 100) < (Forgetting - 50) * 2;
}

int CItemSystem::GetIngredientCount(int ClientID, const SIngredient &Need) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return 0;

	const CInventory &Inventory = m_aInventories[ClientID];
	if(!Need.m_MatchByType)
		return Inventory.Get(Need.m_Id);

	int Total = 0;
	for(int i = 0; i < CInventory::MAX_ITEMS; i++)
	{
		if(Inventory.IsEmpty(i))
			continue;
		if(HasItemType(Inventory.m_aItems[i].m_Id, Need.m_aType))
			Total += Inventory.m_aItems[i].m_Count;
	}
	return Total;
}

bool CItemSystem::HasItem(int ClientID, const CItemID &Id) const
{
	return GetItemCount(ClientID, Id) > 0;
}

int CItemSystem::GetItemCount(int ClientID, const CItemID &Id) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return 0;
	return m_aInventories[ClientID].Get(Id);
}

bool CItemSystem::RemoveItem(int ClientID, const CItemID &Id, int Count)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || Count <= 0)
		return false;

	CInventory &Inventory = m_aInventories[ClientID];
	const int Index = Inventory.Find(Id);
	if(Index < 0 || Inventory.m_aItems[Index].m_Count < Count)
		return false;

	Inventory.m_aItems[Index].m_Count -= Count;
	if(Inventory.m_aItems[Index].m_Count <= 0)
		Inventory.ClearSlot(Index);
	return true;
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
		const SItemDef *pItem = m_Items.get(Inventory.m_aItems[i].m_Id.Value());
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

void CItemSystem::AmmoFindCallback(SItemDef &Item, void *pUser)
{
	SAmmoFindData *pData = static_cast<SAmmoFindData *>(pUser);
	if(pData->m_pFound)
		return;
	// this item must be usable by the weapon and actually owned
	if(pData->m_pItem->GetItemCount(pData->m_ClientID, Item.m_Id) <= 0)
		return;
	for(int k = 0; k < Item.m_NumAmmoFor; k++)
	{
		if(str_comp(Item.m_aAmmoFor[k], pData->m_pWeaponName) == 0)
		{
			pData->m_pFound = &Item;
			return;
		}
	}
}

bool CItemSystem::ConsumeAmmoForWeapon(int ClientID, const char *pWeaponName, SAmmoEffect *pOut)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return false;

	// pick the first ammo item the player owns that is usable by this weapon
	SAmmoFindData Data = {this, ClientID, pWeaponName, nullptr};
	m_Items.for_each(AmmoFindCallback, &Data);
	if(!Data.m_pFound)
		return false;

	const SItemDef *pAmmo = Data.m_pFound;
	if(!RemoveItem(ClientID, pAmmo->m_Id, 1))
		return false;
	if(pOut)
		*pOut = pAmmo->m_AmmoEffect;
	return true;
}

void CItemSystem::AmmoAddCallback(SItemDef &Item, void *pUser)
{
	SAmmoAddData *pData = static_cast<SAmmoAddData *>(pUser);
	if(pData->m_Found)
		return;
	for(int k = 0; k < Item.m_NumAmmoFor; k++)
	{
		if(str_comp(Item.m_aAmmoFor[k], pData->m_pWeaponName) == 0)
		{
			pData->m_Id = Item.m_Id;
			pData->m_Found = true;
			return;
		}
	}
}

void CItemSystem::AddAmmoForWeapon(int ClientID, const char *pWeaponName, int Count)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || Count <= 0)
		return;

	SAmmoAddData Data = {pWeaponName, CItemID(), false};
	m_Items.for_each(AmmoAddCallback, &Data);
	if(Data.m_Found)
		AddItem(ClientID, Data.m_Id, Count, true);
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

	char aCraftId[64];
	if(Len - 5 >= (int) sizeof(aCraftId))
		return 0;
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
	mem_zero(&Craft, sizeof(Craft));
	str_copy(Craft.m_aCraftId, pCraftId, sizeof(Craft.m_aCraftId));

	// results: either a single "result" object, or a "gives" array, or both
	auto AddGive = [&](const json_value &rGive) {
		if(Craft.m_NumGives >= SCraftDef::MAX_GIVES)
			return;
		SResult &Give = Craft.m_aGives[Craft.m_NumGives];
		const json_value &rId = rGive["item_id"];
		const char *pId = rId.type == json_string ? (const char *) rId : (const char *) rGive["id"];
		if(!pId || !pId[0])
			return;
		Give.m_Id = CItemID::FromName(pId);
		Give.m_Count = 1;
		const json_value &rCount = rGive["count"];
		if(rCount.type == json_integer || rCount.type == json_double)
			Give.m_Count = (int) (json_int_t) rCount;
		Craft.m_NumGives++;
	};

	const json_value &Gives = (*pJson)["gives"];
	if(Gives.type == json_array)
		for(unsigned i = 0; i < Gives.u.array.length; i++)
			AddGive(Gives[i]);
	const json_value &Result = (*pJson)["result"];
	if(Result.type == json_object)
		AddGive(Result);

	// one or more ingredients in "needed"
	const json_value &Needed = (*pJson)["needed"];
	const int NumIng = (Needed.type == json_array) ? (int) Needed.u.array.length : ((Needed.type == json_object) ? 1 : 0);
	for(int i = 0; i < NumIng && Craft.m_NumNeeded < SCraftDef::MAX_NEEDED; i++)
	{
		const json_value &Ing = (Needed.type == json_array) ? Needed[i] : Needed;
		SIngredient &Need = Craft.m_aNeeded[Craft.m_NumNeeded];
		// by default match an exact item id; "item_id" (old) or "id" (new)
		const json_value &IdVal = Ing["item_id"];
		const char *pNeedId = IdVal.type == json_string ? (const char *) IdVal : (const char *) Ing["id"];
		Need.m_MatchByType = false;
		if(pNeedId && pNeedId[0])
			Need.m_Id = CItemID::FromName(pNeedId);
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

	const unsigned CraftHash = str_quickhash(pCraftId);
	const SCraftDef *pExistingCraft = m_Crafts.get(CraftHash);
	if(pExistingCraft && str_comp(pExistingCraft->m_aCraftId, pCraftId) != 0)
	{
		dbg_msg("craft", "hash collision: '%s' collides with '%s', keeping the first", pCraftId, pExistingCraft->m_aCraftId);
		return;
	}
	m_Crafts.set(CraftHash, Craft);
	dbg_msg("craft", "loaded craft '%s' (%d ingredient(s), %d result(s))", pCraftId, Craft.m_NumNeeded, Craft.m_NumGives);
}
