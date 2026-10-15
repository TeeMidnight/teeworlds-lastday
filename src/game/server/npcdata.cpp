/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>

#include <engine/shared/jsonparser.h>
#include <engine/storage.h>

#include <game/server/database/database.h>

#include "gamecontext.h"
#include "npcdata.h"

namespace
{
const char *gs_pSkinParts[NUM_SKINPARTS] = {"body", "marking", "decoration", "hands", "feet", "eyes"};

// The TeeInfo skin part colours the engine hands to clients carry H, S and L in
// the low 24 bits (the hardcoded bot skins use the same layout); alpha is not
// part of the value.
int PackHsla(int Hue, int Sat, int Lgt)
{
	return ((Hue & 0xff) << 16) | ((Sat & 0xff) << 8) | (Lgt & 0xff);
}

int ReadInt(const json_value &Value, int Default)
{
	if(Value.type == json_integer || Value.type == json_double)
		return (int) (json_int_t) Value;
	return Default;
}

// the persistable name of an item id: its namespaced source, or the uuid
void ItemSourceName(const CItemID &Id, char *pOut, int Size)
{
	const char *pSource = Id.SourceName();
	if(pSource && pSource[0])
		str_copy(pOut, pSource, Size);
	else
		Id.FormatUuid(pOut, Size);
}
} // namespace

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
					PackHsla(ReadInt(rPart["hue"], 0), ReadInt(rPart["sat"], 0), ReadInt(rPart["lgt"], 0)) : 0;
			}
		}
	}

	// shelf: what this NPC sells; the price is another item
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

				SNpcSell &Sell = pDef->m_lSells.emplace();
				Sell.m_Item = CItemID::FromName(pItem);
				Sell.m_Count = maximum(1, ReadInt(rSell["count"], 1));
				const char *pPrice = rSell["price_item"];
				Sell.m_PriceItem = CItemID::FromName(pPrice);
				Sell.m_PriceCount = maximum(0, ReadInt(rSell["price_count"], 0));

				// restock rules; without a restock interval the stock never changes
				Sell.m_StockMin = maximum(0, ReadInt(rSell["stock_min"], Sell.m_Count));
				Sell.m_StockMax = maximum(Sell.m_StockMin, ReadInt(rSell["stock_max"], Sell.m_StockMin));
				Sell.m_RestockSeconds = maximum(0, ReadInt(rSell["restock_seconds"], 0));
				Sell.m_Stock = 0;
				Sell.m_RestockAt = 0;
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
		}
	}

	m_lDefs.add(pDef);
	dbg_msg("npc", "loaded npc '%s' ('%s') world '%s' pos %d,%d hp %d regen %d sells %d",
		Id.SourceName(), NpcIDDisplayName(Id), pDef->m_aWorld,
		TileX, TileY, pDef->m_Health, pDef->m_HealthRegen, pDef->m_lSells.size());
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
		for(int s = 0; s < pDef->m_lSells.size(); s++)
		{
			SNpcSell &Sell = pDef->m_lSells[s];
			char aItem[64];
			char aPrice[64];
			ItemSourceName(Sell.m_Item, aItem, sizeof(aItem));
			ItemSourceName(Sell.m_PriceItem, aPrice, sizeof(aPrice));
			pDB->SaveNpcSell(pDef->m_Id.Value(), aItem, Sell.m_Count, aPrice, Sell.m_PriceCount);
			LoadOrInitStock(m_lDefs[i], Sell);
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
	char aPrice[64];
	ItemSourceName(Sell.m_Item, aItem, sizeof(aItem));
	ItemSourceName(Sell.m_PriceItem, aPrice, sizeof(aPrice));
	pDB->SaveNpcStock(pDef->m_Id.Value(), aItem, aPrice, Sell.m_Stock, Sell.m_RestockAt);
}

void CNpcSystem::LoadOrInitStock(SNpcDef *pDef, SNpcSell &Sell)
{
	CDatabase *pDB = m_pGameServer ? m_pGameServer->Database() : nullptr;
	char aItem[64];
	char aPrice[64];
	ItemSourceName(Sell.m_Item, aItem, sizeof(aItem));
	ItemSourceName(Sell.m_PriceItem, aPrice, sizeof(aPrice));

	int Stock = 0;
	int RestockAt = 0;
	if(pDB && pDB->GetNpcStock(pDef->m_Id.Value(), aItem, aPrice, &Stock, &RestockAt))
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

const SNpcDef *CNpcSystem::Find(const CNpcID &Id) const
{
	if(Id.IsZero())
		return 0;
	for(int i = 0; i < m_lDefs.size(); i++)
		if(m_lDefs[i]->m_Id == Id)
			return m_lDefs[i];
	return 0;
}
