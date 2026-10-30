/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>
#include <engine/shared/jsonparser.h>
#include <engine/storage.h>
#include <game/server/currency.h>
#include <game/server/database/database.h>

#include "gamecontext.h"
#include "npcdata.h"

static const char *gs_pSkinParts[NUM_SKINPARTS] = {"body", "marking", "decoration", "hands", "feet", "eyes"};

// The TeeInfo skin part colours handed to clients pack H, S, L and A into one
// int: alpha in the top byte, then hue, saturation and lightness. Only the
// marking part is drawn with alpha, and the client keeps its alpha even for
// team colours, so dropping it made a custom marking invisible. A missing
// "alp" means opaque, matching the client's own skin files.
static int PackHsla(int Hue, int Sat, int Lgt, int Alp)
{
	return ((Alp & 0xff) << 24) | ((Hue & 0xff) << 16) | ((Sat & 0xff) << 8) | (Lgt & 0xff);
}

static int ReadInt(const json_value &Value, int Default)
{
	if(Value.type == json_integer || Value.type == json_double)
		return (int) (json_int_t) Value;
	return Default;
}

static int64 ReadInt64(const json_value &Value, int64 Default)
{
	if(Value.type == json_integer || Value.type == json_double)
		return (int64) (json_int_t) Value;
	return Default;
}

static float ReadFloat(const json_value &Value, float Default)
{
	if(Value.type == json_integer || Value.type == json_double)
		return (float) (double) Value;
	return Default;
}

static bool ReadBool(const json_value &Value, bool Default)
{
	if(Value.type == json_boolean)
		return (bool) Value;
	if(Value.type == json_integer)
		return Value.u.integer != 0;
	return Default;
}

// the persistable name of an item id: its namespaced source, or the uuid
static void ItemSourceName(const CItemID &Id, char *pOut, int Size)
{
	const char *pSource = Id.SourceName();
	if(pSource && pSource[0])
		str_copy(pOut, pSource, Size);
	else
		Id.FormatUuid(pOut, Size);
}

CNpcSystem::CNpcSystem()
{
	m_pGameServer = 0;
	m_pStorage = 0;
}

CNpcSystem::~CNpcSystem()
{
	Clear();
}

void CNpcSystem::Clear()
{
	for(int i = 0; i < m_lDefs.size(); i++)
		delete m_lDefs[i];
	m_lDefs.clear();
}

int CNpcSystem::ListCallback(const char *pFilename, int IsDir, int StorageType, void *pUser)
{
	(void) StorageType;
	CNpcSystem *pSelf = static_cast<CNpcSystem *>(pUser);
	if(IsDir)
		return 0;

	const int Len = str_length(pFilename);
	if(Len < 6 || str_comp(pFilename + Len - 5, ".json") != 0)
		return 0;

	// the file name is the default npc id; the json "npc_id" wins when present
	char aDefaultId[64];
	if(Len - 5 >= (int) sizeof(aDefaultId))
	{
		dbg_msg("npc", "npc file name too long: '%s'", pFilename);
		return 0;
	}
	str_copy(aDefaultId, pFilename, sizeof(aDefaultId));
	aDefaultId[Len - 5] = '\0';

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "bots/npc/%s", pFilename);
	pSelf->Load(aDefaultId, aPath);
	return 0;
}

void CNpcSystem::Load(const char *pDefaultId, const char *pFilePath)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseFile(pFilePath, m_pStorage, IStorage::TYPE_ALL);
	if(!pJson || pJson->type != json_object)
	{
		dbg_msg("npc", "failed to load npc '%s' from '%s'", pDefaultId, pFilePath);
		return;
	}

	SNpcDef *pDef = new SNpcDef();
	mem_zero(pDef, sizeof(SNpcDef));

	const char *pId = (*pJson)["npc_id"];
	if(!pId[0])
		pId = pDefaultId;
	const CNpcID Id = CNpcID::FromName(pId);
	if(Id.IsZero())
	{
		dbg_msg("npc", "npc '%s' has an empty id", pFilePath);
		delete pDef;
		return;
	}

	// a definition may only be used once: a duplicate id is a configuration
	// error, not a second spawn
	if(Find(Id))
	{
		dbg_msg("npc", "duplicate npc id '%s' (each npc definition may only be used once)", pId);
		delete pDef;
		return;
	}
	pDef->m_Id = Id;

	// the display name is registered against the id; it is content, not UI text,
	// so it never gets a translation entry
	const char *pName = (*pJson)["name"];
	if(!pName[0])
		pName = pId;
	char aSource[128];
	str_copy(aSource, Id.SourceName(), sizeof(aSource));
	NpcIDRegisterName(Id, aSource, pName);
	str_copy(pDef->m_aWorld, (*pJson)["world"], sizeof(pDef->m_aWorld));

	// the spawn is in tiles; the centre of the tile is where a tee stands
	const int TileX = ReadInt((*pJson)["x"], 0);
	const int TileY = ReadInt((*pJson)["y"], 0);
	pDef->m_Pos = vec2(TileX * 32.0f + 16.0f, TileY * 32.0f + 16.0f);

	pDef->m_Health = maximum(1, ReadInt((*pJson)["health"], 10));
	pDef->m_HealthRegen = maximum(0, ReadInt((*pJson)["health_regen"], 0));

	// skin: one object per part, LunarTee's "skin7" shape
	{
		const json_value &rSkin = (*pJson)["skin"];
		if(rSkin.type == json_object)
		{
			for(int i = 0; i < NUM_SKINPARTS; i++)
			{
				const json_value &rPart = rSkin[gs_pSkinParts[i]];
				if(rPart.type != json_object)
					continue;

				str_copy(pDef->m_Skin.m_aaSkinPartNames[i], rPart["name"], sizeof(pDef->m_Skin.m_aaSkinPartNames[i]));
				const json_value &rCustom = rPart["custom_color"];
				const bool Custom = rCustom.type == json_boolean ? (bool) rCustom : false;
				pDef->m_Skin.m_aUseCustomColors[i] = Custom ? 1 : 0;
				pDef->m_Skin.m_aSkinPartColors[i] = Custom ?
									    PackHsla(ReadInt(rPart["hue"], 0), ReadInt(rPart["sat"], 0), ReadInt(rPart["lgt"], 0), ReadInt(rPart["alp"], 255)) :
									    0;
			}
		}
	}

	// shelf: what this NPC sells. The price is currency in G, so an old entry
	// that still names a price_item is skipped with a warning instead of
	// silently becoming free (or staying barter).
	{
		const json_value &rSells = (*pJson)["sells"];
		if(rSells.type == json_array)
		{
			for(unsigned i = 0; i < rSells.u.array.length; i++)
			{
				const json_value &rSell = rSells[i];
				if(rSell.type != json_object)
					continue;
				const char *pItem = rSell["item"];
				if(!pItem[0])
					continue;

				// An old entry that names a price_item cannot be repriced
				// automatically, so skip it instead of making it free. A
				// missing price keeps the old "free entry" semantics.
				int64 Price = 0;
				const json_value &rPrice = rSell["price"];
				if(rPrice.type == json_integer || rPrice.type == json_double)
					Price = ReadInt64(rPrice, 0);
				else if(rSell["price_item"].type == json_string)
				{
					dbg_msg("npc", "shelf entry '%s' uses the removed price_item barter format; add a \"price\" in G", pItem);
					continue;
				}
				if(Price < 0)
					Price = 0;
				if(Price > CURRENCY_MAX_AMOUNT)
					Price = CURRENCY_MAX_AMOUNT;

				SNpcSell &Sell = pDef->m_lSells.emplace();
				Sell.m_Item = CItemID::FromName(pItem);
				Sell.m_Count = maximum(1, ReadInt(rSell["count"], 1));
				Sell.m_Price = Price;

				// restock rules; without a restock interval the stock never changes
				Sell.m_StockMin = maximum(0, ReadInt(rSell["stock_min"], Sell.m_Count));
				Sell.m_StockMax = maximum(Sell.m_StockMin, ReadInt(rSell["stock_max"], Sell.m_StockMin));
				Sell.m_RestockSeconds = maximum(0, ReadInt(rSell["restock_seconds"], 0));
				Sell.m_Stock = 0;
				Sell.m_RestockAt = 0;
			}
		}
	}

	// buy orders: goods the NPC takes from players for G. The package size is
	// fixed per entry, so the menu can show exactly what one click gives and
	// takes. "limit" is the number of packages per restock period.
	{
		const json_value &rBuys = (*pJson)["buys"];
		if(rBuys.type == json_array)
		{
			for(unsigned i = 0; i < rBuys.u.array.length; i++)
			{
				const json_value &rBuy = rBuys[i];
				if(rBuy.type != json_object)
					continue;
				const char *pItem = rBuy["item"];
				if(!pItem[0])
					continue;

				int64 Price = 0;
				const json_value &rPrice = rBuy["price"];
				if(rPrice.type == json_integer || rPrice.type == json_double)
					Price = ReadInt64(rPrice, 0);
				if(Price <= 0 || Price > CURRENCY_MAX_AMOUNT)
				{
					dbg_msg("npc", "buy order '%s' needs a positive price in G", pItem);
					continue;
				}

				SNpcBuy &Buy = pDef->m_lBuys.emplace();
				Buy.m_Item = CItemID::FromName(pItem);
				Buy.m_Count = maximum(1, ReadInt(rBuy["count"], 1));
				Buy.m_Price = Price;
				Buy.m_Limit = maximum(1, ReadInt(rBuy["limit"], 1));
				Buy.m_RestockSeconds = maximum(0, ReadInt(rBuy["restock_seconds"], 0));
				Buy.m_Demand = 0;
				Buy.m_RestockAt = 0;
			}
		}
	}

	// aggro tuning
	{
		const json_value &rAggro = (*pJson)["aggro"];
		if(rAggro.type == json_object)
		{
			pDef->m_Aggro.m_Threshold = ReadInt(rAggro["threshold"], 0);
			pDef->m_Aggro.m_PerKill = ReadInt(rAggro["per_kill"], 0);
			pDef->m_Aggro.m_PerHit = ReadInt(rAggro["per_hit"], 0);
			pDef->m_Aggro.m_KillDecay = ReadInt(rAggro["kill_decay"], 0);
			pDef->m_Aggro.m_DecaySeconds = maximum(0, ReadInt(rAggro["decay_seconds"], 10));
			pDef->m_Aggro.m_DecayAmount = maximum(1, ReadInt(rAggro["decay_amount"], 1));
		}
	}

	// A faction is explicit when present; otherwise the aggro rules decide:
	// an NPC that can gain hate is neutral, one that never fights is friendly.
	const char *pFaction = (*pJson)["faction"];
	if(pFaction[0])
		pDef->m_Faction = BotFactionFromString(pFaction, BOTFACTION_FRIENDLY);
	else
		pDef->m_Faction = pDef->m_Aggro.m_Threshold > 0 ? BOTFACTION_NEUTRAL : BOTFACTION_FRIENDLY;
	pDef->m_AttackHostiles = ReadBool((*pJson)["attack_hostiles"], pDef->m_Faction == BOTFACTION_NEUTRAL);

	CMobSystem::LoadDialogue((*pJson)["dialogue"], &pDef->m_Dialogue);

	const json_value &rAbilities = (*pJson)["abilities"];
	if(rAbilities.type == json_object)
	{
		const json_value &rHeal = rAbilities["heal_aura"];
		if(rHeal.type == json_object)
		{
			pDef->m_HealAura.m_Enabled = ReadBool(rHeal["enabled"], true);
			pDef->m_HealAura.m_Radius = maximum(16.0f, ReadFloat(rHeal["radius"], pDef->m_HealAura.m_Radius));
			pDef->m_HealAura.m_Amount = maximum(0, ReadInt(rHeal["amount"], pDef->m_HealAura.m_Amount));
			pDef->m_HealAura.m_Interval = maximum(1, ReadInt(rHeal["interval"], pDef->m_HealAura.m_Interval));
		}

		const json_value &rArmor = rAbilities["armor_aura"];
		if(rArmor.type == json_object)
		{
			pDef->m_ArmorAura.m_Enabled = ReadBool(rArmor["enabled"], true);
			pDef->m_ArmorAura.m_Radius = maximum(16.0f, ReadFloat(rArmor["radius"], pDef->m_ArmorAura.m_Radius));
			pDef->m_ArmorAura.m_Amount = maximum(0, ReadInt(rArmor["amount"], pDef->m_ArmorAura.m_Amount));
			pDef->m_ArmorAura.m_Interval = maximum(1, ReadInt(rArmor["interval"], pDef->m_ArmorAura.m_Interval));
		}
	}

	m_lDefs.add(pDef);
	dbg_msg("npc", "loaded npc '%s' ('%s') world '%s' pos %d,%d hp %d regen %d faction %s sells %d buys %d",
		Id.SourceName(), NpcIDDisplayName(Id), pDef->m_aWorld,
		TileX, TileY, pDef->m_Health, pDef->m_HealthRegen, BotFactionName(pDef->m_Faction),
		pDef->m_lSells.size(), pDef->m_lBuys.size());
}

void CNpcSystem::Init(CGameContext *pGameServer, IStorage *pStorage)
{
	m_pGameServer = pGameServer;
	m_pStorage = pStorage;

	Clear();
	m_pStorage->ListDirectory(IStorage::TYPE_ALL, "bots/npc", ListCallback, this);
	dbg_msg("npc", "loaded %d npc definition(s)", m_lDefs.size());

	PersistShelves();
}

void CNpcSystem::PersistShelves()
{
	if(!m_pGameServer || !m_pGameServer->Database())
		return;
	CDatabase *pDB = m_pGameServer->Database();

	for(int i = 0; i < m_lDefs.size(); i++)
	{
		SNpcDef *pDef = m_lDefs[i];
		pDB->ClearNpcSells(pDef->m_Id.Value());
		pDB->ClearNpcBuys(pDef->m_Id.Value());
		for(int s = 0; s < pDef->m_lSells.size(); s++)
		{
			SNpcSell &Sell = pDef->m_lSells[s];
			char aItem[64];
			ItemSourceName(Sell.m_Item, aItem, sizeof(aItem));
			pDB->SaveNpcSell(pDef->m_Id.Value(), aItem, Sell.m_Count, Sell.m_Price);
			LoadOrInitStock(m_lDefs[i], Sell);
		}
		for(int b = 0; b < pDef->m_lBuys.size(); b++)
		{
			SNpcBuy &Buy = pDef->m_lBuys[b];
			char aItem[64];
			ItemSourceName(Buy.m_Item, aItem, sizeof(aItem));
			pDB->SaveNpcBuy(pDef->m_Id.Value(), aItem, Buy.m_Count, Buy.m_Price, Buy.m_Limit, Buy.m_RestockSeconds);
			LoadOrInitBuyDemand(m_lDefs[i], Buy);
		}
	}
}

int CNpcSystem::RollStock(const SNpcSell &Sell) const
{
	if(Sell.m_StockMax <= Sell.m_StockMin)
		return Sell.m_StockMin;
	return Sell.m_StockMin + random_int() % (Sell.m_StockMax - Sell.m_StockMin + 1);
}

void CNpcSystem::PersistStock(const SNpcDef *pDef, const SNpcSell &Sell)
{
	CDatabase *pDB = m_pGameServer ? m_pGameServer->Database() : nullptr;
	if(!pDB)
		return;
	char aItem[64];
	ItemSourceName(Sell.m_Item, aItem, sizeof(aItem));
	pDB->SaveNpcStock(pDef->m_Id.Value(), aItem, Sell.m_Price, Sell.m_Stock, Sell.m_RestockAt);
}

void CNpcSystem::LoadOrInitStock(SNpcDef *pDef, SNpcSell &Sell)
{
	CDatabase *pDB = m_pGameServer ? m_pGameServer->Database() : nullptr;
	char aItem[64];
	ItemSourceName(Sell.m_Item, aItem, sizeof(aItem));

	int Stock = 0;
	int RestockAt = 0;
	if(pDB && pDB->GetNpcStock(pDef->m_Id.Value(), aItem, Sell.m_Price, &Stock, &RestockAt))
	{
		Sell.m_Stock = Stock;
		Sell.m_RestockAt = RestockAt;
		return;
	}

	// first sight of this entry: roll the opening stock and schedule the restock
	Sell.m_Stock = RollStock(Sell);
	if(Sell.m_RestockSeconds > 0)
		Sell.m_RestockAt = time_timestamp() + Sell.m_RestockSeconds;
	PersistStock(pDef, Sell);
}

void CNpcSystem::PersistBuyDemand(const SNpcDef *pDef, const SNpcBuy &Buy)
{
	CDatabase *pDB = m_pGameServer ? m_pGameServer->Database() : nullptr;
	if(!pDB)
		return;
	char aItem[64];
	ItemSourceName(Buy.m_Item, aItem, sizeof(aItem));
	pDB->SaveNpcBuyDemand(pDef->m_Id.Value(), aItem, Buy.m_Price, Buy.m_Demand, Buy.m_RestockAt);
}

void CNpcSystem::LoadOrInitBuyDemand(SNpcDef *pDef, SNpcBuy &Buy)
{
	CDatabase *pDB = m_pGameServer ? m_pGameServer->Database() : nullptr;
	char aItem[64];
	ItemSourceName(Buy.m_Item, aItem, sizeof(aItem));

	int Demand = 0;
	int RestockAt = 0;
	if(pDB && pDB->GetNpcBuyDemand(pDef->m_Id.Value(), aItem, Buy.m_Price, &Demand, &RestockAt))
	{
		Buy.m_Demand = Demand;
		Buy.m_RestockAt = RestockAt;
		return;
	}

	// first sight of this order: the full configured limit is available now
	Buy.m_Demand = Buy.m_Limit;
	if(Buy.m_RestockSeconds > 0)
		Buy.m_RestockAt = time_timestamp() + Buy.m_RestockSeconds;
	PersistBuyDemand(pDef, Buy);
}

bool CNpcSystem::RestockDue()
{
	if(!m_pGameServer || !m_pGameServer->Database())
		return false;

	const int Now = time_timestamp();
	bool Changed = false;
	for(int i = 0; i < m_lDefs.size(); i++)
	{
		SNpcDef *pDef = m_lDefs[i];
		for(int s = 0; s < pDef->m_lSells.size(); s++)
		{
			SNpcSell &Sell = pDef->m_lSells[s];
			if(Sell.m_RestockSeconds <= 0 || Now < Sell.m_RestockAt)
				continue;

			// a still larger remaining stock is kept; only a low one is topped up
			const int Rolled = RollStock(Sell);
			if(Rolled > Sell.m_Stock)
				Sell.m_Stock = Rolled;
			Sell.m_RestockAt = Now + Sell.m_RestockSeconds;
			PersistStock(pDef, Sell);
			Changed = true;
		}
		for(int b = 0; b < pDef->m_lBuys.size(); b++)
		{
			SNpcBuy &Buy = pDef->m_lBuys[b];
			if(Buy.m_RestockSeconds <= 0 || Now < Buy.m_RestockAt)
				continue;

			// a new restock period means the merchant accepts its full limit
			// again; the persisted demand keeps it honest across restarts too
			Buy.m_Demand = Buy.m_Limit;
			Buy.m_RestockAt = Now + Buy.m_RestockSeconds;
			PersistBuyDemand(pDef, Buy);
			Changed = true;
		}
	}
	return Changed;
}

bool CNpcSystem::ConsumeStock(int DefIndex, int SellIndex, int Count)
{
	if(DefIndex < 0 || DefIndex >= m_lDefs.size() || Count <= 0)
		return false;
	SNpcDef *pDef = m_lDefs[DefIndex];
	if(SellIndex < 0 || SellIndex >= pDef->m_lSells.size())
		return false;

	SNpcSell &Sell = pDef->m_lSells[SellIndex];
	if(Sell.m_Stock < Count)
		return false;
	Sell.m_Stock -= Count;
	PersistStock(pDef, Sell);
	return true;
}

bool CNpcSystem::ConsumeBuyDemand(int DefIndex, int BuyIndex)
{
	if(DefIndex < 0 || DefIndex >= m_lDefs.size())
		return false;
	SNpcDef *pDef = m_lDefs[DefIndex];
	if(BuyIndex < 0 || BuyIndex >= pDef->m_lBuys.size())
		return false;

	SNpcBuy &Buy = pDef->m_lBuys[BuyIndex];
	if(Buy.m_Demand <= 0)
		return false;
	Buy.m_Demand--;
	PersistBuyDemand(pDef, Buy);
	return true;
}

const SNpcDef *CNpcSystem::Find(const CNpcID &Id) const
{
	if(Id.IsZero())
		return 0;
	for(int i = 0; i < m_lDefs.size(); i++)
		if(m_lDefs[i]->m_Id == Id)
			return m_lDefs[i];
	return 0;
}
