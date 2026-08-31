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
	m_pStorage->ListDirectory(IStorage::TYPE_ALL, "items", ListItemsCallback, this);
}

void CItemSystem::AddItem(int ClientID, const char *pResId, int Count)
{
	CInventory &Inventory = m_aInventories[ClientID];
	int Index = Inventory.Find(pResId);
	if(Index < 0)
	{
		if(Inventory.m_NumItems >= CInventory::MAX_ITEMS)
			return;
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
	m_pGameServer->SendChat(-1, CHAT_ALL, ClientID, aMsg);
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
