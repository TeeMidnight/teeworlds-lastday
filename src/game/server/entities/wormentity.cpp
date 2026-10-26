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

namespace
{
const float WORM_SEGMENT_SPACING = 28.0f;
const int WORM_PATH_LENGTH = 1024;
const int WORM_BITE_COOLDOWN = 30;
} // namespace

//////////////////////////////////////////////////
// body segment
//////////////////////////////////////////////////

CWormSegmentEntity::CWormSegmentEntity(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const CBotTeeInfo &TeeInfo,
	int Team, int Faction, CWormEntity *pWorm) :
	CBotEntity(pWorld, Pos, BotID, TeeInfo, Team, Faction)
{
	m_pWorm = pWorm;
	m_Despawning = false;
	SetAI(nullptr);
	// the worm drives the body: no input strategy and no physics tick
	SetWormVisible(false);
}

CWormSegmentEntity::~CWormSegmentEntity()
{
	// The worm uses BotIDs, not this pointer, so nothing has to be unlinked
	// here. The worm nulls the back-pointer from its own destructor.
}

void CWormSegmentEntity::Reset()
{
	Despawn();
}

void CWormSegmentEntity::SetWormVisible(bool Visible)
{
	m_Alive = Visible;
	if(m_TeeID != -1 && GameWorld())
		GameWorld()->m_Core.SetCharacter(m_TeeID, Visible ? &m_Core : nullptr);
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
	if(m_Despawning)
		return;
	m_Despawning = true;
	m_Alive = false;
	if(m_TeeID != -1 && GameWorld())
		GameWorld()->m_Core.SetCharacter(m_TeeID, nullptr);
	CBotManager *pBots = GameWorld() ? GameWorld()->BotManager() : nullptr;
	if(pBots)
		pBots->OnBotDeath(m_BotID);
	GameWorld()->DestroyEntity(this);
}

//////////////////////////////////////////////////
// worm
//////////////////////////////////////////////////

CWormEntity::CWormEntity(CGameWorld *pWorld, vec2 Pos, const SMobDef *pDef, Uuid HeadID, const char *pName) :
	CHitableEntity(pWorld, CGameWorld::ENTTYPE_WORM, 0, Pos, 48)
{
	m_pDef = pDef;
	str_copy(m_aName, pName && pName[0] ? pName : (pDef ? pDef->m_aName : "Burrower"), sizeof(m_aName));
	m_MaxHealth = maximum(1, pDef ? pDef->m_Health : 10);
	m_Health = m_MaxHealth;
	m_Armor = pDef ? pDef->m_Armor : 0;
	m_Alive = true;
	m_Burrowed = true;
	m_StateTimer = pDef ? pDef->m_Worm.m_BurrowTicks : 90;
	m_BiteTimer = 0;
	m_LastHitTick = -100;
	m_WanderTimer = 0;
	m_WanderDir = vec2(1.0f, 0.0f);
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
		IBotEntity *pSegment = pBots->SpawnWormSegment(pWorld, this, BotID, TeeInfo, TEAM_RED, pDef->m_Faction);
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

void CWormEntity::Reset()
{
	m_Alive = false;
	SetVisible(false);
	for(int i = 0; i < m_lSegments.size(); i++)
	{
		IBotEntity *pBot = GameWorld()->HasBotManager() ? GameWorld()->GetBotManager()->BotByID(m_lSegments[i]) : nullptr;
		if(pBot && str_comp(pBot->BotTypeName(), "worm_segment") == 0)
			static_cast<CWormSegmentEntity *>(pBot)->Despawn();
	}
	m_lSegments.clear();
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

void CWormEntity::Tick()
{
	if(!m_Alive)
		return;

	if(m_StateTimer > 0)
		m_StateTimer--;
	if(m_StateTimer <= 0)
	{
		m_Burrowed = !m_Burrowed;
		m_StateTimer = m_Burrowed ? m_pDef->m_Worm.m_BurrowTicks : m_pDef->m_Worm.m_SurfaceTicks;
		SetVisible(!m_Burrowed);
		if(!m_Burrowed)
			GameWorld()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_SHORT);
	}

	CCharacter *pTarget = nullptr;
	if(GameWorld()->HasBotManager())
	{
		CEntity *pClosest = GameWorld()->ClosestEntity(m_Pos, m_pDef->m_Worm.m_ChaseRadius, CGameWorld::ENTTYPE_CHARACTER, nullptr);
		if(pClosest)
			pTarget = static_cast<CCharacter *>(pClosest);
	}

	vec2 Dir = m_WanderDir;
	if(pTarget && pTarget->IsAlive())
	{
		const vec2 Target = pTarget->GetPos();
		vec2 Delta = Target - m_Pos;
		if(length(Delta) > 0.001f)
			Dir = normalize(Delta);
		m_WanderDir = Dir;
	}
	else
	{
		if(Server()->Tick() >= m_WanderTimer)
		{
			m_WanderDir = normalize(vec2((float) (random_int() % 200 - 100), (float) (random_int() % 120 - 60)));
			m_WanderTimer = Server()->Tick() + Server()->TickSpeed() * 2;
		}
		Dir = m_WanderDir;
	}

	const float Speed = m_pDef->m_Worm.m_Speed * (m_Burrowed ? 1.15f : 0.75f);
	m_Pos += Dir * Speed;
	// keep the worm inside the playable bounds; GameLayerClipped only tests the
	// map rectangle, so burrowing through solid rock stays legal
	if(GameLayerClipped(m_Pos))
	{
		WormDie();
		return;
	}

	UpdateSegments();

	if(m_BiteTimer > 0)
		m_BiteTimer--;
	if(!m_Burrowed && pTarget && pTarget->IsAlive() && distance(pTarget->GetPos(), m_Pos) <= m_pDef->m_Worm.m_BiteRadius && m_BiteTimer <= 0)
	{
		Bite(pTarget);
		m_BiteTimer = WORM_BITE_COOLDOWN;
	}
}

void CWormEntity::Bite(CCharacter *pChr)
{
	if(!pChr || !pChr->IsAlive())
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
	if(!m_Alive || m_Burrowed)
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
	const char *pLine = Set.m_aaLines[random_int() % Set.m_NumLines];
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		if(!pPlayer || pPlayer->GameWorld() != GameWorld())
			continue;
		if(distance(pPlayer->m_ViewPos, m_Pos) > 800.0f)
			continue;
		CLanguageScope Scope(pPlayer->Language());
		char aBuf[384];
		str_format(aBuf, sizeof(aBuf), "%s: %s", m_aName, Localize(pLine, "Mob Dialogue"));
		GameServer()->SendChat(-1, CHAT_ALL, i, aBuf);
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
	CBotManager *pBots = GameWorld()->HasBotManager() ? GameWorld()->GetBotManager() : nullptr;
	if(pBots)
	{
		for(int i = 0; i < m_lSegments.size(); i++)
		{
			IBotEntity *pBot = pBots->BotByID(m_lSegments[i]);
			if(pBot && str_comp(pBot->BotTypeName(), "worm_segment") == 0)
				static_cast<CWormSegmentEntity *>(pBot)->Despawn();
		}
	}
	m_lSegments.clear();
	GameWorld()->DestroyEntity(this);
}
