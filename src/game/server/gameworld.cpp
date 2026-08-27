/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include <generated/server_data.h>

#include <engine/shared/jsonparser.h>

#include <climits>

#include "entities/character.h"
#include "entity.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "gameworld.h"

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

	for(int i = 0; i < NUM_ENTTYPES; i++)
	{
		m_alpEntityLists[i].hint_size(16);
	}
	m_lpFlagEntityList.hint_size(8);
}

CGameWorld::~CGameWorld()
{
	// delete all entities
	for(int i = 0; i < NUM_ENTTYPES; i++)
		while(m_alpEntityLists[i].size())
			delete m_alpEntityLists[i][0];
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

	// generated worlds carry their entrance definitions as json in the
	// MAPITEMTYPE_JSON item
	CMapItemJson *pJsonItem = (CMapItemJson *) pMap->FindItem(MAPITEMTYPE_JSON, 0);
	if(pJsonItem && pJsonItem->m_Version == CMapItemJson::CURRENT_VERSION && pJsonItem->m_Data > -1)
	{
		const char *pJsonData = (const char *) pMap->GetData(pJsonItem->m_Data);
		if(pJsonData)
			ParseEntrances(pJsonData);
	}
}

void CGameWorld::ParseEntrances(const char *pJsonData)
{
	CJsonParser Parser;
	json_value *pJson = Parser.ParseString(pJsonData, "entrances");
	if(!pJson || pJson->type != json_array)
		return;
	for(unsigned i = 0; i < pJson->u.array.length; i++)
	{
		const json_value &rEntrance = (*pJson)[i];
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
	// update all objects
	for(int i = 0; i < NUM_ENTTYPES; i++)
		for(auto &pEnt : m_alpEntityLists[i])
			pEnt->Tick();

	for(int i = 0; i < NUM_ENTTYPES; i++)
		for(auto &pEnt : m_alpEntityLists[i])
			pEnt->TickDefered();

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

	return pClosest;
}

CEntity *CGameWorld::ClosestEntity(vec2 Pos, float Radius, int Type, CEntity *pNotThis)
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
		vec2 Diff = lpEnts[i]->GetPos() - Pos;
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
