#include <base/math.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/gameworld.h>

#include "dropped_pickup.h"

// movement tuning of a dropped item (per tick; teeworlds gravity is applied
// in units/tick^2)
static const float s_DropElasticity = 0.1f; // light bounce off walls/floors
static const float s_DropGroundFriction = 0.9f; // sliding friction (low)
static const float s_DropAirDrag = 0.97f; // air drag (very low)
static const float s_DropMaxSpeed = 40.0f; // terminal velocity guard

CDroppedPickup::CDroppedPickup(CGameWorld *pGameWorld, vec2 Pos, vec2 Direction, const char *pItemId, int Count) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_DROPPEDPICKUP, 0, Pos, DROP_PHYS_SIZE)
{
	str_copy(m_aItemId, pItemId, sizeof(m_aItemId));
	m_Count = Count;
	m_Vel = Direction;
	// a short grace period so a just thrown item does not get instantly
	// collected by the player standing on the spawn point
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

	// try to collect the item when a player touches it
	if(Now >= m_NoPickupTick)
	{
		CCharacter *pChr = (CCharacter *) GameWorld()->ClosestEntity(m_Pos, 22.0f, CGameWorld::ENTTYPE_CHARACTER, 0);
		if(pChr && pChr->IsAlive() && GameServer()->m_pController->CanCharacterPickup(pChr))
		{
			// only collected when the player has inventory space; otherwise
			// it stays on the ground for someone else
			if(GameServer()->Item()->AddItem(pChr->GetCID(), m_aItemId, m_Count))
			{
				// no write here: the world's drops are persisted only when
				// the world is unloaded or the server shuts down
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

	// horizontal friction: on the ground the item rolls with little friction,
	// in the air it is nearly (but not fully) frictionless, so a thrown item
	// travels far without sliding forever
	if(Grounded)
		m_Vel.x *= s_DropGroundFriction;
	else
		m_Vel.x *= s_DropAirDrag;

	// guard against unbounded velocities (long falls / extreme throws)
	m_Vel.x = clamp(m_Vel.x, -s_DropMaxSpeed, s_DropMaxSpeed);

	// move the item, bouncing lightly off solid tiles
	vec2 NewPos = m_Pos;
	GameWorld()->Collision()->MoveBox(&NewPos, &m_Vel, vec2(DROP_BOX, DROP_BOX), s_DropElasticity);
	m_Pos = NewPos;


	if(GameLayerClipped(m_Pos))
		GameWorld()->DestroyEntity(this);
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
	pP->m_Type = PICKUP_HAMMER;
}
