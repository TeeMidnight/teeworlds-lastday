/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/localization.h>
#include <game/server/botmanager.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/item.h>
#include <game/server/player.h>

#include "dropped_pickup.h"
#include "wormentity.h"

static const float WORM_SEGMENT_SPACING = 28.0f;
static const int WORM_PATH_LENGTH = 1024;
static const int WORM_BITE_COOLDOWN = 30;

//////////////////////////////////////////////////
// body segment
//////////////////////////////////////////////////

CWormSegmentEntity::CWormSegmentEntity(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const CBotTeeInfo &TeeInfo,
	int Team, int Faction, CWormEntity *pWorm, bool IsHead, bool Natural) :
	CBotEntity(pWorld, Pos, BotID, TeeInfo, Team, Faction)
{
	m_pWorm = pWorm;
	m_Despawning = false;
	m_IsHead = IsHead;
	m_Natural = Natural;
	SetAI(nullptr);
	// the worm drives the body: no input strategy and no physics tick
	// The links are visible from birth; the old burrow cycle made the whole
	// creature untargetable and invisible for half of its life.
	SetWormVisible(true);
}

CWormSegmentEntity::~CWormSegmentEntity()
{
	// The worm uses BotIDs, not this pointer, so nothing has to be unlinked
	// from it here: the worm nulls the back-pointer from its own destructor.
	// The manager entry itself is retired by ~CBotEntity, so a segment that is
	// destroyed without Despawn() no longer leaves a dangling BotID behind.
}

void CWormSegmentEntity::Reset()
{
	Despawn();
}

void CWormSegmentEntity::SetWormVisible(bool Visible)
{
	m_Alive = Visible;
	// A hidden link must also leave the hit table: otherwise a projectile
	// aimed at empty ground would still stop on an invisible body.
	if(Visible)
		AddObjFlag(CGameWorld::ENTFLAG_HITABLE);
	else
		RemoveObjFlag(CGameWorld::ENTFLAG_HITABLE);
	if(m_TeeID != -1 && GameWorld())
	{
		if(Visible)
			GameWorld()->m_Core.SetCharacter(m_TeeID, &m_Core);
		else if(GameWorld()->m_Core.GetCharacter(m_TeeID) == &m_Core)
			GameWorld()->m_Core.SetCharacter(m_TeeID, nullptr);
	}
}

void CWormSegmentEntity::SetWormPos(vec2 Pos, vec2 Dir)
{
	m_Pos = Pos;
	m_Core.m_Pos = Pos;
	m_Core.m_Vel = vec2(0.0f, 0.0f);
	m_Core.m_HookPos = Pos;
	m_SendCore.m_Pos = Pos;
	m_SendCore.m_HookPos = Pos;
	m_ReckoningCore.m_Pos = Pos;
	m_ReckoningCore.m_HookPos = Pos;
	m_ReckoningTick = 0;

	if(length(Dir) > 0.001f)
	{
		m_Input.m_Direction = Dir.x < 0.0f ? -1 : (Dir.x > 0.0f ? 1 : 0);
		m_CursorTarget = normalize(Dir) * 100.0f;
	}
}

bool CWormSegmentEntity::TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	if(!m_pWorm || !m_Alive)
		return false;
	return m_pWorm->TakeHit(Force, Source, Dmg, pFrom, Weapon);
}

int CWormSegmentEntity::GetHitAttacker()
{
	return PLAYER_TEAM_HOSTILE;
}

void CWormSegmentEntity::Die(CEntity *pKiller, int Weapon)
{
	// the worm decides when a link disappears; a direct Die must not tear a
	// hole in the chain
	(void) pKiller;
	(void) Weapon;
}

void CWormSegmentEntity::Despawn()
{
	if(m_Despawning || !GameWorld())
		return;
	m_Despawning = true;
	m_Alive = false;
	// The TeeID may already belong to a replacement bot: never clear a core
	// that this segment no longer owns.
	if(m_TeeID != -1 && GameWorld()->m_Core.GetCharacter(m_TeeID) == &m_Core)
		GameWorld()->m_Core.SetCharacter(m_TeeID, nullptr);
	CBotManager *pBots = GameWorld()->BotManager();
	if(pBots)
		pBots->OnBotDeath(m_BotID);
	GameWorld()->DestroyEntity(this);
}

//////////////////////////////////////////////////
// worm
//////////////////////////////////////////////////

CWormEntity::CWormEntity(CGameWorld *pWorld, vec2 Pos, const SMobDef *pDef, Uuid HeadID, const char *pName, bool Natural) :
	CHitableEntity(pWorld, CGameWorld::ENTTYPE_WORM, 0, Pos, 48)
{
	m_pDef = pDef;
	m_Natural = Natural;
	str_copy(m_aName, pName && pName[0] ? pName : (pDef ? pDef->m_aName : "Burrower"), sizeof(m_aName));
	m_MaxHealth = maximum(1, pDef ? pDef->m_Health : 10);
	m_Health = m_MaxHealth;
	m_Armor = pDef ? pDef->m_Armor : 0;
	m_Alive = true;
	m_LastHitTick = -100;
	m_WanderTimer = 0;
	m_WanderDir = vec2(1.0f, 0.0f);
	m_PhaseTimer.start(Server()->Tick(), maximum(10, pDef ? pDef->m_Worm.m_SurfaceTicks : 70));
	m_Burrowed = false;
	m_HeadID = HeadID;

	m_lPath.hint_size(256);
	for(int i = 0; i < 256; i++)
		m_lPath.add(Pos);

	CBotManager *pBots = pWorld->BotManager();
	if(!pBots || !pDef)
	{
		GameWorld()->InsertEntity(this);
		return;
	}

	CBotTeeInfo TeeInfo;
	mem_zero(&TeeInfo, sizeof(TeeInfo));
	if(pDef->m_HasSkin)
		TeeInfo.m_Skin = pDef->m_Skin;
	str_copy(TeeInfo.m_aName, m_aName, sizeof(TeeInfo.m_aName));
	str_copy(TeeInfo.m_aClan, "Burrower", sizeof(TeeInfo.m_aClan));

	const int NumSegments = clamp(pDef->m_Worm.m_Segments, 2, 12);
	for(int i = 0; i < NumSegments; i++)
	{
		Uuid BotID = (i == 0 && m_HeadID != UUID_ZEROED) ? m_HeadID : random_uuid();
		if(pBots->BotByID(BotID))
			BotID = random_uuid();
		IBotEntity *pSegment = pBots->SpawnWormSegment(pWorld, this, BotID, TeeInfo, TEAM_RED, pDef->m_Faction, i == 0, m_Natural);
		if(pSegment)
			m_lSegments.add(BotID);
	}

	GameWorld()->InsertEntity(this);
}

CWormEntity::~CWormEntity()
{
	CBotManager *pBots = GameWorld() ? GameWorld()->BotManager() : nullptr;
	if(!pBots)
		return;
	for(int i = 0; i < m_lSegments.size(); i++)
	{
		IBotEntity *pBot = pBots->BotByID(m_lSegments[i]);
		if(pBot && str_comp(pBot->BotTypeName(), "worm_segment") == 0)
			static_cast<CWormSegmentEntity *>(pBot)->SetWorm(nullptr);
	}
}

IBotEntity *CWormEntity::HeadSegment()
{
	if(!GameWorld() || !GameWorld()->HasBotManager())
		return nullptr;
	IBotEntity *pBot = GameWorld()->GetBotManager()->BotByID(m_HeadID);
	if(pBot && str_comp(pBot->BotTypeName(), "worm_segment") == 0)
		return pBot;
	return nullptr;
}

void CWormEntity::SetVisible(bool Visible)
{
	for(int i = 0; i < m_lSegments.size(); i++)
	{
		IBotEntity *pBot = GameWorld()->HasBotManager() ? GameWorld()->GetBotManager()->BotByID(m_lSegments[i]) : nullptr;
		if(pBot && str_comp(pBot->BotTypeName(), "worm_segment") == 0)
			static_cast<CWormSegmentEntity *>(pBot)->SetWormVisible(Visible);
	}
}

void CWormEntity::DespawnSegments()
{
	array<Uuid> lSegments = m_lSegments;
	m_lSegments.clear();
	CBotManager *pBots = GameWorld()->HasBotManager() ? GameWorld()->GetBotManager() : nullptr;
	if(!pBots)
		return;
	for(int i = 0; i < lSegments.size(); i++)
	{
		IBotEntity *pBot = pBots->BotByID(lSegments[i]);
		if(!pBot || str_comp(pBot->BotTypeName(), "worm_segment") != 0)
			continue;
		CWormSegmentEntity *pSegment = static_cast<CWormSegmentEntity *>(pBot);
		// No link may call back into a worm that is being torn down.
		pSegment->SetWorm(nullptr);
		pSegment->Despawn();
	}
}

void CWormEntity::Reset()
{
	m_Alive = false;
	SetVisible(false);
	DespawnSegments();
	GameWorld()->DestroyEntity(this);
}

void CWormEntity::UpdateSegments()
{
	m_lPath.add(m_Pos);
	while(m_lPath.size() > WORM_PATH_LENGTH)
		m_lPath.remove_index(0);

	for(int s = 0; s < m_lSegments.size(); s++)
	{
		IBotEntity *pBot = GameWorld()->HasBotManager() ? GameWorld()->GetBotManager()->BotByID(m_lSegments[s]) : nullptr;
		if(!pBot || str_comp(pBot->BotTypeName(), "worm_segment") != 0)
			continue;

		const int Index = m_lPath.size() - 1;
		vec2 Prev = m_lPath[Index];
		vec2 Point = Prev;
		float Remaining = s * WORM_SEGMENT_SPACING;
		vec2 Dir = m_WanderDir;

		for(int i = Index - 1; i >= 0; i--)
		{
			const vec2 Next = m_lPath[i];
			const float Dist = distance(Prev, Next);
			if(Remaining <= Dist)
			{
				if(Dist > 0.001f)
					Point = Prev + (Next - Prev) * (Remaining / Dist);
				else
					Point = Next;
				Dir = Prev - Next;
				break;
			}
			Remaining -= Dist;
			Point = Next;
			Prev = Next;
			Dir = Prev - (i > 0 ? m_lPath[i - 1] : Prev);
		}

		static_cast<CWormSegmentEntity *>(pBot)->SetWormPos(Point, Dir);
	}
}

// The first solid tile at X at or below FromY is the floor the worm stands
// on. Starting at the worm's own row lets it step onto a one-tile ledge, while
// a taller wall leaves the candidate centre inside the wall and is rejected by
// the clearance test below.
static bool FindWormGround(CGameWorld *pWorld, float X, float FromY, float *pGroundTop)
{
	CCollision *pCollision = pWorld->Collision();
	const int Height = pCollision->GetHeight();
	const int StartY = maximum(0, (int) (FromY / 32.0f));
	for(int Y = StartY; Y < Height; Y++)
	{
		if(pCollision->CheckPoint(vec2(X, Y * 32.0f + 16.0f)))
		{
			*pGroundTop = Y * 32.0f;
			return true;
		}
	}
	return false;
}

void CWormEntity::Tick()
{
	if(!m_Alive || !m_pDef)
		return;

	CCharacter *pTarget = nullptr;
	if(GameWorld()->HasBotManager())
	{
		CEntity *pClosest = GameWorld()->ClosestEntity(m_Pos, m_pDef->m_Worm.m_ChaseRadius, CGameWorld::ENTTYPE_CHARACTER, nullptr);
		if(pClosest)
			pTarget = static_cast<CCharacter *>(pClosest);
	}

	// Alternate a surface window (visible and hittable) with a burrowed
	// window (hidden and invulnerable). The timers come from the mob JSON, so
	// each burrower can be tuned without touching the entity.
	if(m_PhaseTimer.expired(Server()->Tick()))
	{
		if(m_Burrowed)
		{
			if(!TrySurface(pTarget))
				m_PhaseTimer.start(Server()->Tick(), 15); // no clear floor at this column yet
		}
		else
			EnterBurrow();
	}

	float Direction = 0.0f;
	if(pTarget && pTarget->IsAlive())
	{
		const vec2 Delta = pTarget->GetPos() - m_Pos;
		if(absolute(Delta.x) > 1.0f)
			Direction = Delta.x < 0.0f ? -1.0f : 1.0f;
		m_WanderDir = vec2(Direction, 0.0f);
	}
	else
	{
		if(Server()->Tick() >= m_WanderTimer)
		{
			m_WanderDir = vec2((random_int() % 2) ? 1.0f : -1.0f, 0.0f);
			m_WanderTimer = Server()->Tick() + Server()->TickSpeed() * 2;
		}
		Direction = m_WanderDir.x < 0.0f ? -1.0f : (m_WanderDir.x > 0.0f ? 1.0f : 0.0f);
	}

	const float Speed = m_pDef->m_Worm.m_Speed;
	if(m_Burrowed)
	{
		// Tunnelling ignores terrain entirely. The links are hidden, and the
		// regular dirt puffs show the player roughly where it is heading.
		if(Direction != 0.0f)
		{
			const float MaxX = GameWorld()->Collision()->GetWidth() * 32.0f - 16.0f;
			float NewX = m_Pos.x + Direction * Speed;
			if(NewX < 16.0f || NewX > MaxX)
			{
				NewX = clamp(NewX, 16.0f, MaxX);
				m_WanderDir.x = -m_WanderDir.x;
			}
			m_Pos.x = NewX;

			float GroundTop = 0.0f;
			if(FindWormGround(GameWorld(), m_Pos.x, m_Pos.y - 96.0f, &GroundTop))
			{
				const float NewY = GroundTop - 14.0f;
				// Follow the floor between short hops, but do not snap over a
				// tall wall: the worm is supposed to pass underneath it.
				if(absolute(NewY - m_Pos.y) <= 4.0f * 32.0f)
					m_Pos.y = NewY;
			}
		}
		BurrowRumble();
	}
	else
	{
		// Only horizontal motion is driven here. The vertical position is always
		// snapped back to the first floor below the worm, so it crawls over the
		// terrain instead of flying at the player through the air.
		if(Direction != 0.0f)
		{
			const float NewX = m_Pos.x + Direction * Speed;
			float GroundTop = 0.0f;
			if(FindWormGround(GameWorld(), NewX, m_Pos.y, &GroundTop))
			{
				const vec2 NewPos(NewX, GroundTop - 14.0f);
				const bool SameLevel = absolute(NewPos.y - m_Pos.y) <= 1.5f * 32.0f;
				const bool Clear = !GameWorld()->Collision()->TestBox(NewPos, vec2(24.0f, 24.0f));
				if(SameLevel && Clear)
					m_Pos = NewPos;
				else
					m_WanderDir.x = -m_WanderDir.x;
			}
			else
				m_WanderDir.x = -m_WanderDir.x;
		}
		else
		{
			// keep resting on the floor even while turning or waiting
			float GroundTop = 0.0f;
			if(FindWormGround(GameWorld(), m_Pos.x, m_Pos.y, &GroundTop))
			{
				const float NewY = GroundTop - 14.0f;
				if(absolute(NewY - m_Pos.y) <= 1.5f * 32.0f)
					m_Pos.y = NewY;
			}
		}
	}

	if(GameLayerClipped(m_Pos))
	{
		WormDie();
		return;
	}

	UpdateSegments();

	if(m_Burrowed)
		return;

	if(pTarget && pTarget->IsAlive() && distance(pTarget->GetPos(), m_Pos) <= m_pDef->m_Worm.m_BiteRadius &&
		m_BiteTimer.expired(Server()->Tick()))
	{
		Bite(pTarget);
		m_BiteTimer.start(Server()->Tick(), WORM_BITE_COOLDOWN);
	}
}

void CWormEntity::EnterBurrow()
{
	if(!m_pDef || m_Burrowed)
		return;

	m_Burrowed = true;
	m_PhaseTimer.start(Server()->Tick(), maximum(10, m_pDef->m_Worm.m_BurrowTicks));
	m_RumbleTimer.stop();
	m_BiteTimer.stop();
	RemoveObjFlag(CGameWorld::ENTFLAG_HITABLE);
	SetVisible(false);
	GameWorld()->CreateHammerHit(m_Pos);
	GameWorld()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_GROUND);
}

bool CWormEntity::TrySurface(CCharacter *pTarget)
{
	if(!m_pDef)
		return false;

	// Prefer the floor under the prey, so the worm emerges in the same fight
	// space instead of on top of a random ceiling. Without prey it simply
	// re-emerges at its current column.
	const float FromY = pTarget && pTarget->IsAlive() ? pTarget->GetPos().y - 96.0f : m_Pos.y - 96.0f;
	float GroundTop = 0.0f;
	vec2 NewPos = m_Pos;
	if(FindWormGround(GameWorld(), m_Pos.x, FromY, &GroundTop))
		NewPos.y = GroundTop - 14.0f;

	if(GameWorld()->Collision()->TestBox(NewPos, vec2(24.0f, 24.0f)))
		return false;

	m_Pos = NewPos;
	m_Burrowed = false;
	m_PhaseTimer.start(Server()->Tick(), maximum(10, m_pDef->m_Worm.m_SurfaceTicks));
	// A short delay after breaking the surface gives the player time to react
	// before the first bite lands.
	m_BiteTimer.start(Server()->Tick(), 18);
	AddObjFlag(CGameWorld::ENTFLAG_HITABLE);
	SetVisible(true);
	GameWorld()->CreateHammerHit(m_Pos);
	GameWorld()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_GROUND);
	return true;
}

void CWormEntity::BurrowRumble()
{
	// the original countdown returned on the 18 decrementing ticks and ran the
	// effect on the 19th, so the deadline is 19 ticks out
	if(m_RumbleTimer.active(Server()->Tick()))
		return;
	m_RumbleTimer.start(Server()->Tick(), 19);

	float GroundTop = 0.0f;
	if(FindWormGround(GameWorld(), m_Pos.x, m_Pos.y - 96.0f, &GroundTop))
		GameWorld()->CreateHammerHit(vec2(m_Pos.x, GroundTop - 4.0f));
}

void CWormEntity::Bite(CCharacter *pChr)
{
	if(!pChr || !pChr->IsAlive() || m_Burrowed)
		return;
	vec2 Dir = pChr->GetPos() - m_Pos;
	if(length(Dir) < 0.001f)
		Dir = vec2(0.0f, 1.0f);
	Dir = normalize(Dir);
	pChr->TakeHit(Dir * 3.0f + vec2(0.0f, -1.0f), Dir * -1.0f, m_pDef->m_Worm.m_Damage, this, WEAPON_WORLD);
	GameWorld()->CreateSound(m_Pos, SOUND_HAMMER_HIT);
}

bool CWormEntity::TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	(void) Force;
	(void) Source;
	(void) pFrom;
	(void) Weapon;
	if(!m_Alive)
		return false;

	// While tunnelling the links are hidden, so there is nothing to aim at.
	// Explosions can still reach the worm entity itself, so reject those too:
	// the surface window is the fight.
	if(m_Burrowed)
		return false;

	// an explosion can touch every link at once; one hit per few ticks is
	// enough, otherwise a single grenade would delete the whole worm
	if(Server()->Tick() - m_LastHitTick < 3)
		return false;
	m_LastHitTick = Server()->Tick();

	if(m_Armor > 0)
	{
		if(Dmg > m_Armor)
		{
			Dmg -= m_Armor;
			m_Armor = 0;
		}
		else
		{
			m_Armor -= Dmg;
			Dmg = 0;
		}
	}
	m_Health -= Dmg;
	GameWorld()->CreateSound(m_Pos, Dmg > 2 ? SOUND_PLAYER_PAIN_LONG : SOUND_PLAYER_PAIN_SHORT);

	if(m_Health <= 0)
	{
		WormDie();
		return false;
	}
	return true;
}

int CWormEntity::GetHitAttacker()
{
	return PLAYER_TEAM_HOSTILE;
}

void CWormEntity::Snap(int SnappingClient)
{
	// the head and body are separate bot snapshots; this entity has no visual
	// of its own. Keeping it invisible also means a buried worm has no marker.
	(void) SnappingClient;
}

void CWormEntity::Speak(int Event)
{
	if(!m_pDef || Event < 0 || Event >= NUM_MOBDIALOGUE_EVENTS)
		return;
	const SDialogueSet &Set = m_pDef->m_Dialogue.m_aEvents[Event];
	if(Set.m_NumLines <= 0)
		return;
	// the worm itself has no tee; the head segment is the visible identity
	IBotEntity *pHead = HeadSegment();
	if(!pHead)
		return;

	const char *pLine = Set.m_aaLines[random_int() % Set.m_NumLines];
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		if(!pPlayer || pPlayer->GameWorld() != GameWorld())
			continue;
		if(distance(pPlayer->m_ViewPos, m_Pos) > 800.0f)
			continue;
		CLanguageScope Scope(pPlayer->Language());
		// Translate first: the translation key still carries {player}.
		char aMsg[MAX_MOB_DIALOGUE_LENGTH * 2];
		str_copy(aMsg, Localize(pLine, "Mob Dialogue"), sizeof(aMsg));
		ReplaceDialoguePlayerToken(aMsg, sizeof(aMsg), Server()->ClientName(i));
		GameServer()->SendBotChat(pHead, i, aMsg);
	}
}

void CWormEntity::DropLoot()
{
	if(!m_pDef)
		return;
	for(int i = 0; i < m_pDef->m_lDrops.size(); i++)
	{
		const SMobDrop &Drop = m_pDef->m_lDrops[i];
		if(Drop.m_Chance <= 0.0f)
			continue;
		if(Drop.m_Chance < 1.0f && (float) (random_int() % 10000) / 10000.0f >= Drop.m_Chance)
			continue;
		if(!GameServer()->Item() || !GameServer()->Item()->IsKnownItem(Drop.m_Id))
			continue;
		const int Count = Drop.m_CountMin + (Drop.m_CountMax > Drop.m_CountMin ? random_int() % (Drop.m_CountMax - Drop.m_CountMin + 1) : 0);
		if(Count <= 0)
			continue;
		const vec2 Dir((float) (random_int() % 200 - 100) / 100.0f, -1.0f);
		new CDroppedPickup(GameWorld(), m_Pos + vec2(0.0f, -24.0f), Dir, Drop.m_Id, Count);
	}

	// same money pile rule as the monster entity: a bundle of G split over
	// several physical drops, each drawn as a weapon projectile
	for(int i = 0; i < m_pDef->m_lMoneyDrops.size(); i++)
	{
		const SMobMoneyDrop &Drop = m_pDef->m_lMoneyDrops[i];
		if(Drop.m_Chance <= 0.0f)
			continue;
		if(Drop.m_Chance < 1.0f && (float) (random_int() % 10000) / 10000.0f >= Drop.m_Chance)
			continue;

		const int Total = Drop.m_Min + (Drop.m_Max > Drop.m_Min ? random_int() % (Drop.m_Max - Drop.m_Min + 1) : 0);
		if(Total <= 0)
			continue;

		int Remaining = Total;
		for(int P = 0; P < Drop.m_Piles && Remaining > 0; P++)
		{
			const int PileAmount = Remaining / (Drop.m_Piles - P);
			if(PileAmount <= 0)
				continue;
			Remaining -= PileAmount;
			const vec2 Dir((float) (random_int() % 240 - 120) / 100.0f, -1.2f - (float) (random_int() % 120) / 100.0f);
			const vec2 Offset((float) (random_int() % 24 - 12), -24.0f);
			new CDroppedPickup(GameWorld(), m_Pos + Offset, Dir, (int64) PileAmount, CDroppedPickup::KIND_CURRENCY);
		}
	}
}

void CWormEntity::WormDie()
{
	if(!m_Alive)
		return;
	m_Alive = false;
	Speak(MOBDIALOGUE_DEATH);
	DropLoot();
	GameWorld()->CreateSound(m_Pos, SOUND_PLAYER_DIE);

	SetVisible(false);
	DespawnSegments();
	GameWorld()->DestroyEntity(this);
}
