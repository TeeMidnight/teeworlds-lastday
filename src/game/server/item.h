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
		enum
		{
			MAX_TYPES = 4,
		};
		char m_aName[64];
		char m_aDesc[256];
		// an item can belong to several types (e.g. an ore is both "ore" and
		// "stone"); used to match crafting ingredients by type
		char m_aTypes[MAX_TYPES][32];
		int m_NumTypes;
	};
	hash_table<unsigned, SItemDef, 8> m_Items; // keyed by str_quickhash(res_id)

	static int ListItemsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	void LoadItem(const char *pResId, const char *pFilePath);

	static int ListCraftsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	void LoadCraft(const char *pCraftId, const char *pFilePath);

public:
	// ------- crafting -------
	// one ingredient of a recipe; tools (m_IsTool) are required but not consumed.
	// an ingredient matches an owned item either by exact id (m_MatchByType == false)
	// or by its item_type (m_MatchByType == true)
	struct SIngredient
	{
		char m_aItemId[32]; // exact item id, used when m_MatchByType == false
		char m_aType[32]; // item_type, used when m_MatchByType == true
		bool m_MatchByType;
		int m_Count;
		bool m_IsTool;
	};

	// a crafting recipe, loaded from datasrc/craft/*.json
	struct SCraftDef
	{
		char m_aCraftId[32];
		char m_aResultItemId[32];
		int m_ResultCount;
		SIngredient m_aNeeded[8];
		int m_NumNeeded;
	};

private:
	// try to reserve the recipe's non-tool ingredients against the shared
	// inventory, so an item that carries overlapping types (e.g. an ore that is
	// both "ore" and "stone") is never double-counted. on success fills pTake
	// with the amount to remove per inventory slot and returns true
	bool ReserveIngredients(int ClientID, const SCraftDef *pCraft, int *pTake) const;

public:
	hash_table<unsigned, SCraftDef, 8> m_Crafts; // keyed by str_quickhash(craft_id)

	// callback used to enumerate all loaded recipes
	typedef void (*FCraftCallback)(SCraftDef &Craft, void *pUser);
	void ForEachCraft(FCraftCallback pfnFunc, void *pUser);
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

	// add an item to a player's inventory and broadcast the pickup in chat.
	// returns false (and does not insert) when the inventory is full.
	// when SilentFail is false a "no space" message is broadcast, when true it is not.
	bool AddItem(int ClientID, const char *pResId, int Count, bool SilentFail = false);

	// display name of the item, or the res_id when unknown
	const char *GetName(const char *pResId) const;
	// description of the item, or "" when unknown
	const char *GetDesc(const char *pResId) const;
	// optional item_type of the item: whether the item carries the given type
	bool HasItemType(const char *pResId, const char *pType) const;
	// total quantity the player owns that matches this ingredient (by id or by type)
	int GetIngredientCount(int ClientID, const SIngredient &Need) const;

	// the recipe with the given craft_id, or nullptr
	const SCraftDef *GetCraft(const char *pCraftId) const;
	// whether the player owns every needed item (including tools, in the
	// required quantity for non-tool ingredients)
	bool HasIngredients(int ClientID, const SCraftDef *pCraft) const;

	// result of attempting a craft
	enum ECraftResult
	{
		CRAFT_OK = 0,
		CRAFT_NO_MATERIALS = 1, // the player lacks one or more ingredients
		CRAFT_NO_SPACE = 2, // no inventory slot for the result
	};

	// perform the craft: consume every non-tool needed item and give the
	// result. tools are kept. returns one of the ECraftResult values
	ECraftResult Craft(int ClientID, const char *pCraftId);
};

#endif
