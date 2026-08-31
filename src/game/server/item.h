#ifndef GAME_SERVER_ITEM_H
#define GAME_SERVER_ITEM_H

#include <base/tl/hashtable.h>
#include <base/system.h>

#include <engine/shared/protocol.h>

class CGameContext;
class IStorage;

// the item system: owns the item definitions (datasrc/items/*.json) and every
// player's inventory
class CItemSystem
{
	class CGameContext *m_pGameServer;
	class IStorage *m_pStorage;

	struct SItemDef
	{
		char m_aName[64];
		char m_aDesc[256];
	};
	hash_table<unsigned, SItemDef, 8> m_Items; // keyed by str_quickhash(res_id)

	static int ListItemsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	void LoadItem(const char *pResId, const char *pFilePath);

public:
	// a player's inventory: a fixed-size list of items (res_id + count)
	struct CInventory
	{
		enum
		{
			MAX_ITEMS = 16,
		};

		struct SItem
		{
			char m_aResId[32];
			int m_Count;
		};

		SItem m_aItems[MAX_ITEMS];
		int m_NumItems;

		CInventory();
		// index of the item with the given res_id, or -1
		int Find(const char *pResId) const;
		// current count of the item, or 0
		int Get(const char *pResId) const;
	};

	// one inventory per client
	CInventory m_aInventories[MAX_CLIENTS];

	// set the game server and storage, and load the item definitions
	void Init(class CGameContext *pGameServer, class IStorage *pStorage);
	void Load(class IStorage *pStorage);

	CInventory &GetInventory(int ClientID) { return m_aInventories[ClientID]; }

	// add an item to a player's inventory and broadcast the pickup in chat
	void AddItem(int ClientID, const char *pResId, int Count);

	// display name of the item, or the res_id when unknown
	const char *GetName(const char *pResId) const;
	// description of the item, or "" when unknown
	const char *GetDesc(const char *pResId) const;
};

#endif
