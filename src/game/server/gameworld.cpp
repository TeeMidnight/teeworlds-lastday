/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */

#include <base/math.h>
#include <base/tl/stream.h>
#include <engine/server/mapgen.h>
#include <engine/server/worldgen.h>
#include <engine/shared/jsonparser.h>
#include <engine/shared/jsonwriter.h>
#include <game/server/database/database.h>
#include <generated/server_data.h>

#include "botmanager.h"
#include "container.h"
#include "entities/botentity.h"
#include "entities/character.h"
#include "entities/container.h"
#include "entities/drill.h"
#include "entities/dropped_pickup.h"
#include "entities/monsterentity.h"
#include "entities/resource.h"
#include "entity.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "gameworld.h"
#include "item.h"
#include "mobdata.h"
#include "scripting/scripted_entity.h"

#include <algorithm>
#include <climits>

// CJsonWriter only writes typed values, so a state/params document that is
// already parsed is walked here and written back as a nested object instead of
// being escaped into a string.
static void WriteJsonValue(CJsonWriter &Writer, const json_value &Value)
{
	switch(Value.type)
	{
		case json_object:
			Writer.BeginObject();
			for(unsigned i = 0; i < Value.u.object.length; i++)
			{
				Writer.WriteAttribute(Value.u.object.values[i].name);
				WriteJsonValue(Writer, *Value.u.object.values[i].value);
			}
			Writer.EndObject();
			break;
		case json_array:
			Writer.BeginArray();
			for(unsigned i = 0; i < Value.u.array.length; i++)
				WriteJsonValue(Writer, *Value.u.array.values[i]);
			Writer.EndArray();
			break;
		case json_integer:
			Writer.WriteInt64Value(Value.u.integer);
			break;
		case json_double:
			Writer.WriteDoubleValue(Value.u.dbl);
			break;
		case json_string:
			Writer.WriteStrValue(Value.u.string.ptr);
			break;
		case json_boolean:
			Writer.WriteBoolValue(Value.u.boolean != 0);
			break;
		case json_none:
		case json_null:
		default:
			Writer.WriteNullValue();
			break;
	}
}

// The stored form of a scripted entity's state: a nested json object when the
// text parses, its raw string otherwise (so a damaged document is not lost).
static void WriteJsonText(CJsonWriter &Writer, const char *pAttribute, const std::string &Text)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseString(Text.c_str(), pAttribute);
	Writer.WriteAttribute(pAttribute);
	if(pJson && (pJson->type == json_object || pJson->type == json_array))
		WriteJsonValue(Writer, *pJson);
	else
		Writer.WriteStrValue(Text.c_str());
}

// Reads a value written by WriteJsonText back into the text the script layer
// expects, accepting both the nested object and the legacy string form.
static std::string ReadJsonText(const json_value &Value)
{
	if(Value.type == json_object || Value.type == json_array)
		return ScriptJsonToString(&Value);
	if(Value.type == json_string)
		return Value.u.string.ptr;
	return "{}";
}

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
		*pArrivalX = RingArrivalXClamped(m_Stride, m_Collision.GetWidth(), m_SharedMargin, Direction, LocalX);
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

void CGameWorld::DropRingProxiesOf(CGameWorld *pOwner)
{
	if(!pOwner)
		return;

	// player proxies: the core entry is only released while it still points
	// at our own copy, so a real character of this world is never unbound
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		if(m_apRingProxyOwner[c] != pOwner)
			continue;
		if(m_Core.GetCharacter(c) == &m_aRingProxyCores[c])
			m_Core.SetCharacter(c, nullptr);
		m_apRingProxyOwner[c] = nullptr;
	}

	// bot proxies: the owner was about to be destroyed, so any borrowed core
	// still naming it is dropped with the proxy
	for(int i = m_lBotRingProxies.size() - 1; i >= 0; i--)
	{
		if(m_lBotRingProxies[i].m_pOwner != pOwner)
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
	CBotManager *pBots = GameServer()->BotManager();
	if(pBots)
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
		{
			CEntity *pEnt = m_alpEntityLists[i][0];
			// ClearBots() only claims the bots whose GameWorld() still names
			// this world. A bot left behind here is nonetheless destroyed by
			// this loop, so its manager entry has to go first: otherwise the
			// table keeps a dangling pointer that a later ClearBots() would
			// follow through TeeID().
			if(pBots && i == ENTTYPE_BOTENTITY)
			{
				const Uuid BotID = static_cast<IBotEntity *>(pEnt)->GetBotID();
				if(pBots->BotByID(BotID) == pEnt)
					pBots->ForgetBot(BotID);
			}
			delete pEnt;
		}
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

	// Generated maps keep their metadata in the map pack (or in a legacy
	// sidecar file next to the map). The engine hands the pack's JSON over in
	// memory after LoadMap, so a pack-restored world does not need a duplicate
	// sidecar just to parse its entrances and ring geometry. Older maps still
	// embed the same data as map json items; the reads below are the fallback.
	bool HasMeta = false;
	array<char> aMeta;
	const char *pServerMeta = nullptr;
	int ServerMetaSize = 0;
	if(GameServer()->Server() &&
		GameServer()->Server()->GetMapMeta(m_WorldID, &pServerMeta, &ServerMetaSize) &&
		pServerMeta && ServerMetaSize > 0)
	{
		aMeta.set_size(ServerMetaSize + 1);
		mem_copy(aMeta.base_ptr(), pServerMeta, ServerMetaSize);
		aMeta[ServerMetaSize] = '\0';
	}
	else
		LoadGeneratedWorldMeta(GameServer()->Storage(), m_aMapName, aMeta);

	if(aMeta.size() > 0)
	{
		CJsonParser MetaParser;
		json_value *pMeta = MetaParser.ParseString(aMeta.base_ptr(), "world meta");
		if(pMeta && pMeta->type == json_object)
		{
			HasMeta = true;
			ParseEntrances((*pMeta)["entrances"]);
			ApplyWorldJson((*pMeta)["world"]);
			SpawnResources((*pMeta)["resources"]);
			SpawnContainers((*pMeta)["containers"]);
			SpawnMobs((*pMeta)["mobs"]);
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

void CGameWorld::SpawnContainers(const json_value &rContainers)
{
	if(rContainers.type != json_array)
		return;
	CContainerSystem *pContainers = GameServer()->Containers();
	if(!pContainers)
		return;

	for(unsigned i = 0; i < rContainers.u.array.length; i++)
	{
		const json_value &rEntry = rContainers[i];
		if(rEntry.type != json_object)
			continue;

		const json_value &rKind = rEntry["kind"];
		if(rKind.type != json_string)
			continue;

		const SContainerDef *pDef = pContainers->Find(rKind.u.string.ptr);
		if(!pDef)
		{
			dbg_msg("container", "map '%s' spawns unknown container kind '%s'", m_aMapName, rKind.u.string.ptr);
			continue;
		}

		const int X = (int) (json_int_t) rEntry["pos_x"];
		const int Y = (int) (json_int_t) rEntry["pos_y"];
		const int StorageID = (int) (json_int_t) rEntry["id"];
		const json_value &rLoot = rEntry["loot"];
		const char *pLoot = rLoot.type == json_string ? rLoot.u.string.ptr : "";

		new CContainerEntity(this, vec2(X * 32.0f + 16.0f, Y * 32.0f + 16.0f),
			pDef->m_aKind, pDef->m_aName, pLoot, pDef->m_Capacity, pDef->m_Pickup, StorageID);
	}
}

void CGameWorld::SpawnContainers(const char *pJsonData)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseString(pJsonData, "containers");
	if(pJson)
		SpawnContainers(*pJson);
}

void CGameWorld::SpawnMobs(const json_value &rMobs)
{
	if(rMobs.type != json_array)
		return;
	CMobSystem *pMobs = GameServer()->Mobs();
	CBotManager *pBots = GameServer()->BotManager();
	if(!pMobs || !pBots)
		return;

	for(unsigned i = 0; i < rMobs.u.array.length; i++)
	{
		const json_value &rEntry = rMobs[i];
		if(rEntry.type != json_object)
			continue;
		const json_value &rMob = rEntry["mob"];
		if(rMob.type != json_string)
			continue;
		const SMobDef *pDef = pMobs->Find(rMob.u.string.ptr);
		if(!pDef)
		{
			dbg_msg("mob", "map '%s' spawns unknown mob '%s'", m_aMapName, rMob.u.string.ptr);
			continue;
		}
		const int Group = (int) (json_int_t) rEntry["group"];
		if(Group != 0 && IsBossDefeated(Group))
			continue;
		const int X = (int) (json_int_t) rEntry["pos_x"];
		const int Y = (int) (json_int_t) rEntry["pos_y"];
		IBotEntity *pBot = pBots->SpawnMob(this, pDef, vec2(X * 32.0f + 16.0f, Y * 32.0f + 16.0f), nullptr, nullptr, false);
		if(!pBot || str_comp(pBot->BotTypeName(), "monster") != 0)
			continue;
		static_cast<CMonsterEntity *>(pBot)->SetSpawnGroup(Group);
	}
}

void CGameWorld::SpawnMobs(const char *pJsonData)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseString(pJsonData, "mobs");
	if(pJson)
		SpawnMobs(*pJson);
}

bool CGameWorld::IsBossDefeated(int Group) const
{
	if(Group == 0)
		return false;
	for(int i = 0; i < m_lDefeatedBossGroups.size(); i++)
		if(m_lDefeatedBossGroups[i] == Group)
			return true;
	return false;
}

void CGameWorld::MarkBossDefeated(int Group)
{
	if(Group == 0 || IsBossDefeated(Group))
		return;
	m_lDefeatedBossGroups.add(Group);
}

void CGameWorld::DestroyDefeatedBossGroups()
{
	if(!m_lDefeatedBossGroups.size())
		return;
	for(TypeRange r = DoTypeRange(CGameWorld::ENTTYPE_BOTENTITY); !r.empty(); r.pop_front())
	{
		IBotEntity *pBot = static_cast<IBotEntity *>(r.front());
		if(!pBot || str_comp(pBot->BotTypeName(), "monster") != 0)
			continue;
		CMonsterEntity *pMonster = static_cast<CMonsterEntity *>(pBot);
		if(IsBossDefeated(pMonster->SpawnGroup()))
			pMonster->DespawnSilently();
	}
}

void CGameWorld::InitContainerLoot()
{
	CContainerSystem *pContainers = GameServer()->Containers();
	if(!pContainers)
		return;

	// The deterministic seed keeps the first roll of a map instance stable;
	// the result is what gets persisted, so later loads restore it instead of
	// rolling again.
	const unsigned MapHash = str_quickhash(m_aMapName);
	for(TypeRange r = DoTypeRange(CGameWorld::ENTTYPE_CONTAINER); !r.empty(); r.pop_front())
	{
		CContainerEntity *pContainer = static_cast<CContainerEntity *>(r.front());
		if(pContainer->IsMarkedForDestroy() || pContainer->Initialized())
			continue;
		const unsigned Seed = MapHash ^ ((unsigned) pContainer->StorageID() * 2654435761u) ^ 0x85ebca6bu;
		pContainers->FillLoot(pContainer, Seed);
	}
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

CEntity *CGameWorld::FindEntityByID(int ID)
{
	for(int i = 0; i < NUM_ENTTYPES; i++)
		for(auto &pEnt : m_alpEntityLists[i])
			if(pEnt->GetID() == ID)
				return pEnt;
	return nullptr;
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

bool CGameWorld::HasEntity(const CEntity *pEnt) const
{
	if(!pEnt || pEnt->ObjType() < 0 || pEnt->ObjType() >= NUM_ENTTYPES)
		return false;

	const array<CEntity *> &List = m_alpEntityLists[pEnt->ObjType()];
	for(int i = 0; i < List.size(); i++)
		if(List[i] == pEnt)
			return true;
	return false;
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
			const unsigned TargetID = str_quickhash(aTarget);
			GameServer()->Server()->RegisterMapName(aTarget);
			CGameWorld *pTarget = GameServer()->WorldByID(TargetID);
			if(!pTarget && !IsBot)
			{
				// EnsureWorld never generates: it queues the prefetch and returns
				// null, and the entity waits at the seam for a later tick.
				pTarget = GameServer()->EnsureWorld(TargetID);
			}
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

CEntity *CGameWorld::IntersectFlagEntity(vec2 Pos0, vec2 Pos1, float Radius, vec2 &NewPos, int Flag, CEntity *pNotThis, CEntity *pNotThis2)
{
	CFlagCheck Check(Flag);
	// Find other entities
	float ClosestLen = distance(Pos0, Pos1) * 100.0f;
	CEntity *pClosest = 0;

	for(auto &pEnt : m_lpFlagEntityList)
	{
		if(pEnt == pNotThis || pEnt == pNotThis2 || !Check(pEnt))
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
			if(pEnt == pNotThis || pEnt == pNotThis2 || !Check(pEnt))
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

CContainerEntity *CGameWorld::ClosestContainer(vec2 Pos, float Radius)
{
	float ClosestDist = Radius * 100.0f;
	CContainerEntity *pClosest = nullptr;

	for(auto &pEnt : m_alpEntityLists[ENTTYPE_CONTAINER])
	{
		CContainerEntity *pContainer = static_cast<CContainerEntity *>(pEnt);
		if(pContainer->IsMarkedForDestroy())
			continue;
		const float Dist = distance(pContainer->GetPos(), Pos);
		if(Dist < pContainer->GetProximityRadius() + Radius && Dist < ClosestDist)
		{
			ClosestDist = Dist;
			pClosest = pContainer;
		}
	}
	return pClosest;
}

CContainerEntity *CGameWorld::FindContainerByStorageID(int StorageID)
{
	for(auto &pEnt : m_alpEntityLists[ENTTYPE_CONTAINER])
	{
		CContainerEntity *pContainer = static_cast<CContainerEntity *>(pEnt);
		if(!pContainer->IsMarkedForDestroy() && pContainer->StorageID() == StorageID)
			return pContainer;
	}
	return nullptr;
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

	// collect the live persistent item drops of this world; health pickups use
	// the same entity class but are transient and never saved
	array<CDroppedPickup *> lDrops;
	for(TypeRange r = DoTypeRange(CGameWorld::ENTTYPE_DROPPEDPICKUP); !r.empty(); r.pop_front())
	{
		CDroppedPickup *pPickup = static_cast<CDroppedPickup *>(r.front());
		if(!pPickup->IsMarkedForDestroy() && pPickup->IsPersistent())
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

	// Collect every container whose loot has been initialized. An emptied
	// container must be saved too: otherwise the next load would roll its loot
	// again and refill it.
	array<CContainerEntity *> lContainers;
	for(TypeRange r = DoTypeRange(CGameWorld::ENTTYPE_CONTAINER); !r.empty(); r.pop_front())
	{
		CContainerEntity *pContainer = static_cast<CContainerEntity *>(r.front());
		if(!pContainer->IsMarkedForDestroy() && pContainer->Initialized())
			lContainers.add(pContainer);
	}

	// collect the persistent scripted props (the health jar among them). Only
	// persistent kinds are world content; a transient visual prop comes back
	// from its own spawn point or not at all.
	array<CScriptedEntity *> lScripted;
	for(TypeRange r = DoTypeRange(CGameWorld::ENTTYPE_SCRIPTED); !r.empty(); r.pop_front())
	{
		CScriptedEntity *pEntity = static_cast<CScriptedEntity *>(r.front());
		if(pEntity->IsMarkedForDestroy() || !pEntity->Persistent())
			continue;
		lScripted.add(pEntity);
	}

	// Give every script a chance to fold timers into its state first. This runs
	// after the list walk, so a script that spawns another prop cannot invalidate
	// the range being iterated.
	for(int i = 0; i < lScripted.size(); i++)
		lScripted[i]->CallSave();

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
		// A money drop stores its G amount instead of an item id; both kinds
		// share the position fields and the same restore path.
		if(pPickup->IsCurrencyDrop())
		{
			JsonWriter.WriteAttribute("money");
			JsonWriter.WriteInt64Value(pPickup->Amount());
		}
		else
		{
			char aUuid[UUID_MAXSTRSIZE];
			pPickup->ItemId().FormatUuid(aUuid, sizeof(aUuid));
			JsonWriter.WriteAttribute("item");
			JsonWriter.WriteStrValue(aUuid);
			JsonWriter.WriteAttribute("count");
			JsonWriter.WriteIntValue(pPickup->Count());
		}
		JsonWriter.WriteAttribute("x");
		JsonWriter.WriteIntValue(round_to_int(pPickup->GetPos().x));
		JsonWriter.WriteAttribute("y");
		JsonWriter.WriteIntValue(round_to_int(pPickup->GetPos().y));
		JsonWriter.EndObject();
	}
	JsonWriter.EndArray();

	// container contents: the stable map-instance id and one entry per used
	// slot. The position and the kind are not stored: both are map structure
	// content and are recreated from the generated world metadata.
	JsonWriter.WriteAttribute("container");
	JsonWriter.BeginArray();
	for(int i = 0; i < lContainers.size(); i++)
	{
		const CContainerEntity *pContainer = lContainers[i];
		JsonWriter.BeginObject();
		JsonWriter.WriteAttribute("id");
		JsonWriter.WriteIntValue(pContainer->StorageID());
		JsonWriter.WriteAttribute("kind");
		JsonWriter.WriteStrValue(pContainer->Kind());
		JsonWriter.WriteAttribute("items");
		JsonWriter.BeginArray();
		for(int s = 0; s < pContainer->Capacity(); s++)
		{
			const CContainerEntity::SSlot &Slot = pContainer->Slot(s);
			if(Slot.m_Id.IsZero() || Slot.m_Count <= 0)
				continue;
			char aUuid[UUID_MAXSTRSIZE];
			Slot.m_Id.FormatUuid(aUuid, sizeof(aUuid));
			JsonWriter.BeginObject();
			JsonWriter.WriteAttribute("item");
			JsonWriter.WriteStrValue(aUuid);
			JsonWriter.WriteAttribute("count");
			JsonWriter.WriteIntValue(Slot.m_Count);
			JsonWriter.EndObject();
		}
		JsonWriter.EndArray();
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
		char aOwner[UUID_MAXSTRSIZE];
		format_uuid(pDrill->Access().m_Owner, aOwner, sizeof(aOwner));
		JsonWriter.WriteAttribute("owner");
		JsonWriter.WriteStrValue(aOwner);
		JsonWriter.WriteAttribute("lock");
		JsonWriter.WriteIntValue(pDrill->Access().m_LockMode);
		JsonWriter.EndObject();
	}
	JsonWriter.EndArray();

	// scripted props: kind, position, health, owner account, params and state.
	// The owner is an account uuid so a prop survives a reconnect; a prop placed
	// while logged out keeps its owner client id and comes back ownerless.
	JsonWriter.WriteAttribute("scripted");
	JsonWriter.BeginArray();
	for(int i = 0; i < lScripted.size(); i++)
	{
		const CScriptedEntity *pEntity = lScripted[i];
		char aOwner[UUID_MAXSTRSIZE];
		format_uuid(pEntity->OwnerAccount(), aOwner, sizeof(aOwner));
		JsonWriter.BeginObject();
		JsonWriter.WriteAttribute("kind");
		JsonWriter.WriteStrValue(pEntity->Kind());
		JsonWriter.WriteAttribute("x");
		JsonWriter.WriteIntValue(round_to_int(pEntity->GetPos().x));
		JsonWriter.WriteAttribute("y");
		JsonWriter.WriteIntValue(round_to_int(pEntity->GetPos().y));
		JsonWriter.WriteAttribute("health");
		JsonWriter.WriteIntValue(pEntity->Health());
		JsonWriter.WriteAttribute("lock");
		JsonWriter.WriteIntValue(pEntity->Access().m_LockMode);
		JsonWriter.WriteAttribute("owner");
		JsonWriter.WriteStrValue(aOwner);
		JsonWriter.WriteAttribute("persistent");
		JsonWriter.WriteBoolValue(pEntity->Persistent());
		WriteJsonText(JsonWriter, "params", pEntity->Params());
		WriteJsonText(JsonWriter, "state", pEntity->State());
		JsonWriter.EndObject();
	}
	JsonWriter.EndArray();

	// Boss encounters already cleared in this instance. The whole group is
	// skipped on reload, so a defeated boss and its guards stay gone.
	JsonWriter.WriteAttribute("boss_dead");
	JsonWriter.BeginArray();
	for(int i = 0; i < m_lDefeatedBossGroups.size(); i++)
		JsonWriter.WriteIntValue(m_lDefeatedBossGroups[i]);
	JsonWriter.EndArray();

	// future world attributes get their own fields of this object here
	JsonWriter.EndObject();
	lJson.add(0);

	// Hand the write to the job pool. The map name and the whole json document
	// were snapshotted above, so the job never touches the live world and the
	// world may be unloaded right after this returns. An empty world still
	// queues a job: it deletes the row.
	pDB->QueueWorldSave(m_aMapName, lJson.base_ptr(), lDrops.size() > 0 || lDrills.size() > 0 || lScripted.size() > 0 || lContainers.size() > 0 || m_lDefeatedBossGroups.size() > 0);
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

	const json_value &rBossDead = (*pJson)["boss_dead"];
	for(unsigned i = 0; rBossDead.type == json_array && i < rBossDead.u.array.length; i++)
		MarkBossDefeated((int) (json_int_t) rBossDead[i]);

	CItemSystem *pItems = GameServer()->Item();
	// Nudge the saved position upward when it is stuck inside a solid tile
	// (e.g. the map was regenerated); false when no free spot can be found.
	auto FindDropSpawn = [this](float X, float Y, vec2 *pOut) -> bool
	{
		vec2 SpawnPos(X, Y);
		for(int Step = 0; Step < 64; Step++)
		{
			if(!Collision()->TestBox(SpawnPos, vec2(CDroppedPickup::DROP_BOX, CDroppedPickup::DROP_BOX)))
			{
				*pOut = SpawnPos;
				return true;
			}
			SpawnPos.y -= 32.0f;
		}
		return false;
	};

	const json_value &rDrops = (*pJson)["drop"];
	for(unsigned i = 0; rDrops.type == json_array && i < rDrops.u.array.length; i++)
	{
		const json_value &rDrop = rDrops[i];
		if(rDrop.type != json_object)
			continue;
		const json_value &rX = rDrop["x"];
		const json_value &rY = rDrop["y"];
		if(rX.type != json_integer || rY.type != json_integer)
			continue;
		const float X = (float) (json_int_t) rX;
		const float Y = (float) (json_int_t) rY;
		vec2 SpawnPos;
		if(!FindDropSpawn(X, Y, &SpawnPos))
			continue;

		// money drops store the G amount; anything else is an item stack
		const json_value &rMoney = rDrop["money"];
		if(rMoney.type == json_integer && (int64) (json_int_t) rMoney > 0)
		{
			new CDroppedPickup(this, SpawnPos, vec2(0.0f, 0.0f),
				(int64) (json_int_t) rMoney, CDroppedPickup::KIND_CURRENCY);
			continue;
		}

		const json_value &rItem = rDrop["item"];
		const json_value &rCount = rDrop["count"];
		if(rItem.type != json_string || rCount.type != json_integer)
			continue;
		// the item definitions may have changed while the world was unloaded.
		// The stored value is the canonical uuid; old saves stored the res_id.
		CItemID DropId;
		if(!CItemID::Parse(rItem.u.string.ptr, &DropId) || DropId.IsZero())
			continue;
		if(!pItems->IsKnownItem(DropId))
			continue;

		const int Count = maximum(1, (int) (json_int_t) rCount);
		new CDroppedPickup(this, SpawnPos, vec2(0.0f, 0.0f), DropId, Count);
	}

	// container contents: find the container the map structure spawned with the
	// same stable id and put its stacks back in slot order
	const json_value &rContainers = (*pJson)["container"];
	for(unsigned i = 0; rContainers.type == json_array && i < rContainers.u.array.length; i++)
	{
		const json_value &rEntry = rContainers[i];
		if(rEntry.type != json_object)
			continue;
		const json_value &rID = rEntry["id"];
		if(rID.type != json_integer)
			continue;
		CContainerEntity *pContainer = FindContainerByStorageID((int) (json_int_t) rID);
		if(!pContainer)
		{
			dbg_msg("container", "world '%s': saved container %d no longer exists", m_aMapName, (int) (json_int_t) rID);
			continue;
		}
		// The map content may have changed since the save: a different kind at
		// the same stable id means the stored stacks belong to another prop.
		// Leave this one uninitialized so it rolls fresh loot instead.
		const json_value &rKind = rEntry["kind"];
		if(rKind.type == json_string && str_comp(rKind.u.string.ptr, pContainer->Kind()) != 0)
		{
			dbg_msg("container", "world '%s': container %d changed from '%s' to '%s'",
				m_aMapName, (int) (json_int_t) rID, rKind.u.string.ptr, pContainer->Kind());
			continue;
		}
		// A saved entry always wins, even with an empty item list: the players
		// already emptied this container, so InitContainerLoot must not refill it.
		pContainer->SetInitialized(true);

		const json_value &rItems = rEntry["items"];
		for(unsigned s = 0; rItems.type == json_array && s < rItems.u.array.length; s++)
		{
			const json_value &rItem = rItems[s];
			if(rItem.type != json_object)
				continue;
			const json_value &rItemId = rItem["item"];
			const json_value &rCount = rItem["count"];
			if(rItemId.type != json_string || rCount.type != json_integer)
				continue;
			CItemID Id;
			if(!CItemID::Parse(rItemId.u.string.ptr, &Id) || Id.IsZero())
				continue;
			if(!pItems->IsKnownItem(Id))
				continue;
			pContainer->RestoreSlot(Id, maximum(1, (int) (json_int_t) rCount));
		}
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

		Uuid Owner = UUID_ZEROED;
		const json_value &rOwner = rDrill["owner"];
		if(rOwner.type == json_string)
			parse_uuid(&Owner, rOwner.u.string.ptr);
		const json_value &rLock = rDrill["lock"];
		// an old drill had no lock: keep its historical public behaviour
		const int LockMode = rLock.type == json_integer ? clamp((int) (json_int_t) rLock, (int) BUILD_LOCK_PUBLIC, (int) BUILD_LOCK_TEAM) : BUILD_LOCK_PUBLIC;
		CDrillEntity *pDrill = new CDrillEntity(this, vec2((float) (json_int_t) rX, (float) (json_int_t) rY), DrillId, Def.m_MaxLength, Def.m_Power, -1, Owner, LockMode);
		pDrill->SetDirection(Dir);
	}

	// scripted props: recreate them and let their script rebuild timers/visuals
	// from the stored state (on_spawn, then on_load).
	CScriptedEntitySystem *pScripted = GameServer()->ScriptedEntities();
	const json_value &rScripted = (*pJson)["scripted"];
	for(unsigned i = 0; rScripted.type == json_array && i < rScripted.u.array.length; i++)
	{
		const json_value &rEntity = rScripted[i];
		if(rEntity.type != json_object)
			continue;
		const json_value &rKind = rEntity["kind"];
		const json_value &rX = rEntity["x"];
		const json_value &rY = rEntity["y"];
		if(rKind.type != json_string || rX.type != json_integer || rY.type != json_integer)
			continue;
		if(!pScripted || !pScripted->GetDef(rKind.u.string.ptr))
			continue;

		Uuid Owner = UUID_ZEROED;
		const json_value &rOwner = rEntity["owner"];
		if(rOwner.type == json_string)
			parse_uuid(&Owner, rOwner.u.string.ptr);

		const json_value &rHealth = rEntity["health"];
		// current hp; the definition supplies the maximum
		const int Health = rHealth.type == json_integer ? maximum(1, (int) (json_int_t) rHealth) : -1;
		const json_value &rPersistent = rEntity["persistent"];
		const int Persistent = rPersistent.type == json_boolean ? (rPersistent.u.boolean != 0 ? 1 : 0) : -1;

		const std::string Params = ReadJsonText(rEntity["params"]);
		const std::string State = ReadJsonText(rEntity["state"]);
		const json_value &rLock = rEntity["lock"];
		// old props had no lock and were public; keep that behaviour on load
		const int LockMode = rLock.type == json_integer ? clamp((int) (json_int_t) rLock, (int) BUILD_LOCK_PUBLIC, (int) BUILD_LOCK_TEAM) : BUILD_LOCK_PUBLIC;

		pScripted->Spawn(rKind.u.string.ptr, this, vec2((float) (json_int_t) rX, (float) (json_int_t) rY),
			Params.c_str(), Owner, -1, Persistent, 0, Health, State.c_str(), true, LockMode);
	}

	// Backward compatibility: worlds saved before the scripted entity existed
	// stored health jars in a "jar" array. Convert them into scripted jars so an
	// old world keeps its props after the migration.
	const json_value &rJars = (*pJson)["jar"];
	for(unsigned i = 0; rJars.type == json_array && i < rJars.u.array.length; i++)
	{
		const json_value &rJar = rJars[i];
		if(rJar.type != json_object)
			continue;
		const json_value &rItem = rJar["item"];
		const json_value &rX = rJar["x"];
		const json_value &rY = rJar["y"];
		if(rItem.type != json_string || rX.type != json_integer || rY.type != json_integer)
			continue;

		CItemID JarId;
		if(!CItemID::Parse(rItem.u.string.ptr, &JarId) || JarId.IsZero())
			continue;
		const CItemSystem::SHealthJar Def = pItems->GetHealthJar(JarId);
		if(!Def.m_HasJar)
			continue;
		if(!pScripted || !pScripted->GetDef("health_jar"))
			continue;

		Uuid Owner = UUID_ZEROED;
		const json_value &rOwner = rJar["owner"];
		if(rOwner.type == json_string)
			parse_uuid(&Owner, rOwner.u.string.ptr);
		const json_value &rOwnerName = rJar["owner_name"];
		const char *pOwnerName = rOwnerName.type == json_string ? rOwnerName.u.string.ptr : "";

		pScripted->SpawnHealthJar(this, vec2((float) (json_int_t) rX, (float) (json_int_t) rY),
			JarId, Owner, -1, pOwnerName, Def.m_Cooldown, Def.m_Range, Def.m_Health, "{}", true, BUILD_LOCK_PUBLIC);
	}

	// Mobs are spawned during InitCollision, before the saved defeated list is
	// known. Retire the encounters whose boss was already cleared.
	DestroyDefeatedBossGroups();
}
