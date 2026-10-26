/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_NPCDATA_H
#define GAME_SERVER_NPCDATA_H

#include <base/tl/array.h>
#include <base/uuid.h>
#include <base/vmath.h>

#include <game/item_id.h>
#include <game/npc_id.h>
#include <game/server/bot.h> // CBotSkin
#include <game/server/mobdata.h> // dialogue, auras and faction

class CGameContext;
class IStorage;

// One entry on an NPC's shelf. The price is another item; price_count 0 makes
// the entry free. The stock rules come from the json; the stock itself is
// runtime state persisted in npc_stock.
struct SNpcSell
{
	CItemID m_Item;
	int m_Count; // units handed over per purchase
	CItemID m_PriceItem;
	int m_PriceCount;

	// restock rules
	int m_StockMin;
	int m_StockMax;
	int m_RestockSeconds; // 0 disables restocking

	// runtime stock state
	int m_Stock;
	int m_RestockAt; // unix time of the next restock
};

// How an NPC's aggro level moves. The level itself is a database ledger; these
// values only describe the deltas.
struct SNpcAggroDef
{
	int m_Threshold; // above this the NPC chases and attacks the player
	int m_PerKill;   // added when that player kills the NPC
	int m_PerHit;    // added when that player hits the NPC
	int m_KillDecay; // removed when the NPC kills that player
};

// One configured NPC, loaded from datasrc/bots/npc/<npc_id>.json. A definition
// describes exactly one spawn: it may not be reused for a second one.
struct SNpcDef
{
	CNpcID m_Id;        // deterministic identity and name source
	char m_aWorld[64]; // map name the NPC is spawned in
	vec2 m_Pos;        // world position (tiles converted by the loader)
	int m_Health;
	int m_HealthRegen; // hit points per second
	int m_Faction;     // BOTFACTION_*; friendly NPCs never gain aggro
	CBotSkin m_Skin;
	SNpcAggroDef m_Aggro;
	array<SNpcSell> m_lSells;
	SMobDialogue m_Dialogue;
	SMobHealAura m_HealAura;
	SMobArmorAura m_ArmorAura;
	// neutral NPCs may defend the area against hostile monsters; friendly ones
	// never start a fight, and a neutral NPC without this flag only retaliates
	bool m_AttackHostiles;
};

// How close a player must be to trade with an NPC (and to be told about it),
// in world units. Five tiles matches the NPC's normal attention range.
constexpr float NPC_INTERACT_RADIUS = 5.0f * 32.0f;

// Loads and owns every NPC definition. Definitions are keyed by their CNpcID.
class CNpcSystem
{
	CGameContext *m_pGameServer;
	IStorage *m_pStorage;
	array<SNpcDef *> m_lDefs;

	void Clear();
	// mirror every loaded NPC's shelf into the database (clear + insert) and
	// load (or roll) the runtime stock of each entry
	void PersistShelves();
	// the stock amount one restock rolls for an entry
	int RollStock(const SNpcSell &Sell) const;
	// read an entry's stock from npc_stock, or roll and store a fresh one
	void LoadOrInitStock(SNpcDef *pDef, SNpcSell &Sell);
	void PersistStock(const SNpcDef *pDef, const SNpcSell &Sell);
	static int ListCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	void Load(const char *pDefaultId, const char *pFilePath);

public:
	CNpcSystem();
	~CNpcSystem();

	void Init(CGameContext *pGameServer, IStorage *pStorage);

	// Roll new stock for every entry whose restock time has passed. A rolled
	// amount below the current stock leaves the stock alone. Returns true when
	// something changed, so open shop menus can be regenerated.
	bool RestockDue();
	// Take Count units out of a shelf entry, persisting the result. Returns
	// false when the entry does not have that much stock.
	bool ConsumeStock(int DefIndex, int SellIndex, int Count);

	const SNpcDef *Find(const CNpcID &Id) const;
	const SNpcDef *DefAt(int Index) const { return Index >= 0 && Index < m_lDefs.size() ? m_lDefs[Index] : 0; }
	int NumDefs() const { return m_lDefs.size(); }
};

#endif // GAME_SERVER_NPCDATA_H
