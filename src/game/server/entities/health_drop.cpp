/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/entities/health_drop.h>

#include <base/math.h>

#include <generated/protocol.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>

// movement tuning of a dropped health pickup (per tick; gravity is in
// units/tick^2 and comes from the shared tuning, as for a dropped item)
static const float s_HealthDropElasticity = 0.1f;
static const float s_HealthDropGroundFriction = 0.85f;
static const float s_HealthDropAirDrag = 0.97f;
static const float s_HealthDropMaxSpeed = 40.0f;

CHealthDropEntity::CHealthDropEntity(CGameWorld *pGameWorld, vec2 Pos, vec2 Velocity) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_HEALTH_DROP, 0, Pos, PICKUP_RADIUS)
{
	m_Vel = Velocity;
	// let the pickup fall out of the jar before the owner standing inside it
	// can collect it
	m_NoPickupTick = Server()->Tick() + Server()->TickSpeed() / 2;
	GameWorld()->InsertEntity(this);
}

bool CHealthDropEntity::IsGrounded()
{
	CCollision *pCollision = GameWorld()->Collision();
	return pCollision->CheckPoint(m_Pos.x - 8.0f, m_Pos.y + 10.0f) ||
	       pCollision->CheckPoint(m_Pos.x + 8.0f, m_Pos.y + 10.0f);
}

void CHealthDropEntity::Tick()
{
	// collect exactly like a normal health pickup: only a tee that is missing
	// health can take it, so a full health tee does not waste it
	if(Server()->Tick() >= m_NoPickupTick)
	{
		CCharacter *pChr = (CCharacter *) GameWorld()->ClosestEntity(m_Pos, (float) PICKUP_RADIUS, CGameWorld::ENTTYPE_CHARACTER, 0, true);
		if(pChr && pChr->IsAlive() && GameServer()->m_pController->CanCharacterPickup(pChr))
		{
			if(pChr->IncreaseHealth(1))
			{
				GameWorld()->CreateSound(m_Pos, SOUND_PICKUP_HEALTH);
				GameWorld()->DestroyEntity(this);
				return;
			}
		}
	}

	// ----- physics: fall out of the jar and come to rest on the ground -----
	const bool Grounded = IsGrounded();
	const float Gravity = GameWorld()->m_Core.m_Tuning.m_Gravity;

	m_Vel.y += Gravity;
	if(Grounded)
		m_Vel.x *= s_HealthDropGroundFriction;
	else
		m_Vel.x *= s_HealthDropAirDrag;
	m_Vel.x = clamp(m_Vel.x, -s_HealthDropMaxSpeed, s_HealthDropMaxSpeed);

	vec2 NewPos = m_Pos;
	GameWorld()->Collision()->MoveBox(&NewPos, &m_Vel, vec2(DROP_BOX, DROP_BOX), s_HealthDropElasticity);
	m_Pos = NewPos;

	if(GameLayerClipped(m_Pos))
		GameWorld()->DestroyEntity(this);
}

void CHealthDropEntity::TickPaused()
{
	++m_NoPickupTick;
}

void CHealthDropEntity::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	CNetObj_Pickup *pP = static_cast<CNetObj_Pickup *>(Server()->SnapNewItem(NETOBJTYPE_PICKUP, GetID(), sizeof(CNetObj_Pickup)));
	if(!pP)
		return;

	pP->m_X = round_to_int(m_Pos.x);
	pP->m_Y = round_to_int(m_Pos.y);
	pP->m_Type = PICKUP_HEALTH;
}
