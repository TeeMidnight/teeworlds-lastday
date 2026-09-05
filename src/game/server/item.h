#ifndef GAME_SERVER_ITEM_H
#define GAME_SERVER_ITEM_H

#include <base/system.h>
#include <base/tl/hashtable.h>

#include <engine/shared/protocol.h>

class CGameContext;
class IStorage;

// the item system: owns the item definitions (datasrc/items/*.json) and every
// player's inventory
class CItemSystem
{
public:
	// use effects applied when the player "uses" the item from the inventory
	// menu (consumes one). defined through the optional "use" json object.
	struct SUse
	{
		int m_Health; // restore health (clamped to max)
		int m_Sanity; // restore sanity (clamped to 100)
		bool m_HasUse; // whether a "use" object was declared

		SUse() :
			m_Health(0),
			m_Sanity(0),
			m_HasUse(false)
		{
		}
	};

private:
	class CGameContext *m_pGameServer;
	class IStorage *m_pStorage;

	struct SItemDef
	{
		enum
		{
			MAX_TYPES = 4,
			MAX_AMMO_FOR = 4,
		};
		char m_aResId[32];
		char m_aName[64];
		char m_aDesc[256];
		// an item can belong to several types (e.g. an ore is both "ore" and
		// "stone"); used to match crafting ingredients by type
		char m_aTypes[MAX_TYPES][32];
		int m_NumTypes;
		// weapons (by weapon name) this item can be used as ammo for. an item
		// with at least one entry is an ammo item; the weapon consumes it from
		// the player's inventory when firing.
		char m_aAmmoFor[MAX_AMMO_FOR][32];
		int m_NumAmmoFor;
		// damage dealt when this item is used as ammo (0 = use the weapon's
		// default damage)
		int m_Damage;
		SUse m_Use;
	};
	hash_table<unsigned, SItemDef, 16> m_Items; // keyed by str_quickhash(res_id)

	static int ListItemsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	void LoadItem(const char *pResId, const char *pFilePath);

	// scans every item definition for one usable as ammo for a weapon
	static void AmmoScanCallback(SItemDef &Item, void *pUser);
	// finds the first ammo item definition usable by a weapon
	static void AmmoAddCallback(SItemDef &Item, void *pUser);

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
	hash_table<unsigned, SCraftDef, 16> m_Crafts; // keyed by str_quickhash(craft_id)

	// callback used to enumerate all loaded recipes
	typedef void (*FCraftCallback)(SCraftDef &Craft, void *pUser);
	void ForEachCraft(FCraftCallback pfnFunc, void *pUser);
	// a player's inventory: a fixed number of slots. an empty slot has an
	// empty res_id. slots never move: removing an item that reaches zero
	// simply clears the slot, and adding a new item goes into the first
	// empty slot.
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

		CInventory();
		// index of the item with the given res_id, or -1
		int Find(const char *pResId) const;
		// index of the first empty slot, or -1 when the inventory is full
		int FindEmpty() const;
		// current count of the item, or 0
		int Get(const char *pResId) const;
		// whether the slot is empty
		bool IsEmpty(int Index) const { return Index < 0 || Index >= MAX_ITEMS || m_aItems[Index].m_aResId[0] == '\0'; }
		// clear the slot (used when its count reaches zero)
		void ClearSlot(int Index);
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
	// whether an item definition exists for this res_id
	bool IsKnownItem(const char *pResId) const { return m_Items.get(str_quickhash(pResId)) != 0; }
	// whether the item definition declares a "use" effect (see UseItem)
	bool IsUsable(const char *pResId) const;
	// the item's declared use effects; m_HasUse false when none
	SUse GetUse(const char *pResId) const;

	// consume up to Count items and apply their "use" effects to the player
	// (health/sanity). stops early once nothing is left to restore (full
	// health/sanity) or the player runs out of the item. returns how many
	// items were actually consumed.
	int UseItem(int ClientID, const char *pResId, int Count = 1);
	// optional item_type of the item: whether the item carries the given type
	bool HasItemType(const char *pResId, const char *pType) const;
	// callback used to enumerate the type tags of a single item
	typedef void (*FItemTypeCallback)(const char *pType, void *pUser);
	// calls pfnFunc for every type tag of the item (e.g. "weapon", "ore").
	// pfnFunc is not called when the item is unknown or declares no types.
	void ForEachItemType(const char *pResId, FItemTypeCallback pfnFunc, void *pUser) const;
	// total quantity the player owns that matches this ingredient (by id or by type)
	int GetIngredientCount(int ClientID, const SIngredient &Need) const;

	// whether the player owns at least one item whose res_id hash equals Hash
	bool HasItemHash(int ClientID, unsigned Hash) const;
	// res_id of the first owned item whose hash equals Hash, or nullptr
	const char *GetResIdByHash(int ClientID, unsigned Hash) const;

	// whether the player owns at least one of the given item
	bool HasItem(int ClientID, const char *pResId) const;
	// current count of the given item in the player's inventory, or 0
	int GetItemCount(int ClientID, const char *pResId) const;
	// remove up to Count of the given item from the player's inventory.
	// returns false (and removes nothing) when the player does not own enough.
	bool RemoveItem(int ClientID, const char *pResId, int Count);

	// whether any item definition is usable as ammo for the given weapon
	// (i.e. the weapon needs ammo; otherwise it is unlimited, e.g. hammer)
	bool WeaponNeedsAmmo(const char *pWeaponName);
	// total ammo the player has that is usable by the given weapon
	int GetAmmoCountForWeapon(int ClientID, const char *pWeaponName) const;
	// consume one ammo item usable by the given weapon from the player's
	// inventory. returns the damage of the consumed ammo, or 0 when the
	// player had no ammo for the weapon.
	int ConsumeAmmoForWeapon(int ClientID, const char *pWeaponName);
	// add Count ammo usable by the given weapon to the player's inventory
	// (adds to the first ammo item that matches the weapon)
	void AddAmmoForWeapon(int ClientID, const char *pWeaponName, int Count);

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
