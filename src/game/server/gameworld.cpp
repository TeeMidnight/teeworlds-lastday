/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */

#include <base/math.h>
#include <base/tl/stream.h>

#include <generated/server_data.h>

#include <engine/server/mapgen.h>
#include <engine/server/worldgen.h>
#include <engine/shared/jsonparser.h>
#include <engine/shared/jsonwriter.h>

#include <climits>

#include "botmanager.h"
#include "entities/botentity.h"
#include "entities/character.h"
#include "entities/drill.h"
#include "entities/dropped_pickup.h"
#include "entities/resource.h"
#include "entity.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "gameworld.h"
#include "item.h"

#include <game/server/database/database.h>

#include <algorithm>

//////////////////////////////////////////////////
// game world
//////////////////////////////////////////////////
CGameWorld::CGameWorld(CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
	m_pConfig = m_pGameServer->Config();
	m_pServer = m_pGameServer->Server();
	m_Events.SetGameServer(pGameServer);

	m_aMapName[0] = '\0';
	m_BotConfigured = false;
	m_BotNpcRole = false;
	m_Ring = false;
	m_RingSeed = 0;
	m_RingSize = 0;
	m_SharedMargin = 0;
	m_OffsetX = 0;
	m_Stride = 0;
	m_Period = 0;
	mem_zero(m_apRingProxyOwner, sizeof(m_apRingProxyOwner));

	for(int i = 0; i < NUM_ENTTYPES; i++)
	{
		m_alpEntityLists[i].hint_size(16);
	}
	m_lpFlagEntityList.hint_size(8);
}

float CGameWorld::RingRightTriggerX() const
{
	return RingTriggerX(m_Collision.GetWidth(), m_SharedMargin, 1);
}

float CGameWorld::RingLeftTriggerX() const
{
	return RingTriggerX(m_Collision.GetWidth(), m_SharedMargin, -1);
}

bool CGameWorld::RingNeighbour(int Direction, char *pMapName, int MapNameSize, float LocalX, float *pArrivalX) const
{
	if(!m_Ring || Direction == 0 || !m_aMapName[0])
		return false;

	// the floor name is the map name without its trailing "_<seed>"
	char aFloor[64];
	str_copy(aFloor, m_aMapName, sizeof(aFloor));
	char *pUnderscore = nullptr;
	for(char *p = aFloor; *p; p++)
	{
		if(*p == '_')
			pUnderscore = p;
	}
	if(!pUnderscore)
		return false;
	*pUnderscore = '\0';

	const int Target = RingNeighbourSeed(m_RingSeed, Direction, m_RingSize);

	str_format(pMapName, MapNameSize, "%s_%d", aFloor, Target);
	if(pArrivalX)
		*pArrivalX = RingArrivalX(m_Stride, Direction, LocalX);
	return true;
}

int CGameWorld::CollectRingNeighbours(SRingNeighbour *pOut, int Max)
{
	if(!m_Ring || m_Period <= 0 || Max <= 0)
		return 0;

	int Count = 0;
	for(int Dir = -1; Dir <= 1 && Count < Max; Dir += 2)
	{
		char aName[64];
		if(!RingNeighbour(Dir, aName, sizeof(aName), 0.0f, nullptr))
			continue;
		CGameWorld *pNeighbour = GameServer()->WorldByID(str_quickhash(aName));
		if(!pNeighbour || pNeighbour == this)
			continue;
		const float ShiftX = (float) RingNeighbourShift(m_OffsetX, pNeighbour->m_OffsetX, m_Period) * 32.0f;
		pOut[Count].m_pWorld = pNeighbour;
		pOut[Count].m_Offset = vec2(ShiftX, 0.0f);
		Count++;
	}
	return Count;
}

vec2 CGameWorld::PosInFrame(CEntity *pEnt)
{
	if(!pEnt)
		return vec2(0.0f, 0.0f);
	if(pEnt->GameWorld() == this)
		return pEnt->GetPos();

	SRingNeighbour aNeighbours[2];
	const int Neighbours = CollectRingNeighbours(aNeighbours, 2);
	for(int n = 0; n < Neighbours; n++)
	{
		if(aNeighbours[n].m_pWorld == pEnt->GameWorld())
			return pEnt->GetPos() + aNeighbours[n].m_Offset;
	}
	return pEnt->GetPos();
}

int CGameWorld::FindBotRingProxy(int TeeID)
{
	for(int i = 0; i < m_lBotRingProxies.size(); i++)
		if(m_lBotRingProxies[i].m_TeeID == TeeID)
			return i;
	return -1;
}

void CGameWorld::RefreshRingProxies()
{
	if(!m_Ring)
		return;

	bool aSeen[MAX_CLIENTS];
	mem_zero(aSeen, sizeof(aSeen));

	SRingNeighbour aNeighbours[2];
	const int Neighbours = CollectRingNeighbours(aNeighbours, 2);
	for(int n = 0; n < Neighbours; n++)
	{
		CGameWorld *pNeighbour = aNeighbours[n].m_pWorld;
		const vec2 Offset = aNeighbours[n].m_Offset;
		for(auto &pEnt : pNeighbour->m_alpEntityLists[ENTTYPE_CHARACTER])
		{
			CCharacter *pChr = static_cast<CCharacter *>(pEnt);
			if(!pChr || !pChr->IsAlive())
				continue;
			const int CID = pChr->GetCID();
			if(CID < 0 || CID >= MAX_CLIENTS)
				continue;
			// a real character of this world would own the id; only ever take a
			// free slot or our own proxy
			CCharacterCore *pExisting = m_Core.GetCharacter(CID);
			if(pExisting && pExisting != &m_aRingProxyCores[CID])
				continue;
			if(pExisting != &m_aRingProxyCores[CID] || m_apRingProxyOwner[CID] != pNeighbour)
			{
				m_aRingProxyCores[CID].Init(&m_Core, &m_Collision);
				m_aRingProxyCores[CID].Reset();
				m_Core.SetCharacter(CID, &m_aRingProxyCores[CID]);
				m_apRingProxyOwner[CID] = pNeighbour;
			}
			m_aRingProxyCores[CID].m_Pos = pChr->CorePos() + Offset;
			aSeen[CID] = true;
		}
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apRingProxyOwner[i] && !aSeen[i])
		{
			if(m_Core.GetCharacter(i) == &m_aRingProxyCores[i])
				m_Core.SetCharacter(i, nullptr);
			m_apRingProxyOwner[i] = nullptr;
		}
	}

	// Bot proxies: keyed by the bot's globally unique private tee id. That id is
	// already in the server's core space and the manager's
	// TranslateTeeIDForClient knows it, so a hook that grabs the proxy names the
	// right bot without a second translation table.
	for(int i = 0; i < m_lBotRingProxies.size(); i++)
		m_lBotRingProxies[i].m_Seen = false;

	for(int n = 0; n < Neighbours; n++)
	{
		CGameWorld *pNeighbour = aNeighbours[n].m_pWorld;
		const vec2 Offset = aNeighbours[n].m_Offset;
		for(auto &pEnt : pNeighbour->m_alpEntityLists[ENTTYPE_BOTENTITY])
		{
			IBotEntity *pBot = static_cast<IBotEntity *>(pEnt);
			if(!pBot || !pBot->IsAlive())
				continue;
			const int TeeID = pBot->TeeID();
			if(TeeID < 0)
				continue;

			int Index = FindBotRingProxy(TeeID);
			if(Index < 0)
			{
				SBotRingProxy New;
				New.m_pCore = new CCharacterCore();
				New.m_pCore->Init(&m_Core, &m_Collision);
				New.m_pCore->Reset();
				New.m_pOwner = pNeighbour;
				New.m_TeeID = TeeID;
				New.m_Seen = true;
				Index = m_lBotRingProxies.add(New);
			}

			SBotRingProxy &Proxy = m_lBotRingProxies[Index];
			CCharacterCore *pExisting = m_Core.GetCharacter(TeeID);
			// never steal an id another core already owns; the entry then stays
			// unseen and is dropped below
			if(pExisting && pExisting != Proxy.m_pCore)
				continue;
			if(pExisting != Proxy.m_pCore)
				m_Core.SetCharacter(TeeID, Proxy.m_pCore);
			Proxy.m_pOwner = pNeighbour;
			Proxy.m_Seen = true;
			Proxy.m_pCore->m_Pos = pBot->GetPos() + Offset;
		}
	}

	for(int i = m_lBotRingProxies.size() - 1; i >= 0; i--)
	{
		if(m_lBotRingProxies[i].m_Seen)
			continue;
		const int TeeID = m_lBotRingProxies[i].m_TeeID;
		if(m_Core.GetCharacter(TeeID) == m_lBotRingProxies[i].m_pCore)
			m_Core.SetCharacter(TeeID, nullptr);
		delete m_lBotRingProxies[i].m_pCore;
		m_lBotRingProxies.remove_index(i);
	}
}

void CGameWorld::ForwardRingProxyDrag()
{
	if(!m_Ring)
		return;

	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		if(!m_apRingProxyOwner[c])
			continue;
		CCharacterCore &Proxy = m_aRingProxyCores[c];
		if(Proxy.m_HookDragVel.x == 0.0f && Proxy.m_HookDragVel.y == 0.0f)
			continue;

		CGameWorld *pOwner = m_apRingProxyOwner[c];
		for(auto &pEnt : pOwner->m_alpEntityLists[ENTTYPE_CHARACTER])
		{
			CCharacter *pChr = static_cast<CCharacter *>(pEnt);
			if(pChr && pChr->IsAlive() && pChr->GetCID() == c)
			{
				pChr->AddHookDragVel(Proxy.m_HookDragVel);
				break;
			}
		}
		Proxy.m_HookDragVel = vec2(0.0f, 0.0f);
	}

	// the same for a borrowed neighbour bot: add the drag it took to the real
	// bot's core, which applies it in its own TickDefered
	for(int i = 0; i < m_lBotRingProxies.size(); i++)
	{
		SBotRingProxy &Proxy = m_lBotRingProxies[i];
		if(!Proxy.m_pOwner)
			continue;
		if(Proxy.m_pCore->m_HookDragVel.x == 0.0f && Proxy.m_pCore->m_HookDragVel.y == 0.0f)
			continue;

		CGameWorld *pOwner = Proxy.m_pOwner;
		for(auto &pEnt : pOwner->m_alpEntityLists[ENTTYPE_BOTENTITY])
		{
			IBotEntity *pBot = static_cast<IBotEntity *>(pEnt);
			if(pBot && pBot->TeeID() == Proxy.m_TeeID)
			{
				pBot->Core()->m_HookDragVel += Proxy.m_pCore->m_HookDragVel;
				break;
			}
		}
		Proxy.m_pCore->m_HookDragVel = vec2(0.0f, 0.0f);
	}
}

CGameWorld::~CGameWorld()
{
	// The global manager owns the bots as entities, so retire this world's
	// before the entity lists are torn down: a bot's destructor unbinds its
	// core from this world and must not run after that core is gone.
	if(class CBotManager *pBots = GameServer()->BotManager())
		pBots->ClearBots(this);

	// release the borrowed neighbour bots before this world's core dies
	for(int i = 0; i < m_lBotRingProxies.size(); i++)
	{
		if(m_Core.GetCharacter(m_lBotRingProxies[i].m_TeeID) == m_lBotRingProxies[i].m_pCore)
			m_Core.SetCharacter(m_lBotRingProxies[i].m_TeeID, nullptr);
		delete m_lBotRingProxies[i].m_pCore;
	}
	m_lBotRingProxies.clear();

	// delete all entities
	for(int i = 0; i < NUM_ENTTYPES; i++)
		while(m_alpEntityLists[i].size())
			delete m_alpEntityLists[i][0];
}

CBotManager *CGameWorld::BotManager()
{
	return GameServer()->BotManager();
}

CBotManager *CGameWorld::GetBotManager() const
{
	return GameServer()->BotManager();
}

bool CGameWorld::HasBotManager() const
{
	return GameServer()->BotManager() != nullptr;
}

int CGameWorld::RingNeighbours(CGameWorld **ppWorlds, vec2 *pOffsets, int Max)
{
	if(!ppWorlds || Max <= 0)
		return 0;

	SRingNeighbour aNeighbours[2];
	const int Count = CollectRingNeighbours(aNeighbours, minimum(Max, 2));
	for(int i = 0; i < Count; i++)
	{
		ppWorlds[i] = aNeighbours[i].m_pWorld;
		if(pOffsets)
			pOffsets[i] = aNeighbours[i].m_Offset;
	}
	return Count;
}

void CGameWorld::InitCollision(IMap *pMap)
{
	m_Layers.Init(0, pMap);
	m_Collision.Init(&m_Layers);

	// create all entities from the game layer
	CMapItemLayerTilemap *pTileMap = m_Layers.GameLayer();
	CTile *pTiles = (CTile *) pMap->GetData(pTileMap->m_Data);
	for(int y = 0; y < pTileMap->m_Height; y++)
	{
		for(int x = 0; x < pTileMap->m_Width; x++)
		{
			int Index = pTiles[y * pTileMap->m_Width + x].m_Index;

			if(Index > TILE_UNHOOKABLE && Index < ENTITY_OFFSET)
			{
				vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
				GameServer()->m_pController->OnExtraTile(this, Index, Pos);
			}
			if(Index >= ENTITY_OFFSET)
			{
				vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
				GameServer()->m_pController->OnEntity(this, Index - ENTITY_OFFSET, Pos);
			}
		}
	}

	CMapItemInfo *pItem = (CMapItemInfo *) pMap->FindItem(MAPITEMTYPE_INFO, 0);
	if(pItem && pItem->m_Version == 1)
	{
		if(pItem->m_MapVersion > -1)
		{
			// classic mechanism: the MapVersion directly names the entrance
			// target map (main world, e.g. Connector)
			str_copy(m_aEntrances[0], (char *) pMap->GetData(pItem->m_MapVersion), sizeof(m_aEntrances[0]));
		}
		if(pItem->m_Credits > -1)
			str_copy(m_aEntrances[1], (char *) pMap->GetData(pItem->m_Credits), sizeof(m_aEntrances[1]));
	}

	// Generated maps keep their metadata in a sidecar file next to the map
	// ("<GENERATED_MAPS_DIR>/<name>.json") instead of embedding json items, so
	// the map clients download only carries what they render. Older generated maps
	// still embed it; the item reads below are the fallback.
	bool HasMeta = false;
	array<char> aMeta;
	if(LoadGeneratedWorldMeta(GameServer()->Storage(), m_aMapName, aMeta))
	{
		CJsonParser MetaParser;
		json_value *pMeta = MetaParser.ParseString(aMeta.base_ptr(), "world meta");
		if(pMeta && pMeta->type == json_object)
		{
			HasMeta = true;
			ParseEntrances((*pMeta)["entrances"]);
			ApplyWorldJson((*pMeta)["world"]);
			SpawnResources((*pMeta)["resources"]);
		}
	}
	if(HasMeta)
		return;

	// legacy generated maps carry the entrance definitions as a map json item
	CMapItemJson *pJsonItem = (CMapItemJson *) pMap->FindItem(MAPITEMTYPE_JSON, 0);
	if(pJsonItem && pJsonItem->m_Version == CMapItemJson::CURRENT_VERSION && pJsonItem->m_Data > -1)
	{
		const char *pJsonData = (const char *) pMap->GetData(pJsonItem->m_Data);
		if(pJsonData)
			ParseEntrances(pJsonData);
	}

	// legacy generated maps declare their world geometry as a third map json
	// item (id 2); the wrap only changes how tile lookups read
	CMapItemJson *pWorldItem = (CMapItemJson *) pMap->FindItem(MAPITEMTYPE_JSON, 2);
	if(pWorldItem && pWorldItem->m_Version == CMapItemJson::CURRENT_VERSION && pWorldItem->m_Data > -1)
	{
		const char *pJsonData = (const char *) pMap->GetData(pWorldItem->m_Data);
		if(pJsonData)
		{
			CJsonParser Parser;
			json_value *pJson = Parser.ParseString(pJsonData, "world");
			if(pJson)
				ApplyWorldJson(*pJson);
		}
	}

	// generated worlds carry their resource placement as a second json
	// item (id 1); spawn a resource entity for every entry
	CMapItemJson *pResourceItem = (CMapItemJson *) pMap->FindItem(MAPITEMTYPE_JSON, 1);
	if(pResourceItem && pResourceItem->m_Version == CMapItemJson::CURRENT_VERSION && pResourceItem->m_Data > -1)
	{
		const char *pJsonData = (const char *) pMap->GetData(pResourceItem->m_Data);
		if(pJsonData)
			SpawnResources(pJsonData);
	}
}

void CGameWorld::ApplyWorldJson(const json_value &rWorld)
{
	if(rWorld.type != json_object)
		return;

	const json_value &rWrap = rWorld["wrap_x"];
	const bool Wrap = rWrap.type == json_integer && rWrap.u.integer != 0;
	m_Collision.SetWrapX(Wrap);

	// a ring instance opens into its neighbours instead of wrapping on itself;
	// the geometry it records is what resolves a crossing
	const json_value &rRing = rWorld["ring"];
	m_Ring = rRing.type == json_integer && rRing.u.integer != 0;
	if(m_Ring)
	{
		m_RingSeed = (int) (json_int_t) rWorld["ring_seed"];
		m_RingSize = (int) (json_int_t) rWorld["ring_size"];
		m_SharedMargin = (int) (json_int_t) rWorld["shared_margin"];
		m_OffsetX = (int) (json_int_t) rWorld["offset_x"];
		m_Period = (int) (json_int_t) rWorld["period"];
		m_Stride = m_Collision.GetWidth() - m_SharedMargin;
		// A stale or hand edited metadata set must not leave the world in a
		// state where the two triggers overlap or the ring cannot close;
		// fall back to a plain map instead.
		if(m_Stride <= 0 || m_SharedMargin <= 0 || m_RingSize < 2 || m_Period <= 0)
		{
			dbg_msg("game", "ignoring invalid ring geometry (%d/%d, margin %d, stride %d, period %d)",
				m_RingSeed, m_RingSize, m_SharedMargin, m_Stride, m_Period);
			m_Ring = false;
		}
		else
		{
			dbg_msg("game", "world ring %d/%d, shared margin %d, stride %d, period %d",
				m_RingSeed, m_RingSize, m_SharedMargin, m_Stride, m_Period);
		}
	}
	else
	{
		dbg_msg("game", "world wrap_x = %d", Wrap ? 1 : 0);
	}
}

void CGameWorld::SpawnResources(const json_value &rResources)
{
	if(rResources.type != json_array)
		return;
	for(unsigned i = 0; i < rResources.u.array.length; i++)
	{
		const json_value &rRes = rResources[i];
		if(rRes.type != json_object)
			continue;
		const json_value &rId = rRes["res_id"];
		if(rId.type != json_string)
			continue;

		const int X = (int) (json_int_t) rRes["pos_x"];
		const int Y = (int) (json_int_t) rRes["pos_y"];
		const int Hardness = (int) (json_int_t) rRes["hardness"];
		const int RespawnTime = (int) (json_int_t) rRes["respawntime"];
		const char *pDisplay = rRes["display"];

		new CResourceEntity(this, vec2(X * 32.0f + 16.0f, Y * 32.0f + 16.0f),
			rId.u.string.ptr, pDisplay, Hardness, RespawnTime);
	}
}

void CGameWorld::SpawnResources(const char *pJsonData)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseString(pJsonData, "resources");
	if(pJson)
		SpawnResources(*pJson);
}

void CGameWorld::ParseEntrances(const json_value &rEntrances)
{
	if(rEntrances.type != json_array)
		return;
	for(unsigned i = 0; i < rEntrances.u.array.length; i++)
	{
		const json_value &rEntrance = rEntrances[i];
		if(rEntrance.type != json_object)
			continue;
		const json_value &rTarget = rEntrance["entrance"];
		if(rTarget.type != json_string)
			continue;

		const json_value &rTiles = rEntrance["tiles"];
		for(unsigned t = 0; t < rTiles.u.array.length; t++)
		{
			const json_value &rRect = rTiles[t];
			if(rRect.type != json_object)
				continue;

			// a string value ("ignore") means this axis is not restricted at
			// all: entrances outside the map bounds are recognized as well
			const json_value &rStartX = rRect["tiles_start_x"];
			const json_value &rStartY = rRect["tiles_start_y"];
			const json_value &rEndX = rRect["tiles_end_x"];
			const json_value &rEndY = rRect["tiles_end_y"];

			int StartX = rStartX.type == json_string ? INT_MIN : (int) (json_int_t) rStartX;
			int StartY = rStartY.type == json_string ? INT_MIN : (int) (json_int_t) rStartY;
			int EndX = rEndX.type == json_string ? INT_MAX : (int) (json_int_t) rEndX;
			int EndY = rEndY.type == json_string ? INT_MAX : (int) (json_int_t) rEndY;

			// make sure start <= end
			if(StartX > EndX)
				std::swap(StartX, EndX);
			if(StartY > EndY)
				std::swap(StartY, EndY);

			CEntranceInfo &Entrance = m_lEntrances.emplace();
			Entrance.m_StartX = StartX;
			Entrance.m_StartY = StartY;
			Entrance.m_EndX = EndX;
			Entrance.m_EndY = EndY;
			str_copy(Entrance.m_aTargetMap, rTarget.u.string.ptr, sizeof(Entrance.m_aTargetMap));
		}
	}
}

void CGameWorld::ParseEntrances(const char *pJsonData)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseString(pJsonData, "entrances");
	if(pJson)
		ParseEntrances(*pJson);
}

CGameWorld::TypeRange CGameWorld::DoTypeRange(int Type)
{
	dbg_assert(Type >= 0 && Type < NUM_ENTTYPES, "out of range");
	return m_alpEntityLists[Type].all();
}

CGameWorld::FlagRange CGameWorld::DoFlagRange(int Flag)
{
	return FlagRange(m_lpFlagEntityList.all(), CFlagCheck(Flag));
}

int CGameWorld::FindEntities(vec2 Pos, float Radius, array<CEntity *> &lpEnts, int Type)
{
	if(Type < 0 || Type >= NUM_ENTTYPES)
		return 0;

	int Num = 0;
	for(auto &pEnt : m_alpEntityLists[Type])
	{
		if(distance(pEnt->m_Pos, Pos) < Radius + pEnt->m_ProximityRadius)
		{
			lpEnts.add(pEnt);
			Num++;
		}
	}

	// Ring: a resource node in the shared strip has a single owner (the
	// generator only spawns it on the side whose local column is past the
	// margin), so scanning the neighbour is how a node that physically sits in
	// this world's left strip is reached from here.
	{
		SRingNeighbour aNeighbours[2];
		const int Neighbours = CollectRingNeighbours(aNeighbours, 2);
		for(int n = 0; n < Neighbours; n++)
		{
			const vec2 Offset = aNeighbours[n].m_Offset;
			for(auto &pEnt : aNeighbours[n].m_pWorld->m_alpEntityLists[Type])
			{
				if(distance(pEnt->m_Pos + Offset, Pos) < Radius + pEnt->GetProximityRadius())
				{
					lpEnts.add(pEnt);
					Num++;
				}
			}
		}
	}

	return Num;
}

int CGameWorld::FindFlagEntities(vec2 Pos, float Radius, array<CEntity *> &lpEnts, int Flag)
{
	CFlagCheck Check(Flag);
	int Num = 0;
	for(auto &pEnt : m_lpFlagEntityList)
	{
		if(!Check(pEnt)) continue;
		if(distance(pEnt->m_Pos, Pos) < Radius + pEnt->m_ProximityRadius)
		{
			lpEnts.add(pEnt);
			Num++;
		}
	}

	// see FindEntities: the boundary strip is shared with the ring neighbours
	SRingNeighbour aNeighbours[2];
	const int Neighbours = CollectRingNeighbours(aNeighbours, 2);
	for(int n = 0; n < Neighbours; n++)
	{
		const vec2 Offset = aNeighbours[n].m_Offset;
		for(auto &pEnt : aNeighbours[n].m_pWorld->m_lpFlagEntityList)
		{
			if(!Check(pEnt)) continue;
			if(distance(pEnt->m_Pos + Offset, Pos) < Radius + pEnt->GetProximityRadius())
			{
				lpEnts.add(pEnt);
				Num++;
			}
		}
	}

	return Num;
}

void CGameWorld::InsertEntity(CEntity *pEnt)
{
	m_alpEntityLists[pEnt->m_ObjType].add(pEnt);
	if(pEnt->ObjFlag() != 0)
		m_lpFlagEntityList.add(pEnt);
}

void CGameWorld::DestroyEntity(CEntity *pEnt)
{
	pEnt->MarkForDestroy();
}

void CGameWorld::RemoveEntity(CEntity *pEnt)
{
	m_alpEntityLists[pEnt->m_ObjType].remove_fast(pEnt);
	m_lpFlagEntityList.remove_fast(pEnt);

	if(m_alpEntityLists[pEnt->m_ObjType].size() > 32 && m_alpEntityLists[pEnt->m_ObjType].used_memory() < m_alpEntityLists[pEnt->m_ObjType].memusage() / 3) // lower than 1/3
	{
		m_alpEntityLists[pEnt->m_ObjType].optimize();
	}
	if(m_lpFlagEntityList.size() > 32 && m_lpFlagEntityList.used_memory() < m_lpFlagEntityList.memusage() / 3) // lower than 1/3
	{
		m_lpFlagEntityList.optimize();
	}
}

//
void CGameWorld::Snap(int SnappingClient)
{
	for(int i = 0; i < NUM_ENTTYPES; i++)
		for(auto &pEnt : m_alpEntityLists[i])
			pEnt->Snap(SnappingClient);
	m_Events.Snap(SnappingClient);

	// Ring: the live neighbours share the strip around the boundary, so their
	// entities there belong in this snapshot too, shifted onto this map's axis.
	// Every entity type is mirrored: the entity shifts itself (and whatever it
	// renders from) for the call, so nothing has to be listed here. A type whose
	// per client identity is unknown (a bot the client was not told about)
	// simply renders nothing.
	if(m_Ring && SnappingClient != -1 && m_Period > 0)
	{
		for(int Dir = -1; Dir <= 1; Dir += 2)
		{
			char aName[64];
			if(!RingNeighbour(Dir, aName, sizeof(aName), 0.0f, nullptr))
				continue;
			CGameWorld *pNeighbour = GameServer()->WorldByID(str_quickhash(aName));
			if(!pNeighbour || pNeighbour == this)
				continue;

			const float ShiftX = (float) RingNeighbourShift(m_OffsetX, pNeighbour->m_OffsetX, m_Period) * 32.0f;
			const vec2 Offset(ShiftX, 0.0f);
			for(int t = 0; t < NUM_ENTTYPES; t++)
			{
				for(auto &pEnt : pNeighbour->m_alpEntityLists[t])
				{
					pEnt->ShiftForSnap(Offset);
					pEnt->Snap(SnappingClient);
					pEnt->ShiftForSnap(-Offset);
				}
			}
		}
	}
}

void CGameWorld::PostSnap()
{
	for(int i = 0; i < NUM_ENTTYPES; i++)
		for(auto &pEnt : m_alpEntityLists[i])
			pEnt->PostSnap();
	m_Events.Clear();
}

void CGameWorld::RemoveEntities()
{
	// destroy objects marked for destruction
	for(int i = 0; i < NUM_ENTTYPES; i++)
		for(int j = 0; j < m_alpEntityLists[i].size(); j++)
		{
			if(m_alpEntityLists[i][j]->IsMarkedForDestroy())
			{
				m_alpEntityLists[i][j]->Destroy();
				j--;
			}
		}
}

void CGameWorld::Tick()
{
	// update all objects by index, not with a range: an entity may insert
	// another one into the same list while it ticks (a laser refracts, a
	// projectile fragments). array::add reallocates and invalidates a captured
	// range, while operator[] re-reads the base pointer on every step. Removal
	// uses remove_fast, which swaps the last element down; the index walk may
	// skip that element for one frame but never visits it twice.
	for(int i = 0; i < NUM_ENTTYPES; i++)
		for(int j = 0; j < m_alpEntityLists[i].size(); j++)
			m_alpEntityLists[i][j]->Tick();

	for(int i = 0; i < NUM_ENTTYPES; i++)
		for(int j = 0; j < m_alpEntityLists[i].size(); j++)
			m_alpEntityLists[i][j]->TickDefered();

	// Ring: an entity that crossed a boundary trigger belongs to the
	// neighbouring instance now, so a thrown item or a projectile continues its
	// flight instead of stopping at an edge that is not there. The crossing is
	// collected first and moved after the list was scanned. The neighbour has to
	// be live; when it is not, the entity waits here and moves as soon as
	// someone is in the neighbour (its world is then loaded).
	if(IsRing())
	{
		const float RightX = RingRightTriggerX();
		const float LeftX = RingLeftTriggerX();
		array<CEntity *> lCrossing;
		// generic: every type is scanned and each entity decides whether it
		// crosses. Stationary entities (resources, flags, lasers) do not opt in.
		for(int t = 0; t < NUM_ENTTYPES; t++)
		{
			for(auto &pEnt : m_alpEntityLists[t])
			{
				if(!pEnt->RingCrosses())
					continue;
				const float X = pEnt->RingPos().x;
				if(RingCrossDirection(X, RightX, LeftX) != 0)
					lCrossing.add(pEnt);
			}
		}
		for(int i = 0; i < lCrossing.size(); i++)
		{
			CEntity *pEnt = lCrossing[i];
			const float X = pEnt->RingPos().x;
			const int Direction = RingCrossDirection(X, RightX, LeftX);
			char aTarget[64];
			float ArrivalX = 0.0f;
			if(!RingNeighbour(Direction, aTarget, sizeof(aTarget), X, &ArrivalX))
				continue;
			// A bot is owned by a manager and bound in the world core, so it is
			// handed over rather than moved like a plain entity. It is only
			// crossed into a neighbour that is already live: a world with no
			// players should not be pulled in just because a wandering bot
			// reached the boundary.
			const bool IsBot = pEnt->ObjType() == ENTTYPE_BOTENTITY;
			CGameWorld *pTarget = GameServer()->WorldByID(str_quickhash(aTarget));
			if(!pTarget && !IsBot)
				pTarget = GameServer()->EnsureWorld(str_quickhash(aTarget));
			if(!pTarget || pTarget == this)
				continue;
			if(IsBot)
			{
				static_cast<IBotEntity *>(pEnt)->MoveToWorld(pTarget, vec2(ArrivalX, pEnt->GetPos().y));
				continue;
			}
			RemoveEntity(pEnt);
			pEnt->SetGameWorld(pTarget);
			pEnt->OnRingTransfer(ArrivalX);
			pTarget->InsertEntity(pEnt);
		}
	}

	RemoveEntities();
}

CEntity *CGameWorld::IntersectEntity(vec2 Pos0, vec2 Pos1, float Radius, vec2 &NewPos, int Type, CEntity *pNotThis)
{
	// Find other entities
	float ClosestLen = distance(Pos0, Pos1) * 100.0f;
	CEntity *pClosest = 0;

	for(auto &pEnt : m_alpEntityLists[Type])
	{
		if(pEnt == pNotThis)
			continue;

		vec2 IntersectPos = closest_point_on_line(Pos0, Pos1, pEnt->m_Pos);
		float Len = distance(pEnt->m_Pos, IntersectPos);
		if(Len < pEnt->GetProximityRadius() + Radius)
		{
			Len = distance(Pos0, IntersectPos);
			if(Len < ClosestLen)
			{
				NewPos = IntersectPos;
				ClosestLen = Len;
				pClosest = pEnt;
			}
		}
	}

	SRingNeighbour aNeighbours[2];
	const int Neighbours = CollectRingNeighbours(aNeighbours, 2);
	for(int n = 0; n < Neighbours; n++)
	{
		const vec2 Offset = aNeighbours[n].m_Offset;
		for(auto &pEnt : aNeighbours[n].m_pWorld->m_alpEntityLists[Type])
		{
			if(pEnt == pNotThis)
				continue;
			const vec2 EntityPos = pEnt->m_Pos + Offset;
			vec2 IntersectPos = closest_point_on_line(Pos0, Pos1, EntityPos);
			float Len = distance(EntityPos, IntersectPos);
			if(Len < pEnt->GetProximityRadius() + Radius)
			{
				Len = distance(Pos0, IntersectPos);
				if(Len < ClosestLen)
				{
					NewPos = IntersectPos;
					ClosestLen = Len;
					pClosest = pEnt;
				}
			}
		}
	}

	return pClosest;
}

CEntity *CGameWorld::IntersectFlagEntity(vec2 Pos0, vec2 Pos1, float Radius, vec2 &NewPos, int Flag, CEntity *pNotThis)
{
	CFlagCheck Check(Flag);
	// Find other entities
	float ClosestLen = distance(Pos0, Pos1) * 100.0f;
	CEntity *pClosest = 0;

	for(auto &pEnt : m_lpFlagEntityList)
	{
		if(pEnt == pNotThis || !Check(pEnt))
			continue;

		vec2 IntersectPos = closest_point_on_line(Pos0, Pos1, pEnt->m_Pos);
		float Len = distance(pEnt->m_Pos, IntersectPos);
		if(Len < pEnt->GetProximityRadius() + Radius)
		{
			Len = distance(Pos0, IntersectPos);
			if(Len < ClosestLen)
			{
				NewPos = IntersectPos;
				ClosestLen = Len;
				pClosest = pEnt;
			}
		}
	}

	SRingNeighbour aNeighbours[2];
	const int Neighbours = CollectRingNeighbours(aNeighbours, 2);
	for(int n = 0; n < Neighbours; n++)
	{
		const vec2 Offset = aNeighbours[n].m_Offset;
		for(auto &pEnt : aNeighbours[n].m_pWorld->m_lpFlagEntityList)
		{
			if(pEnt == pNotThis || !Check(pEnt))
				continue;
			const vec2 EntityPos = pEnt->m_Pos + Offset;
			vec2 IntersectPos = closest_point_on_line(Pos0, Pos1, EntityPos);
			float Len = distance(EntityPos, IntersectPos);
			if(Len < pEnt->GetProximityRadius() + Radius)
			{
				Len = distance(Pos0, IntersectPos);
				if(Len < ClosestLen)
				{
					NewPos = IntersectPos;
					ClosestLen = Len;
					pClosest = pEnt;
				}
			}
		}
	}

	return pClosest;
}

CEntity *CGameWorld::ClosestEntity(vec2 Pos, float Radius, int Type, CEntity *pNotThis, bool AcrossRing)
{
	// Find other entities
	float ClosestRange = Radius * 2;
	CEntity *pClosest = 0;

	for(auto &pEnt : m_alpEntityLists[Type])
	{
		if(pEnt == pNotThis)
			continue;

		float Len = distance(Pos, pEnt->m_Pos);
		if(Len < pEnt->m_ProximityRadius + Radius)
		{
			if(Len < ClosestRange)
			{
				ClosestRange = Len;
				pClosest = pEnt;
			}
		}
	}

	// an item or a player right across the boundary is physically adjacent
	if(AcrossRing)
	{
		SRingNeighbour aNeighbours[2];
		const int Neighbours = CollectRingNeighbours(aNeighbours, 2);
		for(int n = 0; n < Neighbours; n++)
		{
			const vec2 Offset = aNeighbours[n].m_Offset;
			for(auto &pEnt : aNeighbours[n].m_pWorld->m_alpEntityLists[Type])
			{
				if(pEnt == pNotThis)
					continue;
				float Len = distance(Pos, pEnt->m_Pos + Offset);
				if(Len < pEnt->GetProximityRadius() + Radius)
				{
					if(Len < ClosestRange)
					{
						ClosestRange = Len;
						pClosest = pEnt;
					}
				}
			}
		}
	}

	return pClosest;
}

CResourceEntity *CGameWorld::ClosestResource(vec2 Pos, float Radius)
{
	float ClosestDist = Radius * 100.0f;
	CResourceEntity *pClosest = nullptr;

	for(auto &pEnt : m_alpEntityLists[ENTTYPE_RESOURCE])
	{
		CResourceEntity *pRes = static_cast<CResourceEntity *>(pEnt);
		if(pRes->IsRespawning() || pRes->IsMarkedForDestroy())
			continue;
		const float Dist = distance(pRes->GetPos(), Pos);
		if(Dist < pRes->GetProximityRadius() + Radius && Dist < ClosestDist)
		{
			ClosestDist = Dist;
			pClosest = pRes;
		}
	}

	// Like FindEntities: a node in the shared strip is owned by a live ring
	// neighbour, so compare it in this world's frame.
	SRingNeighbour aNeighbours[2];
	const int Neighbours = CollectRingNeighbours(aNeighbours, 2);
	for(int n = 0; n < Neighbours; n++)
	{
		const vec2 Offset = aNeighbours[n].m_Offset;
		for(auto &pEnt : aNeighbours[n].m_pWorld->m_alpEntityLists[ENTTYPE_RESOURCE])
		{
			CResourceEntity *pRes = static_cast<CResourceEntity *>(pEnt);
			if(pRes->IsRespawning() || pRes->IsMarkedForDestroy())
				continue;
			const float Dist = distance(pRes->GetPos() + Offset, Pos);
			if(Dist < pRes->GetProximityRadius() + Radius && Dist < ClosestDist)
			{
				ClosestDist = Dist;
				pClosest = pRes;
			}
		}
	}

	return pClosest;
}

CEntity *CGameWorld::ClosestFlagEntity(vec2 Pos, float Radius, int Flag, CEntity *pNotThis)
{
	CFlagCheck Check(Flag);
	// Find other entities
	float ClosestRange = Radius * 2;
	CEntity *pClosest = 0;

	for(auto &pEnt : m_lpFlagEntityList)
	{
		if(pEnt == pNotThis || !Check(pEnt))
			continue;

		float Len = distance(Pos, pEnt->m_Pos);
		if(Len < pEnt->m_ProximityRadius + Radius)
		{
			if(Len < ClosestRange)
			{
				ClosestRange = Len;
				pClosest = pEnt;
			}
		}
	}

	SRingNeighbour aNeighbours[2];
	const int Neighbours = CollectRingNeighbours(aNeighbours, 2);
	for(int n = 0; n < Neighbours; n++)
	{
		const vec2 Offset = aNeighbours[n].m_Offset;
		for(auto &pEnt : aNeighbours[n].m_pWorld->m_lpFlagEntityList)
		{
			if(pEnt == pNotThis || !Check(pEnt))
				continue;
			float Len = distance(Pos, pEnt->m_Pos + Offset);
			if(Len < pEnt->GetProximityRadius() + Radius)
			{
				if(Len < ClosestRange)
				{
					ClosestRange = Len;
					pClosest = pEnt;
				}
			}
		}
	}

	return pClosest;
}

bool CGameWorld::CFlagCheck::operator()(CEntity *&pEntity) const { return pEntity->ObjFlag() & m_ConditionFlag; }

void CGameWorld::CreateDamage(vec2 Pos, int Id, vec2 Source, int HealthAmount, int ArmorAmount, bool Self)
{
	float f = angle(Source);
	CNetEvent_Damage *pEvent = (CNetEvent_Damage *) m_Events.Create(NETEVENTTYPE_DAMAGE, sizeof(CNetEvent_Damage));
	if(pEvent)
	{
		pEvent->m_X = (int) Pos.x;
		pEvent->m_Y = (int) Pos.y;
		pEvent->m_ClientID = Id;
		pEvent->m_Angle = (int) (f * 256.0f);
		pEvent->m_HealthAmount = HealthAmount;
		pEvent->m_ArmorAmount = ArmorAmount;
		pEvent->m_Self = Self;
	}
}

void CGameWorld::CreateHammerHit(vec2 Pos)
{
	// create the event
	CNetEvent_HammerHit *pEvent = (CNetEvent_HammerHit *) m_Events.Create(NETEVENTTYPE_HAMMERHIT, sizeof(CNetEvent_HammerHit));
	if(pEvent)
	{
		pEvent->m_X = (int) Pos.x;
		pEvent->m_Y = (int) Pos.y;
	}
}

void CGameWorld::CreateExplosion(vec2 Pos, CEntity *pOwner, int Weapon, int MaxDamage)
{
	// create the event
	CNetEvent_Explosion *pEvent = (CNetEvent_Explosion *) m_Events.Create(NETEVENTTYPE_EXPLOSION, sizeof(CNetEvent_Explosion));
	if(pEvent)
	{
		pEvent->m_X = (int) Pos.x;
		pEvent->m_Y = (int) Pos.y;
	}

	// deal damage
	array<CEntity *> lpEnts;
	lpEnts.hint_size(8);
	float Radius = g_pData->m_Explosion.m_Radius;
	float InnerRadius = 48.0f;
	float MaxForce = g_pData->m_Explosion.m_MaxForce;
	const int Num = FindFlagEntities(Pos, Radius, lpEnts, CGameWorld::ENTFLAG_HITABLE);
	for(int i = 0; i < Num; i++)
	{
		vec2 Diff = PosInFrame(lpEnts[i]) - Pos;
		vec2 Force(0, MaxForce);
		float l = length(Diff);
		if(l)
			Force = normalize(Diff) * MaxForce;
		float Factor = 1 - clamp((l - InnerRadius) / (Radius - InnerRadius), 0.0f, 1.0f);
		if((int) (Factor * MaxDamage))
			static_cast<CHitableEntity *>(lpEnts[i])->TakeHit(Force * Factor, Diff * -1, (int) (Factor * MaxDamage), pOwner, Weapon);
	}
}

void CGameWorld::CreatePlayerSpawn(vec2 Pos)
{
	// create the event
	CNetEvent_Spawn *ev = (CNetEvent_Spawn *) m_Events.Create(NETEVENTTYPE_SPAWN, sizeof(CNetEvent_Spawn));
	if(ev)
	{
		ev->m_X = (int) Pos.x;
		ev->m_Y = (int) Pos.y;
	}
}

void CGameWorld::CreateDeath(vec2 Pos, int ClientID)
{
	// create the event
	CNetEvent_Death *pEvent = (CNetEvent_Death *) m_Events.Create(NETEVENTTYPE_DEATH, sizeof(CNetEvent_Death));
	if(pEvent)
	{
		pEvent->m_X = (int) Pos.x;
		pEvent->m_Y = (int) Pos.y;
		pEvent->m_ClientID = ClientID;
	}
}

void CGameWorld::CreateSound(vec2 Pos, int Sound, int64 Mask)
{
	if(Sound < 0)
		return;

	// create a sound
	CNetEvent_SoundWorld *pEvent = (CNetEvent_SoundWorld *) m_Events.Create(NETEVENTTYPE_SOUNDWORLD, sizeof(CNetEvent_SoundWorld), Mask);
	if(pEvent)
	{
		pEvent->m_X = (int) Pos.x;
		pEvent->m_Y = (int) Pos.y;
		pEvent->m_SoundID = Sound;
	}
}

// ---------------------------------------------------------------
// world state persistence
//
// A world's persistent state lives in one world_saves row (keyed by
// m_aMapName), as a json document. Each kind of world content owns its own
// field of that document: dropped item pickups are stored under "drop" as
// an array. Future world attributes get their own fields here, so both
// functions below are the single place where the world's state is written
// and restored.
//
// Writes happen only when the world is unloaded or the server shuts down;
// while a world is loaded it exists purely in memory.
// ---------------------------------------------------------------

void CGameWorld::SaveToDatabase()
{
	CDatabase *pDB = GameServer()->Database();
	if(!pDB || !m_aMapName[0])
		return;

	// collect the live dropped pickups of this world
	array<CDroppedPickup *> lDrops;
	for(TypeRange r = DoTypeRange(CGameWorld::ENTTYPE_DROPPEDPICKUP); !r.empty(); r.pop_front())
	{
		CDroppedPickup *pPickup = static_cast<CDroppedPickup *>(r.front());
		if(!pPickup->IsMarkedForDestroy())
			lDrops.add(pPickup);
	}

	// collect the placed drills
	array<CDrillEntity *> lDrills;
	for(TypeRange r = DoTypeRange(CGameWorld::ENTTYPE_DRILL); !r.empty(); r.pop_front())
	{
		CDrillEntity *pDrill = static_cast<CDrillEntity *>(r.front());
		if(!pDrill->IsMarkedForDestroy())
			lDrills.add(pDrill);
	}

	// build the whole world document from scratch
	array<char> lJson;
	memory_stream<char> Stream(&lJson);
	CJsonWriter JsonWriter(&Stream);
	JsonWriter.BeginObject();
	JsonWriter.WriteAttribute("drop");
	JsonWriter.BeginArray();
	for(int i = 0; i < lDrops.size(); i++)
	{
		const CDroppedPickup *pPickup = lDrops[i];
		JsonWriter.BeginObject();
		char aUuid[UUID_MAXSTRSIZE];
			pPickup->ItemId().FormatUuid(aUuid, sizeof(aUuid));
			JsonWriter.WriteAttribute("item");
		JsonWriter.WriteStrValue(aUuid);
		JsonWriter.WriteAttribute("count");
		JsonWriter.WriteIntValue(pPickup->Count());
		JsonWriter.WriteAttribute("x");
		JsonWriter.WriteIntValue(round_to_int(pPickup->GetPos().x));
		JsonWriter.WriteAttribute("y");
		JsonWriter.WriteIntValue(round_to_int(pPickup->GetPos().y));
		JsonWriter.EndObject();
	}
	JsonWriter.EndArray();

	// placed drills: the item, its position and the last beam direction
	JsonWriter.WriteAttribute("drill");
	JsonWriter.BeginArray();
	for(int i = 0; i < lDrills.size(); i++)
	{
		const CDrillEntity *pDrill = lDrills[i];
		char aUuid[UUID_MAXSTRSIZE];
		pDrill->ItemId().FormatUuid(aUuid, sizeof(aUuid));
		const vec2 Dir = pDrill->Dir();
		JsonWriter.BeginObject();
		JsonWriter.WriteAttribute("item");
		JsonWriter.WriteStrValue(aUuid);
		JsonWriter.WriteAttribute("x");
		JsonWriter.WriteIntValue(round_to_int(pDrill->GetPos().x));
		JsonWriter.WriteAttribute("y");
		JsonWriter.WriteIntValue(round_to_int(pDrill->GetPos().y));
		JsonWriter.WriteAttribute("dx");
		JsonWriter.WriteIntValue(round_to_int(Dir.x * 1000.0f));
		JsonWriter.WriteAttribute("dy");
		JsonWriter.WriteIntValue(round_to_int(Dir.y * 1000.0f));
		JsonWriter.EndObject();
	}
	JsonWriter.EndArray();
	// future world attributes get their own fields of this object here
	JsonWriter.EndObject();
	lJson.add(0);

	// Hand the write to the job pool. The map name and the whole json document
	// were snapshotted above, so the job never touches the live world and the
	// world may be unloaded right after this returns. An empty world still
	// queues a job: it deletes the row.
	pDB->QueueWorldSave(m_aMapName, lJson.base_ptr(), lDrops.size() > 0 || lDrills.size() > 0);
}

void CGameWorld::RestoreFromDatabase()
{
	CDatabase *pDB = GameServer()->Database();
	if(!pDB || !m_aMapName[0])
		return;

	// read the whole world state and recreate the dropped items from the
	// "drop" array (read only, no writes). The size is queried first so a large
	// document is never silently truncated.
	int DataSize = 0;
	if(!pDB->GetWorldSaveSize(m_aMapName, &DataSize) || DataSize <= 0)
		return;
	array<char> aData;
	aData.set_size(DataSize + 1);
	if(!pDB->GetWorldSaveData(m_aMapName, aData.base_ptr(), aData.size()))
		return;

	CJsonParser Parser;
	json_value *pJson = Parser.ParseString(aData.base_ptr(), "world save");
	if(!pJson || pJson->type != json_object)
		return;

	CItemSystem *pItems = GameServer()->Item();
	const json_value &rDrops = (*pJson)["drop"];
	for(unsigned i = 0; rDrops.type == json_array && i < rDrops.u.array.length; i++)
	{
		const json_value &rDrop = rDrops[i];
		if(rDrop.type != json_object)
			continue;
		const json_value &rItem = rDrop["item"];
		const json_value &rCount = rDrop["count"];
		const json_value &rX = rDrop["x"];
		const json_value &rY = rDrop["y"];
		if(rItem.type != json_string || rCount.type != json_integer || rX.type != json_integer || rY.type != json_integer)
			continue;
		// the item definitions may have changed while the world was unloaded.
		// The stored value is the canonical uuid; old saves stored the res_id.
		CItemID DropId;
		if(!CItemID::Parse(rItem.u.string.ptr, &DropId) || DropId.IsZero())
			continue;
		if(!pItems->IsKnownItem(DropId))
			continue;

		const int Count = maximum(1, (int) (json_int_t) rCount);
		const float X = (float) (json_int_t) rX;
		const float Y = (float) (json_int_t) rY;

		// nudge the spawn position upward when the stored position is stuck
		// inside a solid tile (e.g. the map was regenerated); skip the entry
		// when no free spot can be found
		vec2 SpawnPos(X, Y);
		bool Stuck = true;
		for(int Step = 0; Step < 64; Step++)
		{
			if(!Collision()->TestBox(SpawnPos, vec2(CDroppedPickup::DROP_BOX, CDroppedPickup::DROP_BOX)))
			{
				Stuck = false;
				break;
			}
			SpawnPos.y -= 32.0f;
		}
		if(Stuck)
			continue;

		new CDroppedPickup(this, SpawnPos, vec2(0.0f, 0.0f), DropId, Count);
	}

	// placed drills: item id, position and the last beam direction
	const json_value &rDrills = (*pJson)["drill"];
	for(unsigned i = 0; rDrills.type == json_array && i < rDrills.u.array.length; i++)
	{
		const json_value &rDrill = rDrills[i];
		if(rDrill.type != json_object)
			continue;
		const json_value &rItem = rDrill["item"];
		const json_value &rX = rDrill["x"];
		const json_value &rY = rDrill["y"];
		if(rItem.type != json_string || rX.type != json_integer || rY.type != json_integer)
			continue;

		CItemID DrillId;
		if(!CItemID::Parse(rItem.u.string.ptr, &DrillId) || DrillId.IsZero())
			continue;
		const CItemSystem::SDrill Def = pItems->GetDrill(DrillId);
		if(!Def.m_HasDrill)
			continue;

		// the stored direction is a scaled unit vector; fall back to "down"
		vec2 Dir(0.0f, 1.0f);
		const json_value &rDx = rDrill["dx"];
		const json_value &rDy = rDrill["dy"];
		if(rDx.type == json_integer && rDy.type == json_integer)
		{
			const vec2 Stored((float) (json_int_t) rDx / 1000.0f, (float) (json_int_t) rDy / 1000.0f);
			if(length(Stored) > 0.0001f)
				Dir = normalize(Stored);
		}

		CDrillEntity *pDrill = new CDrillEntity(this, vec2((float) (json_int_t) rX, (float) (json_int_t) rY), DrillId, Def.m_MaxLength, Def.m_Power, -1);
		pDrill->SetDirection(Dir);
	}
}
