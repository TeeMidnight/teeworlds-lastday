/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_NPCDATA_H
#define GAME_SERVER_NPCDATA_H

#include <base/tl/array.h>
#include <base/uuid.h>
#include <base/vmath.h>

#include <game/item_id.h>
#include <game/server/bot.h> // CBotSkin

class CGameContext;
class IStorage;

// One entry on an NPC's shelf. The price is another item; price_count 0 makes
// the entry free.
struct SNpcSell
{
	CItemID m_Item;
	int m_Count;
	CItemID m_PriceItem;
	int m_PriceCount;
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
	char m_aNpcId[64];
	Uuid m_BotId; // deterministic, derived from the npc id
	char m_aName[64];
	char m_aWorld[64]; // map name the NPC is spawned in
	vec2 m_Pos;        // world position (tiles converted by the loader)
	int m_Health;
	int m_HealthRegen; // hit points per second
	CBotSkin m_Skin;
	SNpcAggroDef m_Aggro;
	array<SNpcSell> m_lSells;
};

// Loads and owns every NPC definition. Definitions are keyed by their npc id
// and by their deterministic BotID.
class CNpcSystem
{
	CGameContext *m_pGameServer;
	IStorage *m_pStorage;
	array<SNpcDef *> m_lDefs;

	void Clear();
	static int ListCallback(const char *pFilename, int IsDir, int StorageType, void *pUser);
	void Load(const char *pDefaultId, const char *pFilePath);

public:
	CNpcSystem();
	~CNpcSystem();

	void Init(CGameContext *pGameServer, IStorage *pStorage);

	const SNpcDef *Find(const char *pNpcId) const;
	const SNpcDef *Find(const Uuid &BotId) const;
	const SNpcDef *DefAt(int Index) const { return Index >= 0 && Index < m_lDefs.size() ? m_lDefs[Index] : 0; }
	int NumDefs() const { return m_lDefs.size(); }
};

#endif // GAME_SERVER_NPCDATA_H
