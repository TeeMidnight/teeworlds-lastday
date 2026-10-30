/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/localization.h>
#include <game/server/botmanager.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/item.h>
#include <game/server/mobdata.h>
#include <game/server/player.h>

#include "character.h"
#include "dropped_pickup.h"
#include "monsterentity.h"
#include "projectile.h"

static const int s_aDialogueCooldown[NUM_MOBDIALOGUE_EVENTS] = {
	0, // spawn, one shot by the spawn handshake
	1800, // sight, 36s at 50Hz
	900, // combat, 18s
	600, // hurt, 12s
	1200, // kill, 24s
	0, // death
	6000, // idle, 120s: ambient chatter, not a running commentary
	0, // low health, one shot by the health gate
};

static int FactionTeam(int Faction)
{
	return Faction == BOTFACTION_FRIENDLY ? TEAM_BLUE : TEAM_RED;
}

static const char *FactionClan(int Faction)
{
	switch(Faction)
	{
		case BOTFACTION_HOSTILE: return "Hostile";
		case BOTFACTION_NEUTRAL: return "Neutral";
		case BOTFACTION_FRIENDLY: return "Friendly";
	}
	return "";
}

CMonsterEntity::CMonsterEntity(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const SMobDef *pDef, const char *pName, bool Natural) :
	CBotEntity(pWorld, Pos, BotID, CBotTeeInfo(), FactionTeam(pDef ? pDef->m_Faction : BOTFACTION_HOSTILE), pDef ? pDef->m_Faction : BOTFACTION_HOSTILE)
{
	m_pDef = pDef;
	m_Natural = Natural;
	m_SpawnPos = Pos;
	m_SpawnWorldID = pWorld ? pWorld->WorldID() : 0;
	m_LastSpeakTick = -1000;
	m_SpawnAnnounceTick = -1;
	for(int i = 0; i < NUM_MOBDIALOGUE_EVENTS; i++)
		m_aLastEventTick[i] = -1000;
	m_SaidSight = false;
	m_SaidLowHealth = false;
	m_Enraged = false;
	m_Summoned = false;
	m_SpawnGroup = 0;
	m_RetaliateUntil = 0;
	m_LastAttackerBot = UUID_ZEROED;
	m_LastAttackerClient = -1;
	m_WanderDir = random_int() % 3 - 1;
	m_WanderChangeTick = 0;
	m_LastTargetTick = 0;
	m_pLastTarget = nullptr;

	m_aName[0] = '\0';
	const char *pDisplayName = pName && pName[0] ? pName : (pDef ? pDef->m_aName : "monster");
	str_copy(m_aName, pDisplayName, sizeof(m_aName));
	str_copy(m_TeeInfo.m_aName, m_aName, sizeof(m_TeeInfo.m_aName));
	str_copy(m_TeeInfo.m_aClan, FactionClan(m_pDef ? m_pDef->m_Faction : BOTFACTION_HOSTILE), sizeof(m_TeeInfo.m_aClan));

	if(pDef && pDef->m_HasSkin)
		m_TeeInfo.m_Skin = pDef->m_Skin;

	if(pDef)
	{
		m_MaxHealth = maximum(1, pDef->m_Health);
		m_Health = m_MaxHealth;
		m_MaxArmor = CBotEntity::MAX_ARMOR;
		m_Armor = clamp(pDef->m_Armor, 0, m_MaxArmor);
		m_Core.m_SpeedScale = pDef->m_SpeedScale;
		SetMeleeTuning(pDef->m_Melee.m_Damage, pDef->m_Melee.m_Cooldown, pDef->m_Melee.m_Range);
		if(pDef->m_Ranged.m_Enabled)
			m_WeaponSlot = clamp(pDef->m_Ranged.m_Weapon, 0, NUM_WEAPONS - 1);
	}
	// pick up the definition's speed scale before the first snapshot, even if
	// the first Tick lands after the bot list was already walked
	UpdateTuning();

	// the content AI drives the body directly; the generic strategy layer
	// would only add a second, conflicting input pass
	SetAI(nullptr);

	// The spawn line is a chat from the bot's own tee id, and that id only
	// exists after the manager has assigned it and a snapshot has told the
	// client the name. Announce a couple of ticks later for that handshake.
	m_SpawnAnnounceTick = Server()->Tick() + 2;
}

bool CMonsterEntity::IsHostileBot(const IBotEntity *pBot) const
{
	if(!pBot)
		return false;
	// Only friendly units start bot-vs-bot fights: they defend players from
	// hostile monsters. Hostile and neutral monsters are neutral to every
	// other bot until one hits them; that retaliation is resolved separately
	// in IsValidTarget() so an accidental hit still turns into a fight.
	return m_pDef->m_Faction == BOTFACTION_FRIENDLY && pBot->GetBotFaction() == BOTFACTION_HOSTILE;
}

bool CMonsterEntity::IsHostilePlayer(const CCharacter *pChr) const
{
	if(!pChr || !pChr->IsAlive())
		return false;
	if(m_pDef->m_Faction != BOTFACTION_HOSTILE)
		return false;
	return true;
}

bool CMonsterEntity::CanSee(CEntity *pTarget)
{
	if(!pTarget)
		return false;
	if(GameWorld()->Collision()->IntersectLine(m_Pos, GameWorld()->PosInFrame(pTarget), nullptr, nullptr))
		return false;
	return true;
}

bool CMonsterEntity::IsValidTarget(CEntity *pTarget, bool RequireLineOfSight)
{
	if(!pTarget || pTarget == this)
		return false;
	if(pTarget->IsMarkedForDestroy())
		return false;

	if(pTarget->ObjType() == CGameWorld::ENTTYPE_CHARACTER)
	{
		CCharacter *pChr = static_cast<CCharacter *>(pTarget);
		if(!IsHostilePlayer(pChr))
		{
			// a neutral monster may answer the player that hit it
			if(m_pDef->m_Faction != BOTFACTION_NEUTRAL || m_RetaliateUntil <= Server()->Tick())
				return false;
			if(pChr->GetCID() != m_LastAttackerClient)
				return false;
		}
	}
	else if(pTarget->ObjType() == CGameWorld::ENTTYPE_BOTENTITY)
	{
		IBotEntity *pBot = static_cast<IBotEntity *>(pTarget);
		if(!pBot->IsAlive())
			return false;

		// Every non-friendly faction answers the exact bot that hit it. This
		// is what turns an accidental hit between two otherwise neutral
		// monsters into a duel without making them hunt each other on sight.
		const bool Retaliating = m_pDef->m_Faction != BOTFACTION_FRIENDLY &&
			m_RetaliateUntil > Server()->Tick() &&
			m_LastAttackerBot != UUID_ZEROED && pBot->GetBotID() == m_LastAttackerBot;
		if(!Retaliating && !IsHostileBot(pBot))
			return false;
	}
	else
		return false;

	if(distance(GameWorld()->PosInFrame(pTarget), m_Pos) > m_pDef->m_AggroRadius * 1.5f)
		return false;
	if(RequireLineOfSight && !CanSee(pTarget))
		return false;
	return true;
}

CEntity *CMonsterEntity::RetaliationTarget()
{
	if(m_RetaliateUntil <= Server()->Tick())
		return nullptr;

	if(m_LastAttackerClient >= 0 && m_LastAttackerClient < MAX_CLIENTS)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[m_LastAttackerClient];
		if(pPlayer && pPlayer->GameWorld() == GameWorld())
		{
			CCharacter *pChr = pPlayer->GetCharacter();
			if(pChr && pChr->IsAlive() && IsValidTarget(pChr, true))
				return pChr;
		}
	}

	if(m_LastAttackerBot != UUID_ZEROED)
	{
		IBotEntity *pBot = GameWorld()->BotManager() ? GameWorld()->BotManager()->BotByID(m_LastAttackerBot) : nullptr;
		if(pBot && pBot->IsAlive() && IsValidTarget(static_cast<CEntity *>(pBot), true))
			return static_cast<CEntity *>(pBot);
	}
	return nullptr;
}

CEntity *CMonsterEntity::FindMonsterTarget()
{
	CEntity *pRetaliation = RetaliationTarget();
	if(pRetaliation)
		return pRetaliation;

	if(m_pDef->m_Faction == BOTFACTION_NEUTRAL)
		return nullptr;

	array<CEntity *> lCandidates;
	lCandidates.hint_size(16);
	GameWorld()->FindFlagEntities(m_Pos, m_pDef->m_AggroRadius, lCandidates, CGameWorld::ENTFLAG_HITABLE);

	CEntity *pBest = nullptr;
	float BestDist = 1e30f;
	for(int i = 0; i < lCandidates.size(); i++)
	{
		CEntity *pCandidate = lCandidates[i];
		if(!IsValidTarget(pCandidate, true))
			continue;
		const float Dist = distance(GameWorld()->PosInFrame(pCandidate), m_Pos);
		if(Dist < BestDist)
		{
			BestDist = Dist;
			pBest = pCandidate;
		}
	}
	return pBest;
}

CCharacter *CMonsterEntity::NearestPlayer(float Radius)
{
	CCharacter *pBest = nullptr;
	float BestDist = Radius;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		if(!pPlayer || pPlayer->GameWorld() != GameWorld())
			continue;
		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;
		const float Dist = distance(pChr->GetPos(), m_Pos);
		if(Dist <= BestDist)
		{
			BestDist = Dist;
			pBest = pChr;
		}
	}
	return pBest;
}

void CMonsterEntity::OnBotTick()
{
	if(m_SpawnAnnounceTick >= 0 && Server()->Tick() >= m_SpawnAnnounceTick)
	{
		SayEvent(MOBDIALOGUE_SPAWN, true);
		m_SpawnAnnounceTick = -1;
	}
	CBotEntity::OnBotTick();
}

void CMonsterEntity::OnAction()
{
	if(!m_Alive || !m_pDef)
		return;

	TickAuras();
	TickEnrage();
	TrySummon();
	TickDialogue();

	CEntity *pTarget = FindMonsterTarget();
	m_pLastTarget = pTarget;
	m_LastTargetTick = Server()->Tick();

	const char *pAI = m_pDef->m_aAI;
	if(str_comp_nocase(pAI, "skirmisher") == 0 || str_comp_nocase(pAI, "ranged") == 0)
		ThinkSkirmisher(pTarget);
	else if(str_comp_nocase(pAI, "sentry") == 0)
		ThinkSentry(pTarget);
	else if(str_comp_nocase(pAI, "wanderer") == 0)
		ThinkWanderer(pTarget);
	else if(str_comp_nocase(pAI, "companion") == 0)
		ThinkCompanion(pTarget);
	else if(str_comp_nocase(pAI, "support") == 0)
		ThinkSupport(pTarget);
	else
		ThinkHunter(pTarget);

	AimCursor();
}

void CMonsterEntity::WanderStep()
{
	if(Server()->Tick() >= m_WanderChangeTick)
	{
		m_WanderDir = random_int() % 3 - 1;
		m_WanderChangeTick = Server()->Tick() + Server()->TickSpeed() * (1 + random_int() % 3);
	}

	// stay near the spawn point; content spawns are anchors, not patrol routes
	if(distance(m_Pos, m_SpawnPos) > 180.0f)
		m_WanderDir = m_SpawnPos.x < m_Pos.x ? -1 : 1;

	m_Input.m_Direction = m_WanderDir;
	if(m_WanderDir)
		m_CursorTarget = vec2(length(m_CursorTarget) * m_WanderDir, 0.0f);
	if(random_int() % 1000 < m_pDef->m_JumpChance)
		m_Input.m_Jump = 1;
}

void CMonsterEntity::ThinkHunter(CEntity *pTarget)
{
	if(!pTarget)
	{
		WanderStep();
		SayEvent(MOBDIALOGUE_IDLE);
		return;
	}

	const vec2 Target = GameWorld()->PosInFrame(pTarget);
	const float Dist = distance(Target, m_Pos);
	const float DX = Target.x - m_Pos.x;

	SayEvent(m_SaidSight ? MOBDIALOGUE_COMBAT : MOBDIALOGUE_SIGHT);
	if(!m_SaidSight)
		m_SaidSight = true;

	if(m_pDef->m_Ranged.m_Enabled && Dist <= m_pDef->m_Ranged.m_Range &&
		Dist >= m_pDef->m_Ranged.m_MinRange && CanSee(pTarget))
		FireRanged(Target);

	if(Dist > m_pDef->m_Melee.m_Range)
	{
		m_Input.m_Direction = (int) sign(DX);
		if(Target.y < m_Pos.y - 40.0f && (m_Core.m_Jumped & 1) == 0 && m_Core.m_Vel.y > -1.0f)
			m_Input.m_Jump = 1;
	}
	else
	{
		m_Input.m_Direction = 0;
		m_Input.m_Fire = 1;
	}

	if(TryLeap(Target))
		return;
	m_CursorTarget = Target - m_Pos;
}

void CMonsterEntity::ThinkSkirmisher(CEntity *pTarget)
{
	if(!pTarget)
	{
		WanderStep();
		SayEvent(MOBDIALOGUE_IDLE);
		return;
	}

	const vec2 Target = GameWorld()->PosInFrame(pTarget);
	const float Dist = distance(Target, m_Pos);
	const float DX = Target.x - m_Pos.x;
	const float Keep = maximum(m_pDef->m_KeepDistance, m_pDef->m_Ranged.m_MinRange);

	SayEvent(m_SaidSight ? MOBDIALOGUE_COMBAT : MOBDIALOGUE_SIGHT);
	m_SaidSight = true;

	if(Dist < Keep - 16.0f)
		m_Input.m_Direction = DX > 0.0f ? -1 : 1; // back away
	else if(Dist > m_pDef->m_Ranged.m_Range)
		m_Input.m_Direction = (int) sign(DX);
	else
		m_Input.m_Direction = 0;

	if(Target.y < m_Pos.y - 48.0f && (m_Core.m_Jumped & 1) == 0 && m_Core.m_Vel.y > -1.0f)
		m_Input.m_Jump = 1;
	if(random_int() % 1000 < m_pDef->m_JumpChance)
		m_Input.m_Jump = 1;

	if(m_pDef->m_Ranged.m_Enabled && Dist <= m_pDef->m_Ranged.m_Range && Dist >= m_pDef->m_Ranged.m_MinRange && CanSee(pTarget))
		FireRanged(Target);

	m_CursorTarget = Target - m_Pos;
}

void CMonsterEntity::ThinkSentry(CEntity *pTarget)
{
	const float DistFromHome = distance(m_Pos, m_SpawnPos);
	if(pTarget && distance(GameWorld()->PosInFrame(pTarget), m_SpawnPos) > m_pDef->m_AggroRadius)
		pTarget = nullptr;

	if(!pTarget)
	{
		if(DistFromHome > 48.0f)
		{
			m_Input.m_Direction = m_SpawnPos.x < m_Pos.x ? -1 : (m_SpawnPos.x > m_Pos.x ? 1 : 0);
			if(m_SpawnPos.y < m_Pos.y - 32.0f)
				m_Input.m_Jump = 1;
			m_CursorTarget = vec2(length(m_CursorTarget) * m_Input.m_Direction, 0.0f);
		}
		else
			WanderStep();
		SayEvent(MOBDIALOGUE_IDLE);
		return;
	}

	if(DistFromHome > m_pDef->m_AggroRadius || DistFromHome > 480.0f)
	{
		ThinkWanderer(nullptr);
		return;
	}

	ThinkHunter(pTarget);
}

void CMonsterEntity::ThinkWanderer(CEntity *pTarget)
{
	if(pTarget)
	{
		ThinkHunter(pTarget);
		return;
	}

	WanderStep();
	CCharacter *pNear = NearestPlayer(240.0f);
	if(pNear)
	{
		m_CursorTarget = pNear->GetPos() - m_Pos;
		SayEvent(MOBDIALOGUE_SIGHT);
	}
	else
		SayEvent(MOBDIALOGUE_IDLE);
}

void CMonsterEntity::ThinkCompanion(CEntity *pTarget)
{
	if(pTarget)
	{
		ThinkSkirmisher(pTarget);
		return;
	}

	CCharacter *pPlayer = NearestPlayer(700.0f);
	if(!pPlayer)
	{
		WanderStep();
		return;
	}

	const vec2 Target = pPlayer->GetPos();
	const float Dist = distance(Target, m_Pos);
	const float DX = Target.x - m_Pos.x;
	if(Dist > 140.0f)
		m_Input.m_Direction = (int) sign(DX);
	else if(Dist < 64.0f)
		m_Input.m_Direction = DX > 0.0f ? -1 : 1;
	else
		m_Input.m_Direction = 0;

	if(Target.y < m_Pos.y - 48.0f && (m_Core.m_Jumped & 1) == 0 && m_Core.m_Vel.y > -1.0f)
		m_Input.m_Jump = 1;
	m_CursorTarget = Target - m_Pos;
	SayEvent(MOBDIALOGUE_SIGHT);
}

void CMonsterEntity::ThinkSupport(CEntity *)
{
	CCharacter *pPlayer = NearestPlayer(800.0f);
	if(!pPlayer)
	{
		WanderStep();
		return;
	}

	const vec2 Target = pPlayer->GetPos();
	const float Dist = distance(Target, m_Pos);
	const float DX = Target.x - m_Pos.x;
	if(Dist > 160.0f)
		m_Input.m_Direction = (int) sign(DX);
	else if(Dist < 72.0f)
		m_Input.m_Direction = DX > 0.0f ? -1 : 1;
	else
		m_Input.m_Direction = 0;

	if(Target.y < m_Pos.y - 48.0f && (m_Core.m_Jumped & 1) == 0 && m_Core.m_Vel.y > -1.0f)
		m_Input.m_Jump = 1;
	m_CursorTarget = Target - m_Pos;
}

bool CMonsterEntity::FireRanged(vec2 Target)
{
	if(!m_pDef->m_Ranged.m_Enabled || m_RangedTimer.active(Server()->Tick()))
		return false;

	vec2 Dir = Target - m_Pos;
	if(length(Dir) < 1.0f)
		return false;
	Dir = normalize(Dir);

	// A ranged mob aims at the target's current position, so a perfectly
	// accurate shot is impossible to dodge at these projectile speeds. The
	// content-defined spread gives the player a real chance to strafe out of
	// the line, while keeping the mob threatening at close range.
	if(m_pDef->m_Ranged.m_AimSpread > 0.0f)
	{
		const float Offset = (random_float() * 2.0f - 1.0f) * m_pDef->m_Ranged.m_AimSpread;
		Dir = rotate(Dir, Offset);
	}

	const int Owner = m_pDef->m_Faction == BOTFACTION_FRIENDLY ? PLAYER_TEAM_FRIENDLY : PLAYER_TEAM_HOSTILE;
	const vec2 Start = m_Pos + Dir * GetProximityRadius() * 0.75f;
	CProjectile *pProjectile = new CProjectile(GameWorld(), m_pDef->m_Ranged.m_Weapon, Owner, Start, Dir,
		m_pDef->m_Ranged.m_Span, m_pDef->m_Ranged.m_Damage, false,
		(float) m_pDef->m_Ranged.m_Force, m_pDef->m_Ranged.m_Sound, m_pDef->m_Ranged.m_Weapon);
	// A bot has no client owner for the projectile to resolve, so without this
	// the shot can collide with the monster that fired it on the very next tick.
	pProjectile->SetIgnoreEntity(GetID());
	// The team sentinel in m_Owner cannot name the shooter. Keep the bot id on
	// the projectile so a victim can retaliate against the actual attacker.
	pProjectile->SetOwnerBot(GetBotID());

	GameWorld()->CreateSound(m_Pos, m_pDef->m_Ranged.m_Sound);
	m_AttackTick = Server()->Tick();
	m_RangedTimer.start(Server()->Tick(), m_pDef->m_Ranged.m_Cooldown);
	return true;
}

bool CMonsterEntity::TryLeap(vec2 Target)
{
	if(!m_pDef->m_Leap.m_Enabled || m_LeapTimer.active(Server()->Tick()))
		return false;
	const float Dist = distance(Target, m_Pos);
	if(Dist < 48.0f || Dist > m_pDef->m_Leap.m_Range)
		return false;
	if((m_Core.m_Jumped & 1) != 0 || m_Core.m_Vel.y < 0.0f)
		return false;

	const float DX = Target.x - m_Pos.x;
	m_Input.m_Direction = (int) sign(DX);
	m_Input.m_Jump = 1;
	m_Core.m_Vel.x = (DX >= 0.0f ? 1.0f : -1.0f) * m_pDef->m_Leap.m_ForceX;
	m_Core.m_Vel.y = m_pDef->m_Leap.m_ForceY;
	m_LeapTimer.start(Server()->Tick(), m_pDef->m_Leap.m_Cooldown);
	return true;
}

void CMonsterEntity::TickAuras()
{
	if(!m_pDef)
		return;

	if(m_pDef->m_HealAura.m_Enabled && m_HealTimer.expired(Server()->Tick()))
	{
		// the old "set the interval, then count it down" period was interval+1
		m_HealTimer.start(Server()->Tick(), m_pDef->m_HealAura.m_Interval + 1);
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			CPlayer *pPlayer = GameServer()->m_apPlayers[i];
			if(!pPlayer || pPlayer->GameWorld() != GameWorld())
				continue;
			CCharacter *pChr = pPlayer->GetCharacter();
			if(!pChr || !pChr->IsAlive() || distance(pChr->GetPos(), m_Pos) > m_pDef->m_HealAura.m_Radius)
				continue;
			pChr->IncreaseHealth(m_pDef->m_HealAura.m_Amount);
		}
	}

	if(m_pDef->m_ArmorAura.m_Enabled && m_ArmorTimer.expired(Server()->Tick()))
	{
		m_ArmorTimer.start(Server()->Tick(), m_pDef->m_ArmorAura.m_Interval + 1);
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			CPlayer *pPlayer = GameServer()->m_apPlayers[i];
			if(!pPlayer || pPlayer->GameWorld() != GameWorld())
				continue;
			CCharacter *pChr = pPlayer->GetCharacter();
			if(!pChr || !pChr->IsAlive() || distance(pChr->GetPos(), m_Pos) > m_pDef->m_ArmorAura.m_Radius)
				continue;
			pChr->IncreaseArmor(m_pDef->m_ArmorAura.m_Amount);
		}
	}
}

void CMonsterEntity::TickEnrage()
{
	if(m_Enraged || m_pDef->m_Enrage.m_HealthPercent <= 0 || m_MaxHealth <= 0)
		return;
	if(m_Health * 100 > m_MaxHealth * m_pDef->m_Enrage.m_HealthPercent)
		return;

	m_Enraged = true;
	m_MeleeDamage += m_pDef->m_Enrage.m_DamageBonus;
	m_Core.m_SpeedScale *= 1.0f + m_pDef->m_Enrage.m_SpeedPercent / 100.0f;
	SayEvent(MOBDIALOGUE_LOW_HEALTH, true);
}

void CMonsterEntity::TrySummon()
{
	if(m_Summoned || !m_pDef || !m_pDef->m_IsBoss || !m_pDef->m_Summon.m_aMob[0] || m_MaxHealth <= 0)
		return;
	if(m_Health * 100 > m_MaxHealth * m_pDef->m_Summon.m_HealthPercent)
		return;
	if(!GameServer()->Mobs() || !GameServer()->BotManager())
		return;

	const SMobDef *pSummon = GameServer()->Mobs()->Find(m_pDef->m_Summon.m_aMob);
	if(!pSummon)
		return;
	m_Summoned = true;
	SayEvent(MOBDIALOGUE_COMBAT, true);
	for(int i = 0; i < m_pDef->m_Summon.m_Count; i++)
	{
		const vec2 Offset((float) ((i % 2) * 2 - 1) * 48.0f, (float) (i / 2) * -32.0f);
		GameServer()->BotManager()->SpawnMob(GameWorld(), pSummon, m_Pos + Offset, nullptr, nullptr, false);
	}
}

void CMonsterEntity::TickDialogue()
{
	if(!m_pLastTarget)
		SayEvent(MOBDIALOGUE_IDLE);
}

void CMonsterEntity::SayEvent(int Event, bool Force)
{
	if(!m_pDef || Event < 0 || Event >= NUM_MOBDIALOGUE_EVENTS)
		return;
	const SDialogueSet &Set = m_pDef->m_Dialogue.m_aEvents[Event];
	if(Set.m_NumLines <= 0)
		return;

	const int Now = Server()->Tick();
	const int Cooldown = s_aDialogueCooldown[Event];
	if(!Force)
	{
		if(Cooldown > 0 && Now - m_aLastEventTick[Event] < Cooldown)
			return;
		// at most one line per 8s even across events, so a fight does not turn
		// into a stream of quips
		if(Now - m_LastSpeakTick < 400)
			return;
	}

	const char *pLine = Set.m_aaLines[random_int() % Set.m_NumLines];

	// The line is localized per receiver, so each client reads it in its own
	// language; a content pack only has to provide the source text. The
	// {player} token is expanded after localization: the translation table is
	// keyed by the source line, which is the form that still carries it.
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		if(!pPlayer)
			continue;
		if(pPlayer->GameWorld() != GameWorld())
			continue;
		if(distance(pPlayer->m_ViewPos, m_Pos) > 900.0f)
			continue;
		CLanguageScope Scope(pPlayer->Language());
		char aMsg[MAX_MOB_DIALOGUE_LENGTH * 2];
		str_copy(aMsg, Localize(pLine, "Mob Dialogue"), sizeof(aMsg));
		ReplaceDialoguePlayerToken(aMsg, sizeof(aMsg), Server()->ClientName(i));
		GameServer()->SendBotChat(this, i, aMsg);
	}

	m_LastSpeakTick = Now;
	m_aLastEventTick[Event] = Now;
}

void CMonsterEntity::RememberAttacker(CEntity *pFrom)
{
	if(!pFrom)
		return;

	// A friendly source must not make a neutral or friendly monster retaliate:
	// CBotEntity::TakeDamage rejects that damage, but the aggro bookkeeping
	// happens before it. Hostile monsters still record it, because they take
	// friendly damage as well.
	int From = -1;
	if(pFrom->ObjFlag() & CGameWorld::ENTFLAG_CHILD)
		From = static_cast<CChildEntity *>(pFrom)->GetOwner();
	else
		From = pFrom->GetHitAttacker();
	if(m_pDef->m_Faction != BOTFACTION_HOSTILE && From == PLAYER_TEAM_FRIENDLY)
		return;

	m_RetaliateUntil = Server()->Tick() + Server()->TickSpeed() * 10;

	if(pFrom->ObjType() == CGameWorld::ENTTYPE_CHARACTER)
	{
		m_LastAttackerClient = static_cast<CCharacter *>(pFrom)->GetCID();
		m_LastAttackerBot = UUID_ZEROED;
	}
	else if(pFrom->ObjType() == CGameWorld::ENTTYPE_BOTENTITY)
	{
		m_LastAttackerBot = static_cast<IBotEntity *>(pFrom)->GetBotID();
		m_LastAttackerClient = -1;
	}
	else if(pFrom->ObjType() == CGameWorld::ENTTYPE_PROJECTILE)
	{
		// A bot's projectile only carries a team sentinel in m_Owner, so the
		// projectile also records the firing bot. Without this a Spitter could
		// shoot another monster from range and never be identified as the
		// attacker; that monster would stay neutral instead of fighting back.
		const CProjectile *pProjectile = static_cast<const CProjectile *>(pFrom);
		if(pProjectile->OwnerBot() != UUID_ZEROED)
		{
			m_LastAttackerBot = pProjectile->OwnerBot();
			m_LastAttackerClient = -1;
		}
		else
		{
			const int Owner = pProjectile->GetOwner();
			if(Owner >= 0)
			{
				m_LastAttackerClient = Owner;
				m_LastAttackerBot = UUID_ZEROED;
			}
		}
	}
	else if(pFrom->ObjFlag() & CGameWorld::ENTFLAG_CHILD)
	{
		const int Owner = static_cast<CChildEntity *>(pFrom)->GetOwner();
		if(Owner >= 0)
		{
			m_LastAttackerClient = Owner;
			m_LastAttackerBot = UUID_ZEROED;
		}
	}
}

bool CMonsterEntity::TakeDamage(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	if(!m_Alive)
		return false;
	RememberAttacker(pFrom);
	return CBotEntity::TakeDamage(Force, Source, Dmg, pFrom, Weapon);
}

bool CMonsterEntity::OnBotDamaged(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	(void) Force;
	(void) Source;
	(void) Weapon;
	if(Dmg > 0)
		SayEvent(MOBDIALOGUE_HURT);
	if(!m_SaidLowHealth && m_MaxHealth > 0 && m_Health * 100 <= m_MaxHealth * 25)
	{
		m_SaidLowHealth = true;
		SayEvent(MOBDIALOGUE_LOW_HEALTH, true);
	}
	(void) pFrom;
	return true;
}

int CMonsterEntity::GetHitAttacker()
{
	// Melee and abilities of a monster are not a player team. The victim must
	// skip the team friendly-fire check or every red player would be immune.
	return m_pDef && m_pDef->m_Faction == BOTFACTION_FRIENDLY ? PLAYER_TEAM_FRIENDLY : PLAYER_TEAM_HOSTILE;
}

void CMonsterEntity::OnBotHit(CEntity *pTarget, bool Killed)
{
	if(Killed && pTarget && pTarget->ObjType() == CGameWorld::ENTTYPE_CHARACTER)
		SayEvent(MOBDIALOGUE_KILL);
}

void CMonsterEntity::DropLoot(CEntity *pKiller)
{
	(void) pKiller;
	for(int i = 0; i < m_pDef->m_lDrops.size(); i++)
	{
		const SMobDrop &Drop = m_pDef->m_lDrops[i];
		if(Drop.m_Chance <= 0.0f)
			continue;
		if(Drop.m_Chance < 1.0f && (float) (random_int() % 10000) / 10000.0f >= Drop.m_Chance)
			continue;
		if(!GameServer()->Item() || !GameServer()->Item()->IsKnownItem(Drop.m_Id))
		{
			dbg_msg("mob", "%s drops unknown item '%s'", m_pDef->m_aId, Drop.m_aItem);
			continue;
		}
		const int Count = Drop.m_CountMin + (Drop.m_CountMax > Drop.m_CountMin ? random_int() % (Drop.m_CountMax - Drop.m_CountMin + 1) : 0);
		if(Count <= 0)
			continue;
		const vec2 Dir((float) (random_int() % 200 - 100) / 100.0f, -1.0f - (float) (random_int() % 100) / 100.0f);
		new CDroppedPickup(GameWorld(), m_Pos + vec2(0.0f, -16.0f), Dir, Drop.m_Id, Count);
	}

	// Money is not an item: roll one bundle and scatter it over several
	// physical piles, each drawn as a different weapon projectile.
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
			const vec2 Offset((float) (random_int() % 24 - 12), -16.0f);
			new CDroppedPickup(GameWorld(), m_Pos + Offset, Dir, (int64) PileAmount, CDroppedPickup::KIND_CURRENCY);
		}
	}
}

void CMonsterEntity::Die(CEntity *pKiller, int Weapon)
{
	if(!m_Alive)
		return;
	SayEvent(MOBDIALOGUE_DEATH, true);
	DropLoot(pKiller);
	if(IsBoss() && m_SpawnGroup != 0)
		GameWorld()->MarkBossDefeated(m_SpawnGroup);
	CBotEntity::Die(pKiller, Weapon);
}

void CMonsterEntity::DespawnSilently()
{
	if(!m_Alive)
		return;
	m_Alive = false;
	if(GameWorld()->BotManager())
		GameWorld()->BotManager()->OnBotDeath(m_BotID);
	GameWorld()->DestroyEntity(this);
}
