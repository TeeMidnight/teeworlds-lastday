/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_ITEM_H
#define GAME_SERVER_ITEM_H

#include <base/system.h>
#include <base/tl/hashtable.h>
#include <engine/shared/protocol.h>
#include <game/item_id.h>
#include <game/server/ammo.h>

class CGameContext;
class IStorage;

// the item system: owns the item definitions (data/items/*.json) and every
// player's inventory. Items are identified by a namespaced CItemID (a UUID),
// never by a bare hash of a file name.
class CItemSystem
{
public:
	// forward declaration: the inventory type is defined in the public section
	// below, but the private craft helpers need to name it
	struct CInventory;

	// A fixed-size grid attached to one inventory stack (a backpack). The
	// effective number of slots comes from the item definition, so the struct
	// can hold the largest backpack without changing its layout.
	enum
	{
		MAX_ITEM_DATA_SLOTS = 16,
	};

	struct SItemStack
	{
		CItemID m_Id;
		int m_Count = 0;
	};

	// Per-stack payload. Ordinary items leave m_HasData false; a backpack sets
	// it even while empty so the saved row always carries its "data" field.
	struct SItemData
	{
		bool m_HasData = false;
		SItemStack m_aSlots[MAX_ITEM_DATA_SLOTS];
	};

	// use effects applied when the player "uses" the item from the inventory
	// menu (consumes one). defined through the optional "use" json object.
	struct SUse
	{
		int m_Health; // restore health (clamped to max)
		// forgetting delta (mercury chapter): positive = forget more,
		// negative = recall (clamped to 0..100)
		int m_Forgetting;
		bool m_HasUse; // whether a "use" object was declared

		SUse() :
			m_Health(0),
			m_Forgetting(0),
			m_HasUse(false)
		{
		}
	};

	// passive effects of an accessory. an item that declares an "accessory"
	// json object can be worn in one of the character's accessory slots instead
	// of being fired or used, and grants these bonuses while worn.
	struct SAccessory
	{
		int m_Health; // extra maximum health
		float m_Range; // bonus attack range multiplier (0.20 = +20%)
		float m_Speed; // bonus movement speed multiplier (0.10 = +10%)
		bool m_HasAccessory; // whether an "accessory" object was declared

		SAccessory() :
			m_Health(0),
			m_Range(0.0f),
			m_Speed(0.0f),
			m_HasAccessory(false)
		{
		}
	};

	// placement effect of a drill item: a drill can be put down as a building
	// and controlled from the scoreboard. m_MaxLength is how far (in world
	// units) its beam can bore into the rock.
	struct SDrill
	{
		int m_MaxLength;
		// hardness removed from a resource node per boring pass. Different
		// materials bore at different rates (iron 1, steel 3, ...).
		int m_Power;
		bool m_HasDrill; // whether a "drill" object was declared

		SDrill() :
			m_MaxLength(0),
			m_Power(1),
			m_HasDrill(false)
		{
		}
	};

	// placement effect of a health jar item: a laser-framed container that shows
	// a health pickup in its middle. While its owner stands close it drops one
	// health pickup to the ground, at most once per m_Cooldown seconds.
	struct SHealthJar
	{
		int m_Cooldown; // seconds between two drops
		int m_Range; // how close the owner has to stand (world units)
		int m_Health; // damage the frame takes before it is dismantled
		bool m_HasJar; // whether a "jar" object was declared

		SHealthJar() :
			m_Cooldown(45),
			m_Range(96),
			m_Health(10),
			m_HasJar(false)
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
			MAX_AMMO_FOR = 8,
		};
		CItemID m_Id;
		char m_aIdString[64]; // normalized namespaced source ("lastday:stone")
		char m_aName[64];
		char m_aDesc[256];
		// the item's folder under data/items, e.g. "weapon"
		char m_aCategory[32];
		// optional Python module for on_use / on_place; -1 = native only
		char m_aScript[128];
		int m_Module;
		// the weapon this item equips as, or "" when it is not a weapon
		char m_aWeapon[32];
		// an item can belong to several types (e.g. an ore is both "ore" and
		// "stone"); used to match crafting ingredients by type
		char m_aTypes[MAX_TYPES][32];
		int m_NumTypes;
		// weapons (by weapon name) this item can be used as ammo for. an item
		// with at least one entry is an ammo item; the weapon consumes it from
		// the player's inventory when firing.
		char m_aAmmoFor[MAX_AMMO_FOR][32];
		int m_NumAmmoFor;
		// the ammo's effect (see SAmmoEffect); free form tags a weapon may use
		SAmmoEffect m_AmmoEffect;
		SUse m_Use;
		SAccessory m_Accessory;
		SDrill m_Drill;
		SHealthJar m_Jar;
		// maximum copies in one inventory stack; 0 = unlimited. weapons,
		// accessories and backpacks are forced to 1 by the accessors below.
		int m_MaxStack;
		// fixed slot count of a backpack item; 0 = not a backpack
		int m_BackpackSlots;
		// container kind this item places ("crate", "chest", ...); "" when
		// the item does not place a container
		char m_aContainerKind[32];
	};
	hash_table<Uuid, SItemDef, 16, uuid_table_function> m_Items;

	static int ListItemDirsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	static int ListItemFilesCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	void LoadItem(const char *pDefaultId, const char *pFilePath, const char *pCategory);

	// scans every item definition for one usable as ammo for a weapon
	static void AmmoScanCallback(SItemDef &Item, void *pUser);
	// find the ammo definition a weapon should consume next (first owned one)
	struct SAmmoFindData
	{
		const CItemSystem *m_pItem;
		int m_ClientID;
		const char *m_pWeaponName;
		const SItemDef *m_pFound;
	};
	static void AmmoFindCallback(SItemDef &Item, void *pUser);

	struct SAmmoAddData
	{
		const char *m_pWeaponName;
		CItemID m_Id;
		bool m_Found;
	};
	static void AmmoAddCallback(SItemDef &Item, void *pUser);
	static void ReloadScriptCallback(SItemDef &Item, void *pUser);

	// add to an inventory without any chat / logging, used by craft simulation
	static bool AddToInventory(CInventory &Inventory, const CItemID &Id, int Count, const SItemDef *pDef, const SItemData *pData = nullptr);
	// maximum stack for a definition; enforces the weapon/accessory/backpack
	// rules in one place (the loader stores the content-authored value)
	static int MaxStackFor(const SItemDef *pDef);
	// native placement of a container item (no Python script needed)
	bool PlaceContainer(int ClientID, const CItemID &Id);
	// add Count copies into a backpack grid without chat/logging
	static bool AddToNested(SItemData &Data, int Slots, const CItemID &Id, int Count, const SItemDef *pDef);
	// how many more copies of Id fit in the main inventory, capped at Limit
	static int InventoryRoom(const CInventory &Inventory, const CItemID &Id, const SItemDef *pDef, int Limit);
	// how many more copies fit in one backpack grid, capped at Limit
	static int NestedRoom(const SItemData &Data, int Slots, const CItemID &Id, const SItemDef *pDef, int Limit);
	// Add Count copies, filling the main inventory first and overflowing into
	// owned backpack grids. Nothing is committed unless every copy fits.
	bool AddToInventoryOrBackpacks(CInventory &Inventory, const CItemID &Id, int Count, const SItemDef *pDef, int *pMainAdded = nullptr, int *pBagAdded = nullptr);
	// total copies in the main inventory plus every backpack grid (display only;
	// gameplay lookups still use the main inventory unless they move the stack)
	int CountOwned(const CInventory &Inventory, const CItemID &Id) const;

	static int ListCraftDirsCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	static int ListCraftFilesCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	void LoadCraft(const char *pCraftId, const char *pFilePath, const char *pCategory);

public:
	// ------- crafting -------
	// one ingredient of a recipe; tools (m_IsTool) are required but not consumed.
	// an ingredient matches an owned item either by exact id (m_MatchByType == false)
	// or by its item_type (m_MatchByType == true)
	struct SIngredient
	{
		CItemID m_Id; // exact item id, used when m_MatchByType == false
		char m_aType[32]; // item_type, used when m_MatchByType == true
		bool m_MatchByType;
		int m_Count;
		bool m_IsTool;
	};

	// one output of a recipe
	struct SResult
	{
		CItemID m_Id;
		int m_Count;
	};

	// a crafting recipe, loaded from data/craft/*.json. A recipe may have more
	// than one output (byproducts) and more than one input.
	struct SCraftDef
	{
		enum
		{
			MAX_NEEDED = 8,
			MAX_GIVES = 8,
		};
		char m_aCraftId[32];
		// the recipe's folder under data/craft, e.g. "weapon"; this doubles as
		// the localized category header in the crafting menu
		char m_aCategory[32];
		SResult m_aGives[MAX_GIVES];
		int m_NumGives;
		SIngredient m_aNeeded[MAX_NEEDED];
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

	// a player's inventory: a fixed number of slots. an empty slot has a
	// zeroed item id. slots never move: removing an item that reaches zero
	// simply clears the slot. Adding a capped item first tops up its existing
	// stacks and then takes empty slots, so one item kind may span several
	// slots while the rest of the inventory is full.
	struct CInventory
	{
		enum
		{
			MAX_ITEMS = 16,
		};

		struct SItem
		{
			CItemID m_Id;
			int m_Count;
			// backpack grid; m_HasData is false for ordinary items
			SItemData m_Data;
		};

		SItem m_aItems[MAX_ITEMS];

		CInventory();
		// drop everything: used when a client slot is recycled so the new
		// occupant never sees the previous player's inventory
		void Reset() { mem_zero(m_aItems, sizeof(m_aItems)); }
		// index of the first stack with the given id, or -1
		int Find(const CItemID &Id) const;
		// index of the first empty slot, or -1 when the inventory is full
		int FindEmpty() const;
		// total count of the item across every stack, or 0
		int Get(const CItemID &Id) const;
		// whether the slot is empty
		bool IsEmpty(int Index) const { return Index < 0 || Index >= MAX_ITEMS || m_aItems[Index].m_Id.IsZero() || m_aItems[Index].m_Count <= 0; }
		// clear the slot (used when its count reaches zero)
		void ClearSlot(int Index);
	};

	// one inventory per client
	CInventory m_aInventories[MAX_CLIENTS];

	void Init(class CGameContext *pGameServer, class IStorage *pStorage);
	void Load(class IStorage *pStorage);
	// re-exec every item script in place (monotonic hot reload)
	bool ReloadScripts();

	CInventory &GetInventory(int ClientID) { return m_aInventories[ClientID]; }
	const CInventory &GetInventory(int ClientID) const { return m_aInventories[ClientID]; }

	// add an item to a player's inventory and broadcast the pickup in chat.
	// returns false (and does not insert) when the inventory is full.
	bool AddItem(int ClientID, const CItemID &Id, int Count, bool SilentFail = false);
	// add a stack that carries its own data (a dropped backpack). The data is
	// copied into the new slot; a stack with data never merges with another.
	bool AddItemWithData(int ClientID, const CItemID &Id, int Count, const SItemData &Data, bool SilentFail = false);
	// remove up to Count items from one concrete inventory slot. Used when the
	// caller has to keep the stack identity (a backpack's contents).
	bool RemoveItemFromSlot(int ClientID, int Slot, int Count);
	// convenience overload for content / map data that carries a name
	bool AddItem(int ClientID, const char *pId, int Count, bool SilentFail = false);

	// display name of the item (localized), or the source/id fallback
	const char *GetName(const CItemID &Id) const;
	// untranslated display name, used as the description's translation context
	const char *GetRawName(const CItemID &Id) const;
	// description of the item, or "" when unknown
	const char *GetDesc(const CItemID &Id) const;
	// the item's category folder ("weapon", "tool", ...), or "" when unknown
	const char *GetCategory(const CItemID &Id) const;
	// whether an item definition exists for this id
	bool IsKnownItem(const CItemID &Id) const { return m_Items.get(Id.Value()) != 0; }
	bool IsKnownItem(const char *pId) const { return IsKnownItem(CItemID::FromName(pId)); }
	// whether the item definition declares a "use" effect (see UseItem)
	bool IsUsable(const CItemID &Id) const;
	// the item's declared use effects; m_HasUse false when none
	SUse GetUse(const CItemID &Id) const;
	// the weapon name this item equips as, or "" when it is not a weapon
	const char *GetWeaponName(const CItemID &Id) const;
	// whether the item is an accessory (declares an "accessory" object)
	bool IsAccessory(const CItemID &Id) const;
	// the item's accessory effects; m_HasAccessory false when it is not one
	SAccessory GetAccessory(const CItemID &Id) const;
	// whether the item can be placed as a drill building
	bool IsDrill(const CItemID &Id) const;
	// the item's drill data; m_HasDrill false when it is not a drill
	SDrill GetDrill(const CItemID &Id) const;
	// whether the item can be placed as a health jar building
	bool IsHealthJar(const CItemID &Id) const;
	// the item's health jar data; m_HasJar false when it is not one
	SHealthJar GetHealthJar(const CItemID &Id) const;

	// maximum copies allowed in one stack; 0 = unlimited. Weapons,
	// accessories and backpacks always return 1.
	int GetMaxStack(const CItemID &Id) const;
	// whether the item carries a fixed backpack grid
	bool IsBackpack(const CItemID &Id) const;
	// the backpack's fixed slot count, or 0 when it is not a backpack
	int GetBackpackSlots(const CItemID &Id) const;
	// container kind this item places into the world, or "" when it places
	// no container
	const char *GetContainerKind(const CItemID &Id) const;

	// move up to Count copies from one inventory slot into one of the
	// backpacks the player owns. Returns false without changing anything when
	// the item does not fit. Backpacks cannot contain another backpack.
	bool StoreInBackpack(int ClientID, int BackpackSlot, int ItemSlot, int Count, bool SilentFail = false);
	// move up to Count copies from a backpack's nested slot back into the main
	// inventory. Returns false without changing anything when it does not fit.
	bool TakeFromBackpack(int ClientID, int BackpackSlot, int NestedSlot, int Count, bool SilentFail = false);
	// first backpack slot with room for the given item, or -1
	int FindBackpackWithRoom(int ClientID, const CItemID &Id, int Count = 1) const;

	// consume up to Count items and apply their "use" effects to the player
	int UseItem(int ClientID, const CItemID &Id, int Count = 1);
	// whether the item has an on_place script that builds a world object
	bool IsPlaceable(const CItemID &Id) const;
	// run the item's on_place script; one copy is consumed on a True return
	bool PlaceItem(int ClientID, const CItemID &Id);
	// optional item_type of the item
	bool HasItemType(const CItemID &Id, const char *pType) const;
	// callback used to enumerate the type tags of a single item
	typedef void (*FItemTypeCallback)(const char *pType, void *pUser);
	void ForEachItemType(const CItemID &Id, FItemTypeCallback pfnFunc, void *pUser) const;

	// mercury chapter amnesia (deterministic, hash based)
	bool IsItemForgotten(int Forgetting, const CItemID &Id) const;
	bool IsRecipeForgotten(int Forgetting, const char *pCraftId) const;

	// total quantity the player owns that matches this ingredient
	int GetIngredientCount(int ClientID, const SIngredient &Need) const;

	// whether the player owns at least one of the given item
	bool HasItem(int ClientID, const CItemID &Id) const;
	bool HasItem(int ClientID, const char *pId) const { return HasItem(ClientID, CItemID::FromName(pId)); }
	// current count of the given item in the player's inventory, or 0
	int GetItemCount(int ClientID, const CItemID &Id) const;
	// remove up to Count of the given item. returns false (removes nothing)
	// when the player does not own enough.
	bool RemoveItem(int ClientID, const CItemID &Id, int Count);

	// whether any item definition is usable as ammo for the given weapon
	bool WeaponNeedsAmmo(const char *pWeaponName);
	// total ammo the player has that is usable by the given weapon
	int GetAmmoCountForWeapon(int ClientID, const char *pWeaponName) const;
	// consume one ammo item usable by the given weapon and fill its effect.
	// returns false (and leaves pOut untouched) when the player had no ammo.
	bool ConsumeAmmoForWeapon(int ClientID, const char *pWeaponName, SAmmoEffect *pOut);
	// add Count ammo usable by the given weapon
	void AddAmmoForWeapon(int ClientID, const char *pWeaponName, int Count);

	// the recipe with the given craft_id, or nullptr
	const SCraftDef *GetCraft(const char *pCraftId) const;
	// whether the player owns every needed item (including tools)
	bool HasIngredients(int ClientID, const SCraftDef *pCraft) const;

	enum ECraftResult
	{
		CRAFT_OK = 0,
		CRAFT_NO_MATERIALS = 1,
		CRAFT_NO_SPACE = 2,
	};

	// perform the craft: consume every non-tool needed item and give every
	// result. tools are kept. returns one of the ECraftResult values
	ECraftResult Craft(int ClientID, const char *pCraftId);
};

#endif
