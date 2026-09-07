/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <game/server/gamecontext.h>
#include <generated/server_data.h>

#include "character.h"
#include "laser.h"

CLaser::CLaser(CGameWorld *pGameWorld, vec2 Pos, vec2 Direction, float StartEnergy, int Owner, int Damage) : CChildEntity(pGameWorld, CGameWorld::ENTTYPE_LASER, 0, Pos)
{
	m_Damage = Damage;
	m_Owner = Owner;
	m_Energy = StartEnergy;
	m_Dir = Direction;
	m_Bounces = 0;
	m_EvalTick = 0;
	GameWorld()->InsertEntity(this);
	DoBounce();
}

bool CLaser::HitCharacter(vec2 From, vec2 To)
{
	vec2 At;
	CCharacter *pOwnerChar = GameServer()->GetPlayerChar(m_Owner);
	CHitableEntity *pHit = (CHitableEntity *) GameWorld()->IntersectFlagEntity(m_Pos, To, 0.f, At, CGameWorld::ENTFLAG_HITABLE, pOwnerChar);
	if(!pHit)
		return false;

	m_From = From;
	m_Pos = At;
	m_Energy = -1;
	pHit->TakeHit(vec2(0.f, 0.f), normalize(To - From), m_Damage, this, WEAPON_LASER);
	return true;
}

void CLaser::DoBounce()
{
	m_EvalTick = Server()->Tick();

	if(m_Energy < 0)
	{
		GameWorld()->DestroyEntity(this);
		return;
	}

	const vec2 To = m_Pos + m_Dir * m_Energy;

	// march along the beam and stop at the first interesting tile: a solid
	// wall (bounce), or a water surface (reflect + refract)
	enum
	{
		EVENT_NONE = 0,
		EVENT_SOLID,
		EVENT_WATER,
	};
	int Event = EVENT_NONE;
	bool WasInWater = false;
	// whether the crossed water boundary was a vertical face (i.e. the beam
	// entered/exited through the side of the water body) or a horizontal face
	// (the pool surface). decided by comparing the tiles of the two adjacent
	// samples, never by the beam direction
	bool CrossedXFace = false;
	vec2 EventPos = To;
	CCollision *pCollision = GameWorld()->Collision();
	const int Steps = maximum(1, (int) distance(m_Pos, To));
	int LastTileX = (int) (m_Pos.x / 32.0f);
	int LastTileY = (int) (m_Pos.y / 32.0f);
	vec2 LastSample = m_Pos;
	for(int i = 0; i <= Steps; i++)
	{
		const vec2 Pos = mix(m_Pos, To, i / (float) Steps);
		const int Flags = pCollision->GetCollisionAt(Pos.x, Pos.y);
		if(Flags & CCollision::COLFLAG_SOLID)
		{
			Event = EVENT_SOLID;
			EventPos = Pos;
			break;
		}
		const bool InWater = (Flags & CCollision::COLFLAG_WATER) != 0;
		if(i == 0)
			WasInWater = InWater;
		else if(InWater != WasInWater)
		{
			Event = EVENT_WATER;
			EventPos = Pos;
			const int TileX = (int) (Pos.x / 32.0f);
			const int TileY = (int) (Pos.y / 32.0f);
			if(TileX != LastTileX && TileY != LastTileY)
				// the sample clipped a tile corner: use the dominant sample delta
				CrossedXFace = (Pos.x - LastSample.x) * (Pos.x - LastSample.x) >= (Pos.y - LastSample.y) * (Pos.y - LastSample.y);
			else
				CrossedXFace = TileX != LastTileX;
			break;
		}
		LastTileX = (int) (Pos.x / 32.0f);
		LastTileY = (int) (Pos.y / 32.0f);
		LastSample = Pos;
	}

	// no tile in the way: the beam just runs out
	if(Event == EVENT_NONE)
	{
		if(!HitCharacter(m_Pos, To))
		{
			m_From = m_Pos;
			m_Pos = To;
			m_Energy = -1;
		}
		return;
	}

	// hit a solid wall: bounce off it
	if(Event == EVENT_SOLID)
	{
		if(!HitCharacter(m_Pos, EventPos))
		{
			// intersected
			m_From = m_Pos;
			m_Pos = EventPos;

			vec2 TempPos = m_Pos;
			vec2 TempDir = m_Dir * 4.0f;
			pCollision->MovePoint(&TempPos, &TempDir, 1.0f, 0);
			m_Pos = TempPos;
			m_Dir = normalize(TempDir);

			m_Energy -= distance(m_From, m_Pos) + GameServer()->Tuning()->m_LaserBounceCost;
			m_Bounces++;

			if(m_Bounces > GameServer()->Tuning()->m_LaserBounceNum)
				m_Energy = -1;

			GameWorld()->CreateSound(m_Pos, SOUND_LASER_BOUNCE);
		}
		return;
	}

	// crossed a water surface: the beam reflects *and* refracts
	if(HitCharacter(m_Pos, EventPos))
		return;

	// the crossed face decides how the beam splits. the reflected part mirrors
	// the velocity component perpendicular to that face (diving into a pool
	// through its surface reflects back up; entering through the side reflects
	// sideways), the refracted part bends like light crossing the air/water
	// boundary (ratio 1.33) and continues into the other medium
	const bool EnteringWater = !WasInWater; // travelled from air into the water
	const float Ratio = EnteringWater ? 1.0f / 1.33f : 1.33f;
	vec2 ReflectDir = m_Dir;
	vec2 RefractDir = m_Dir;
	if(CrossedXFace)
	{
		ReflectDir.x = -ReflectDir.x;
		RefractDir.y *= Ratio; // tangential part, parallel to the surface
	}
	else
	{
		ReflectDir.y = -ReflectDir.y;
		RefractDir.x *= Ratio;
	}

	const float Rest = m_Energy - distance(m_Pos, EventPos) - GameServer()->Tuning()->m_LaserBounceCost;
	if(Rest <= 0)
	{
		GameWorld()->DestroyEntity(this);
		return;
	}

	// the reflected part continues from just outside the surface
	m_Energy = Rest;
	m_From = m_Pos;
	m_Pos = EventPos + ReflectDir * 3.0f;
	m_Dir = ReflectDir;
	GameWorld()->CreateSound(EventPos, SOUND_LASER_BOUNCE);

	// total internal reflection: when the refracted direction is too flat to
	// leave the water, only the reflection survives
	const float Tangential = CrossedXFace ? (RefractDir.y < 0 ? -RefractDir.y : RefractDir.y) : (RefractDir.x < 0 ? -RefractDir.x : RefractDir.x);
	const bool TotalReflection = !EnteringWater && Tangential >= 1.0f;
	// the refracted part becomes a new beam inside the water (offset a few
	// pixels so it does not touch the surface it came from again)
	if(!TotalReflection && Rest > 100.0f)
	{
		const vec2 RefrDir = normalize(RefractDir);
		new CLaser(GameWorld(), EventPos + RefrDir * 3.0f, RefrDir, Rest * 0.6f, m_Owner, m_Damage);
	}
}

void CLaser::Reset()
{
	GameWorld()->DestroyEntity(this);
}

void CLaser::Tick()
{
	if((Server()->Tick() - m_EvalTick) > (Server()->TickSpeed() * GameServer()->Tuning()->m_LaserBounceDelay) / 1000.0f)
		DoBounce();
}

void CLaser::TickPaused()
{
	++m_EvalTick;
}

void CLaser::Snap(int SnappingClient)
{
	if(NetworkClippedLine(SnappingClient, m_From, m_Pos))
		return;

	CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, GetID(), sizeof(CNetObj_Laser)));
	if(!pObj)
		return;

	pObj->m_X = round_to_int(m_Pos.x);
	pObj->m_Y = round_to_int(m_Pos.y);
	pObj->m_FromX = round_to_int(m_From.x);
	pObj->m_FromY = round_to_int(m_From.y);
	pObj->m_StartTick = m_EvalTick;
}
