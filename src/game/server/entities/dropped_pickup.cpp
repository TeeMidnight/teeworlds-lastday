/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/math.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/gameworld.h>

#include "dropped_pickup.h"

// movement tuning of a dropped pickup (per tick; teeworlds gravity is applied
// in units/tick^2)
static const float s_DropElasticity = 0.1f; // light bounce off walls/floors
static const float s_DropGroundFriction = 0.9f; // sliding friction (low)
static const float s_DropAirDrag = 0.97f; // air drag (very low)
static const float s_DropMaxSpeed = 40.0f; // terminal velocity guard

CDroppedPickup::CDroppedPickup(CGameWorld *pGameWorld, vec2 Pos, vec2 Direction, const CItemID &ItemId, int Count) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_DROPPEDPICKUP, 0, Pos, DROP_PHYS_SIZE)
{
	m_Kind = KIND_ITEM;
	m_ItemId = ItemId;
	m_Count = Count;
	m_Health = 0;
	m_Vel = Direction;
	// a short grace period so a just thrown item does not get instantly
	// collected by the player standing on the spawn point
	m_NoPickupTick = Server()->Tick() + Server()->TickSpeed() / 2;

	GameWorld()->InsertEntity(this);
}

CDroppedPickup::CDroppedPickup(CGameWorld *pGameWorld, vec2 Pos, vec2 Direction, int Health) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_DROPPEDPICKUP, 0, Pos, HEALTH_PHYS_SIZE)
{
	m_Kind = KIND_HEALTH;
	m_ItemId = CItemID();
	m_Count = 0;
	m_Health = maximum(1, Health);
	m_Vel = Direction;
	// let the pickup fall out of the jar before the owner standing inside it
	// can collect it
	m_NoPickupTick = Server()->Tick() + Server()->TickSpeed() / 2;

	GameWorld()->InsertEntity(this);
}

bool CDroppedPickup::IsGrounded()
{
	CCollision *pCollision = GameWorld()->Collision();
	return pCollision->CheckPoint(m_Pos.x - 8.0f, m_Pos.y + 10.0f) ||
	       pCollision->CheckPoint(m_Pos.x + 8.0f, m_Pos.y + 10.0f);
}

void CDroppedPickup::Tick()
{
	const int Now = Server()->Tick();

	// try to collect the payload when a player touches it
	if(Now >= m_NoPickupTick)
	{
		const float PickupRadius = m_Kind == KIND_HEALTH ? 20.0f : 22.0f;
		CCharacter *pChr = (CCharacter *) GameWorld()->ClosestEntity(m_Pos, PickupRadius, CGameWorld::ENTTYPE_CHARACTER, 0, true);
		if(pChr && pChr->IsAlive() && GameServer()->m_pController->CanCharacterPickup(pChr))
		{
			bool Picked = false;
			if(m_Kind == KIND_HEALTH)
			{
				// only a tee that is missing health can take it, so a full
				// health tee does not waste it
				Picked = pChr->IncreaseHealth(m_Health);
			}
			else
			{
				// only collected when the player has inventory space;
				// otherwise it stays on the ground for someone else
				Picked = GameServer()->Item()->AddItem(pChr->GetCID(), m_ItemId, m_Count);
			}

			if(Picked)
			{
				// no write here: item drops are persisted only when the world
				// is unloaded or the server shuts down
				GameWorld()->CreateSound(m_Pos, SOUND_PICKUP_HEALTH);
				GameWorld()->DestroyEntity(this);
				return;
			}
		}
	}

	// ----- physics -----
	const bool Grounded = IsGrounded();
	const float Gravity = GameWorld()->m_Core.m_Tuning.m_Gravity;

	// gravity
	m_Vel.y += Gravity;

	// horizontal friction: on the ground the pickup rolls with little
	// friction, in the air it is nearly (but not fully) frictionless, so a
	// thrown item travels far without sliding forever
	if(Grounded)
		m_Vel.x *= s_DropGroundFriction;
	else
		m_Vel.x *= s_DropAirDrag;

	// guard against unbounded velocities (long falls / extreme throws)
	m_Vel.x = clamp(m_Vel.x, -s_DropMaxSpeed, s_DropMaxSpeed);

	// move the pickup, bouncing lightly off solid tiles
	vec2 NewPos = m_Pos;
	GameWorld()->Collision()->MoveBox(&NewPos, &m_Vel, vec2(DROP_BOX, DROP_BOX), s_DropElasticity);
	m_Pos = NewPos;

	if(GameLayerClipped(m_Pos))
		GameWorld()->DestroyEntity(this);
}

void CDroppedPickup::TickPaused()
{
	++m_NoPickupTick;
}

void CDroppedPickup::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	CNetObj_Pickup *pP = static_cast<CNetObj_Pickup *>(Server()->SnapNewItem(NETOBJTYPE_PICKUP, GetID(), sizeof(CNetObj_Pickup)));
	if(!pP)
		return;

	pP->m_X = round_to_int(m_Pos.x);
	pP->m_Y = round_to_int(m_Pos.y);
	pP->m_Type = m_Kind == KIND_HEALTH ? PICKUP_HEALTH : PICKUP_HAMMER;
}
