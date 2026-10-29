/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/gamecontext.h>
#include <game/server/player.h>
#include <generated/protocol.h>

#include "resource.h"

const int ResourcePhysSize = 14;

// maps the "display" json field to a client side pickup sprite
static int DisplayToPickupType(const char *pDisplay)
{
	if(str_comp_nocase(pDisplay, "health") == 0)
		return PICKUP_HEALTH;
	if(str_comp_nocase(pDisplay, "grenade") == 0)
		return PICKUP_GRENADE;
	if(str_comp_nocase(pDisplay, "shotgun") == 0)
		return PICKUP_SHOTGUN;
	if(str_comp_nocase(pDisplay, "laser") == 0)
		return PICKUP_LASER;
	if(str_comp_nocase(pDisplay, "ninja") == 0)
		return PICKUP_NINJA;
	if(str_comp_nocase(pDisplay, "gun") == 0)
		return PICKUP_GUN;
	if(str_comp_nocase(pDisplay, "hammer") == 0)
		return PICKUP_HAMMER;
	return PICKUP_ARMOR;
}

CResourceEntity::CResourceEntity(CGameWorld *pGameWorld, vec2 Pos, const char *pResId, const char *pDisplay, int Hardness, int RespawnTime) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_RESOURCE, 0, Pos, ResourcePhysSize)
{
	m_Id = CItemID::FromName(pResId);
	str_copy(m_aDisplay, pDisplay, sizeof(m_aDisplay));
	m_Hardness = Hardness;
	m_MaxHardness = Hardness;
	m_RespawnTime = RespawnTime;
	m_Respawning = false;

	GameWorld()->InsertEntity(this);
}

void CResourceEntity::Snap(int SnappingClient)
{
	if(IsRespawning())
		return;
	if(NetworkClipped(SnappingClient))
		return;

	CNetObj_Pickup *pP = static_cast<CNetObj_Pickup *>(Server()->SnapNewItem(NETOBJTYPE_PICKUP, GetID(), sizeof(CNetObj_Pickup)));
	if(!pP)
		return;

	pP->m_X = round_to_int(m_Pos.x);
	pP->m_Y = round_to_int(m_Pos.y);
	pP->m_Type = DisplayToPickupType(m_aDisplay);
}

void CResourceEntity::Tick()
{
	// the timer holds the deadline, not a decremented counter; a world that
	// was frozen catches up here the first tick it runs again
	if(m_Respawning && m_RespawnTimer.expired(Server()->Tick()))
	{
		m_Respawning = false;
		m_Hardness = m_MaxHardness;
	}
}

bool CResourceEntity::TakeHit(int Hardness)
{
	// a respawning resource can't be harvested
	if(IsRespawning())
		return false;
	m_Hardness -= Hardness;
	if(m_Hardness <= 0)
	{
		if(m_RespawnTime > 0)
		{
			m_Respawning = true;
			m_RespawnTimer.start(Server()->Tick(), m_RespawnTime * Server()->TickSpeed());
			m_Hardness = 0;
		}
		else
		{
			GameWorld()->DestroyEntity(this);
		}
		return true;
	}
	return false;
}
