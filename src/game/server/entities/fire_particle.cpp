/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>
#include <generated/protocol.h>

#include "character.h"
#include "fire_particle.h"

// A puff is a small point, not a projectile-sized box: the old flame scripts
// used CProjectile's 6 unit hit radius, and a target must be within its own
// proximity radius plus this one.
static const float FIRE_PARTICLE_HIT_RADIUS = 6.0f;

// A landed ember keeps emitting its client trail for a moment, then vanishes.
// The trail particles themselves live 0.25-0.5 seconds and shrink to nothing,
// so stopping the emitter reads as a gradual fade; a short linger is enough
// and keeps the live emitter count (and the client particle load) down.
static const int FIRE_PARTICLE_LAND_FADE_TICKS = 6;

CFireParticle::CFireParticle(CGameWorld *pGameWorld, vec2 Pos, vec2 Velocity, int Owner, int Span, int Damage, int Weapon, int IgnoreEntityID, float Gravity) :
	CChildEntity(pGameWorld, CGameWorld::ENTTYPE_FIREPARTICLE, 0, Pos)
{
	m_Owner = Owner;
	// Same team resolution as CProjectile: a negative owner is a damage
	// sentinel, and a real client records the team it fired for.
	if(Owner >= 0 && Owner < MAX_CLIENTS && GameServer()->m_apPlayers[Owner])
		m_OwnerTeam = GameServer()->m_apPlayers[Owner]->GetTeam();
	else
		m_OwnerTeam = Owner == PLAYER_TEAM_BLUE ? TEAM_BLUE : TEAM_RED;

	m_Vel = Velocity;
	m_Gravity = Gravity;
	m_Landed = false;
	m_StartTick = Server()->Tick();
	m_LifeSpan.start(m_StartTick, Span + 1);
	m_Damage = maximum(1, Damage);
	m_Weapon = Weapon;
	m_IgnoreEntityID = IgnoreEntityID;
	GameWorld()->InsertEntity(this);
}

void CFireParticle::LoseOwner()
{
	if(m_OwnerTeam == TEAM_BLUE)
		m_Owner = PLAYER_TEAM_BLUE;
	else
		m_Owner = PLAYER_TEAM_RED;
}

CHitableEntity *CFireParticle::FindPointTarget(CEntity *pNotThis, CEntity *pIgnore)
{
	// FindFlagEntities also walks the ring neighbours and applies the
	// entity's own proximity radius; PosInFrame brings a neighbour hit back
	// into this world's frame before comparing it with the puff.
	array<CEntity *> lpEnts;
	lpEnts.hint_size(8);
	const int Num = GameWorld()->FindFlagEntities(m_Pos, FIRE_PARTICLE_HIT_RADIUS, lpEnts, CGameWorld::ENTFLAG_HITABLE);

	CHitableEntity *pClosest = nullptr;
	float ClosestDist = 1.0e30f;
	for(int i = 0; i < Num; i++)
	{
		CEntity *pEnt = lpEnts[i];
		if(pEnt == this || pEnt == pNotThis || pEnt == pIgnore)
			continue;

		const vec2 TargetPos = GameWorld()->PosInFrame(pEnt);
		const float Dist = distance(m_Pos, TargetPos);
		if(Dist < pEnt->GetProximityRadius() + FIRE_PARTICLE_HIT_RADIUS && Dist < ClosestDist)
		{
			ClosestDist = Dist;
			pClosest = static_cast<CHitableEntity *>(pEnt);
		}
	}
	return pClosest;
}

void CFireParticle::Reset()
{
	GameWorld()->DestroyEntity(this);
}

void CFireParticle::Tick()
{
	CCharacter *pOwnerChar = GameServer()->GetPlayerChar(m_Owner);
	CEntity *pIgnore = m_IgnoreEntityID >= 0 ? GameWorld()->FindEntityByID(m_IgnoreEntityID) : nullptr;

	if(m_Landed)
	{
		if(m_FadeSpan.expired(Server()->Tick()))
		{
			GameWorld()->DestroyEntity(this);
			return;
		}
		// A landed ember can still catch a target that steps into it.
		if(CHitableEntity *pTarget = FindPointTarget(pOwnerChar, pIgnore))
		{
			pTarget->TakeHit(vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), m_Damage, this, m_Weapon);
			GameWorld()->DestroyEntity(this);
		}
		return;
	}

	// Gravity is applied first, so the swept segment already contains this
	// tick's fall and a fast puff cannot step over a target.
	m_Vel.y += m_Gravity;

	const vec2 PrevPos = m_Pos;
	const vec2 CurPos = m_Pos + m_Vel;

	CHitableEntity *pTarget = nullptr;
	if(length(m_Vel) > 0.0001f)
	{
		vec2 HitPos = CurPos;
		pTarget = static_cast<CHitableEntity *>(GameWorld()->IntersectFlagEntity(
			PrevPos, CurPos, FIRE_PARTICLE_HIT_RADIUS, HitPos,
			CGameWorld::ENTFLAG_HITABLE, pOwnerChar, pIgnore));
	}
	else
	{
		// A stationary flame (the campfire) has no segment to sweep.
		pTarget = FindPointTarget(pOwnerChar, pIgnore);
	}

	if(pTarget)
	{
		pTarget->TakeHit(vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), m_Damage, this, m_Weapon);
		GameWorld()->DestroyEntity(this);
		return;
	}

	vec2 CollisionPos = CurPos;
	vec2 BeforeCollision = CurPos;
	const int Collide = GameWorld()->Collision()->IntersectLine(PrevPos, CurPos, &CollisionPos, &BeforeCollision);
	if(Collide)
	{
		// Stop at the last clear sample so the ember sits on the surface and
		// keeps its client trail there while it fades out.
		m_Pos = BeforeCollision;
		m_Vel = vec2(0.0f, 0.0f);
		m_Landed = true;
		m_LifeSpan.stop();
		m_FadeSpan.start(Server()->Tick(), FIRE_PARTICLE_LAND_FADE_TICKS + random_int() % 5);
		return;
	}

	m_Pos = CurPos;

	if(m_LifeSpan.expired(Server()->Tick()))
		GameWorld()->DestroyEntity(this);
}

void CFireParticle::TickPaused()
{
	// Keep the visual age and the relevant deadline on the paused clock.
	++m_StartTick;
	if(m_Landed)
		m_FadeSpan.start_at(m_FadeSpan.deadline() + 1);
	else
		m_LifeSpan.start_at(m_LifeSpan.deadline() + 1);
}

void CFireParticle::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient, m_Pos))
		return;

	// The stock 0.7 client has no sprite for a hammer projectile but does
	// emit its trail particle for every live projectile. The server updates
	// m_Pos every tick, so the same stationary-type net object leaves a moving
	// ember trail.
	CNetObj_Projectile *pProj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, GetID(), sizeof(CNetObj_Projectile)));
	if(!pProj)
		return;

	pProj->m_X = round_to_int(m_Pos.x);
	pProj->m_Y = round_to_int(m_Pos.y);
	pProj->m_VelX = 0;
	pProj->m_VelY = 0;
	pProj->m_Type = WEAPON_HAMMER;
	pProj->m_StartTick = m_StartTick;
}
