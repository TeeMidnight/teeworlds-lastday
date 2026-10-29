/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
/* Bot layer ported from teeworlds-carbon (c) TeeMidnight, zlib license.                     */
#include <game/localization.h>
#include <game/localization_format.h>
#include <game/server/botmanager.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/npcdata.h>
#include <game/server/player.h>

#include "npcentity.h"

// How far (in tiles) an NPC may wander away from its spawn point. Outside this
// radius it drops whatever it was doing and walks back.
static const int NPC_ROAM_RADIUS_TILES = 5;
static const float NPC_ROAM_RADIUS = NPC_ROAM_RADIUS_TILES * 32.0f;

// a hated player may lure the NPC this many tiles beyond its normal circle,
// but the NPC never pursues further than the combined radius
static const int NPC_CHASE_BONUS_TILES = 5;
static const float NPC_CHASE_RADIUS = (NPC_ROAM_RADIUS_TILES + NPC_CHASE_BONUS_TILES) * 32.0f;

// Whether a solid wall sits right in front of the NPC at body height. This is
// the only reason a wandering NPC jumps: it walks into a step or a wall.
static bool WallAhead(CGameWorld *pWorld, vec2 Pos, int Dir, float HalfSize)
{
	if(Dir == 0)
		return false;
	const float X = Pos.x + Dir * (HalfSize + 4.0f);
	return pWorld->Collision()->CheckPoint(X, Pos.y) ||
	       pWorld->Collision()->CheckPoint(X, Pos.y - HalfSize * 0.5f) ||
	       pWorld->Collision()->CheckPoint(X, Pos.y + HalfSize * 0.5f);
}

// Whether the NPC is standing on solid ground.
static bool OnGround(CGameWorld *pWorld, vec2 Pos, float HalfSize)
{
	return pWorld->Collision()->CheckPoint(Pos.x, Pos.y + HalfSize + 2.0f) ||
	       pWorld->Collision()->CheckPoint(Pos.x, Pos.y + HalfSize + 6.0f);
}

// The identity the client is shown, taken straight from the definition.
static CBotTeeInfo NpcTeeInfoFromDef(const SNpcDef *pDef)
{
	CBotTeeInfo Info;
	mem_zero(&Info, sizeof(Info));
	Info.m_Skin = pDef->m_Skin;
	str_copy(Info.m_aName, NpcIDDisplayName(pDef->m_Id), sizeof(Info.m_aName));
	return Info;
}

CNpcEntity::CNpcEntity(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const CBotTeeInfo &TeeInfo, int Team) :
	CBotEntity(pWorld, Pos, BotID, TeeInfo, Team, BOTFACTION_FRIENDLY)
{
	m_pDef = nullptr;
	m_HealthRegen = 0;
	m_RegenCounter = 0;
	m_SpawnPos = Pos;
	m_WanderDir = random_int() % 2 ? 1 : -1;
	m_CursorTarget = vec2(100.0f * m_WanderDir, 0.0f);
	// start walking right away instead of waiting for the first random turn
	m_Input.m_Direction = m_WanderDir;
}

CNpcEntity::CNpcEntity(CGameWorld *pWorld, const SNpcDef *pDef, int Team) :
	CBotEntity(pWorld, pDef->m_Pos, pDef->m_Id.Value(), NpcTeeInfoFromDef(pDef), Team, pDef->m_Faction)
{
	m_pDef = pDef;
	m_MaxHealth = pDef->m_Health;
	m_Health = m_MaxHealth;
	m_HealthRegen = pDef->m_HealthRegen;
	m_RegenCounter = 0;

	m_SpawnPos = pDef->m_Pos;
	m_WanderDir = random_int() % 2 ? 1 : -1;
	m_CursorTarget = vec2(100.0f * m_WanderDir, 0.0f);
	m_Input.m_Direction = m_WanderDir;

	// The spawn line is a chat from the bot's own tee id, and that id only
	// exists after the manager has assigned it and a snapshot has told the
	// client the name. Announce a couple of ticks later for that handshake.
	m_SpawnAnnounceTick = Server()->Tick() + 2;
}

void CNpcEntity::OnBotTick()
{
	if(m_SpawnAnnounceTick >= 0 && Server()->Tick() >= m_SpawnAnnounceTick)
	{
		SayEvent(MOBDIALOGUE_SPAWN, true);
		m_SpawnAnnounceTick = -1;
	}
	CBotEntity::OnBotTick();
}

void CNpcEntity::Tick()
{
	CBotEntity::Tick();

	TickInteract();
	TickAuras();
	TickAggroDecay();

	// the scripted respawn drives itself once started
	if(m_State == NPCSTATE_TELEPORTING)
		TickRespawn();

	// health regenerates over time; the rate is hit points per second, which is
	// accumulated so a fractional amount per tick still adds up exactly
	if(!m_Alive || m_HealthRegen <= 0)
	{
		m_RegenCounter = 0;
		return;
	}

	m_RegenCounter += m_HealthRegen;
	const int PerSecond = Server()->TickSpeed();
	while(m_RegenCounter >= PerSecond)
	{
		m_RegenCounter -= PerSecond;
		if(!IncreaseHealth(1))
			break;
	}
}

int CNpcEntity::HitClientID(CEntity *pFrom)
{
	if(!pFrom)
		return -1;
	// a projectile or explosion reports the client that fired it
	if(pFrom->ObjFlag() & CGameWorld::ENTFLAG_CHILD)
		return static_cast<CChildEntity *>(pFrom)->GetOwner();
	if(pFrom->ObjType() == CGameWorld::ENTTYPE_CHARACTER)
		return static_cast<CCharacter *>(pFrom)->GetCID();
	return -1;
}

void CNpcEntity::AddAggro(int ClientID, int Delta)
{
	if(!m_pDef || Delta == 0 || ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;
	// every client, logged in or not, keeps a session aggro so a guest can
	// still provoke a neutral NPC
	m_aSessionAggro[ClientID] = maximum(0, m_aSessionAggro[ClientID] + Delta);

	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientID];
	if(!pPlayer || pPlayer->m_AccountUuid == UUID_ZEROED)
		return;
	GameServer()->NpcAddAggro(m_pDef->m_Id.Value(), pPlayer->m_AccountUuid, Delta);
	UpdateAggroCache(pPlayer->m_AccountUuid, Delta);
}

void CNpcEntity::ShowHealth(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;
	const char *pName = m_pDef ? NpcIDDisplayName(m_pDef->m_Id) : m_TeeInfo.m_aName;
	char aBuf[160];
	str_format(aBuf, sizeof(aBuf), "%s: %d/%d", pName, maximum(0, m_Health), m_MaxHealth);
	GameServer()->SendChat(-1, CHAT_WHISPER, ClientID, aBuf);
}

int CNpcEntity::CachedAggro(int ClientID)
{
	if(!m_pDef || ClientID < 0 || ClientID >= MAX_CLIENTS)
		return 0;
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientID];
	if(!pPlayer || pPlayer->m_AccountUuid == UUID_ZEROED)
		return m_aSessionAggro[ClientID];

	SNpcAggroEntry *pEntry = nullptr;
	for(int i = 0; i < m_lAggro.size(); i++)
		if(m_lAggro[i].m_Account == pPlayer->m_AccountUuid)
			pEntry = &m_lAggro[i];

	if(!pEntry)
	{
		pEntry = &m_lAggro.emplace();
		pEntry->m_Account = pPlayer->m_AccountUuid;
		pEntry->m_Aggro = 0;
		pEntry->m_RefreshTick = -1;
	}

	// the ledger is the truth, but it is only re-read once a second per player
	if(pEntry->m_RefreshTick < 0 || Server()->Tick() - pEntry->m_RefreshTick >= Server()->TickSpeed())
	{
		pEntry->m_Aggro = GameServer()->NpcGetAggro(m_pDef->m_Id.Value(), pPlayer->m_AccountUuid);
		pEntry->m_RefreshTick = Server()->Tick();
	}
	return pEntry->m_Aggro;
}

void CNpcEntity::UpdateAggroCache(const Uuid &Account, int Delta)
{
	for(int i = 0; i < m_lAggro.size(); i++)
		if(m_lAggro[i].m_Account == Account)
		{
			m_lAggro[i].m_Aggro += Delta;
			return;
		}

	SNpcAggroEntry &Entry = m_lAggro.emplace();
	Entry.m_Account = Account;
	Entry.m_Aggro = Delta;
	Entry.m_RefreshTick = -1;
}

int CNpcEntity::FindHatedPlayer()
{
	if(!m_pDef || m_pDef->m_Faction == BOTFACTION_FRIENDLY || m_pDef->m_Aggro.m_Threshold <= 0)
		return -1;

	int Best = -1;
	float BestDist = NPC_CHASE_RADIUS;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		if(!pPlayer || pPlayer->GameWorld() != GameWorld())
			continue;
		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;

		// stale ledger aggro must not turn a neutral NPC into a guard that
		// attacks every visitor; only a recent hit justifies retaliation
		if(Server()->Tick() - m_aLastPlayerHitTick[i] > Server()->TickSpeed() * 30)
			continue;

		// only a player the NPC could actually reach is a target
		const float Dist = distance(pChr->GetPos(), m_SpawnPos);
		if(Dist > BestDist)
			continue;
		if(CachedAggro(i) <= m_pDef->m_Aggro.m_Threshold)
			continue;

		BestDist = Dist;
		Best = i;
	}
	return Best;
}

void CNpcEntity::Chase(int ClientID)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientID];
	CCharacter *pChr = pPlayer ? pPlayer->GetCharacter() : nullptr;
	if(!pChr || !pChr->IsAlive())
		return;

	ChaseEntity(pChr);
}

void CNpcEntity::ChaseEntity(CEntity *pTarget)
{
	if(!pTarget)
		return;

	const vec2 Target = pTarget->GetPos();

	// never pursue past the chase radius: turn back instead
	if(distance(m_Pos, m_SpawnPos) > NPC_CHASE_RADIUS)
	{
		m_Input.m_Direction = m_SpawnPos.x < m_Pos.x ? -1 : (m_SpawnPos.x > m_Pos.x ? 1 : 0);
		m_Input.m_Jump = m_SpawnPos.y < m_Pos.y - CCharacterCore::PHYS_SIZE ? 1 : 0;
		if(m_SpawnPos.x != m_Pos.x)
			m_CursorTarget = vec2(length(m_CursorTarget) * m_Input.m_Direction, 0.0f);
		return;
	}

	const float dx = Target.x - m_Pos.x;
	if(absolute(dx) > 24.0f)
		m_Input.m_Direction = (int) sign(dx);
	else
		m_Input.m_Direction = 0;

	if(Target.y - m_Pos.y < -48.0f && !(m_Core.m_Jumped & 1) && (m_Core.m_Vel.y > -1.25f))
		m_Input.m_Jump = 1;

	m_CursorTarget = Target - m_Pos;
	if(distance(Target, m_Pos) < 48.0f)
		m_Input.m_Fire = 1;
}

CEntity *CNpcEntity::FindHostileBot()
{
	// A neutral NPC is not automatically at war with every monster: it only
	// engages hostile monsters when its definition says it defends the area
	// (the blacksmith does, the medic does not), or when that exact monster
	// attacked it. This keeps town NPCs from clearing the whole floor on sight.
	if(!m_pDef || m_pDef->m_Faction == BOTFACTION_FRIENDLY)
		return nullptr;

	const int Now = Server()->Tick();
	float BestDist = NPC_CHASE_RADIUS;
	CEntity *pBest = nullptr;

	array<CEntity *> lpEnts;
	lpEnts.hint_size(8);
	const int Num = GameWorld()->FindFlagEntities(m_Pos, NPC_CHASE_RADIUS, lpEnts, CGameWorld::ENTFLAG_HITABLE);
	for(int i = 0; i < Num; i++)
	{
		CEntity *pEnt = lpEnts[i];
		if(pEnt == this || pEnt->ObjType() != CGameWorld::ENTTYPE_BOTENTITY)
			continue;

		IBotEntity *pBot = static_cast<IBotEntity *>(pEnt);
		if(!pBot->IsAlive() || pBot->GetBotFaction() != BOTFACTION_HOSTILE)
			continue;

		const bool bRetaliating = m_RetaliateUntil > Now && m_LastAttackerBot != UUID_ZEROED && pBot->GetBotID() == m_LastAttackerBot;
		if(!m_pDef->m_AttackHostiles && !bRetaliating)
			continue;

		// only a bot the NPC could actually reach and see is a target
		if(GameWorld()->Collision()->IntersectLine(m_Pos, GameWorld()->PosInFrame(pEnt), NULL, NULL))
			continue;

		const float Dist = distance(GameWorld()->PosInFrame(pEnt), m_SpawnPos);
		if(Dist > BestDist)
			continue;

		BestDist = Dist;
		pBest = pEnt;
	}
	return pBest;
}

bool CNpcEntity::OnBotDamaged(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	(void) Force;
	(void) Source;
	(void) Dmg;
	(void) Weapon;

	// a player landing a hit sees the remaining health and adds per-hit aggro
	const int ClientID = HitClientID(pFrom);
	if(ClientID >= 0)
	{
		m_aLastPlayerHitTick[ClientID] = Server()->Tick();
		ShowHealth(ClientID);
		if(m_pDef && m_pDef->m_Faction == BOTFACTION_NEUTRAL)
			AddAggro(ClientID, m_pDef->m_Aggro.m_PerHit);
	}
	RememberBotAttacker(pFrom);
	if(Dmg > 0)
		SayEvent(MOBDIALOGUE_HURT);
	return true;
}

void CNpcEntity::OnBotHit(CEntity *pTarget, bool Killed)
{
	if(Killed)
		SayEvent(MOBDIALOGUE_KILL);

	// the NPC killing a player decays the aggro it holds for that player
	if(!Killed || !m_pDef || !pTarget || pTarget->ObjType() != CGameWorld::ENTTYPE_CHARACTER)
		return;
	CCharacter *pChr = static_cast<CCharacter *>(pTarget);
	AddAggro(pChr->GetCID(), -m_pDef->m_Aggro.m_KillDecay);
}

void CNpcEntity::Die(CEntity *pKiller, int Weapon)
{
	if(!m_Alive || m_Invincible)
		return;

	SayEvent(MOBDIALOGUE_DEATH, true);

	// a player that lands the killing blow gains per-kill aggro and sees the
	// final health; the world or any non-player source adds nothing
	const int ClientID = HitClientID(pKiller);
	if(ClientID >= 0)
	{
		ShowHealth(ClientID);
		if(m_pDef)
			AddAggro(ClientID, m_pDef->m_Aggro.m_PerKill);
	}

	// configured NPCs do not simply vanish: they respawn through the main world
	StartRespawn();
}

bool CNpcEntity::TakeDamage(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	// during the scripted respawn the NPC cannot be damaged at all
	if(m_Invincible)
		return false;

	// NPCs are immovable: a hit lands and damages, but its knockback impulse is
	// dropped so a player cannot shove the NPC around.
	(void) Force;
	RememberBotAttacker(pFrom);
	return CBotEntity::TakeDamage(vec2(0.0f, 0.0f), Source, Dmg, pFrom, Weapon);
}

CEntity *CNpcEntity::ClosestPlayer()
{
	CEntity *pClosest = nullptr;
	float ClosestDist = NPC_ROAM_RADIUS;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		if(!pPlayer || pPlayer->GameWorld() != GameWorld())
			continue;
		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;

		const float Dist = distance(pChr->GetPos(), m_Pos);
		if(Dist <= ClosestDist)
		{
			ClosestDist = Dist;
			pClosest = pChr;
		}
	}
	return pClosest;
}

void CNpcEntity::StartRespawn()
{
	// the NPC is not gone: it keeps its identity, gets its health back and is
	// untouchable for the duration of the teleport
	m_Alive = true;
	m_Health = m_MaxHealth;
	m_Invincible = true;
	m_State = NPCSTATE_TELEPORTING;
	m_StateTick = Server()->Tick();
	m_Emoted = false;
	m_HomeWorldID = GameWorld()->WorldID();
	m_HomePos = m_SpawnPos;

	// respawn in the main world (Connector), like a player does
	CGameWorld *pMain = GameServer()->MainWorld();
	if(!pMain)
		return;

	vec2 SpawnPos = m_Pos;
	if(!GameServer()->m_pController->CanSpawn(pMain, TEAM_BLUE, &SpawnPos))
		SpawnPos = m_Pos;

	if(pMain == GameWorld())
	{
		// already in the main world: a plain teleport to a spawn point
		m_Core.m_Pos = SpawnPos;
		m_Core.m_Vel = vec2(0.0f, 0.0f);
		m_Pos = SpawnPos;
		GameWorld()->CreatePlayerSpawn(SpawnPos);
	}
	else
		MoveToWorld(pMain, SpawnPos);
}

void CNpcEntity::TickRespawn()
{
	const int HalfSecond = maximum(1, Server()->TickSpeed() / 2);
	const int Elapsed = Server()->Tick() - m_StateTick;

	// 0.5s after the death: an angry emoticon
	if(!m_Emoted && Elapsed >= HalfSecond)
	{
		m_Emoted = true;
		SetEmote(EMOTE_ANGRY, Server()->Tick() + HalfSecond);
		if(GameWorld()->HasBotManager())
			GameWorld()->GetBotManager()->SendBotEmoticon(m_BotID, EMOTE_ANGRY);
	}

	// another 0.5s: spawn smoke at both ends of the teleport and go home
	if(Elapsed >= 2 * HalfSecond)
	{
		GameWorld()->CreatePlayerSpawn(m_Pos);

		CGameWorld *pHome = GameServer()->WorldByID(m_HomeWorldID);
		if(pHome)
		{
			if(pHome != GameWorld())
				MoveToWorld(pHome, m_HomePos);
			else
			{
				m_Core.m_Pos = m_HomePos;
				m_Core.m_Vel = vec2(0.0f, 0.0f);
				m_Pos = m_HomePos;
			}
			pHome->CreatePlayerSpawn(m_HomePos);
		}

		m_Invincible = false;
		m_State = NPCSTATE_ALIVE;
		SetEmote(EMOTE_NORMAL, -1);
	}
}

void CNpcEntity::TickInteract()
{
	if(!m_pDef)
		return;

	bool AnyoneWasInRange = false;
	bool AnyoneInRange = false;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		CCharacter *pChr = pPlayer ? pPlayer->GetCharacter() : nullptr;
		const bool InRange = pPlayer && pPlayer->GameWorld() == GameWorld() &&
				     pChr && pChr->IsAlive() && distance(pChr->GetPos(), m_Pos) <= NPC_INTERACT_RADIUS;

		// tell the player once, when they first step into range
		if(InRange && !m_aClientInRange[i])
		{
			CLanguageScope LanguageScope(GameServer()->LanguageOf(i));
			const std::string Msg = LocalizeFormat("NPC", "You unlocked the shop of {}.", NpcIDDisplayName(m_pDef->m_Id));
			GameServer()->SendChat(-1, CHAT_WHISPER, i, Msg.c_str());
		}
		AnyoneWasInRange = AnyoneWasInRange || m_aClientInRange[i];
		AnyoneInRange = AnyoneInRange || InRange;
		m_aClientInRange[i] = InRange;
	}

	// Greet the first visitor after the shop was empty, not every player who
	// walks up while someone is already standing there. The dialogue cooldown
	// keeps even that greeting occasional.
	if(!AnyoneWasInRange && AnyoneInRange)
		SayEvent(MOBDIALOGUE_SIGHT);
}

void CNpcEntity::TickAggroDecay()
{
	if(!m_pDef || m_pDef->m_Aggro.m_DecaySeconds <= 0 || m_pDef->m_Aggro.m_DecayAmount <= 0)
		return;
	const int Interval = m_pDef->m_Aggro.m_DecaySeconds * Server()->TickSpeed();
	if(Server()->Tick() - m_LastAggroDecayTick < Interval)
		return;
	m_LastAggroDecayTick = Server()->Tick();

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		if(!pPlayer || pPlayer->GameWorld() != GameWorld())
			continue;
		const int Aggro = CachedAggro(i);
		if(Aggro <= 0)
			continue;
		AddAggro(i, -minimum(Aggro, m_pDef->m_Aggro.m_DecayAmount));
	}
}

void CNpcEntity::RememberBotAttacker(CEntity *pFrom)
{
	if(!pFrom || pFrom->ObjType() != CGameWorld::ENTTYPE_BOTENTITY)
		return;
	IBotEntity *pBot = static_cast<IBotEntity *>(pFrom);
	if(!pBot->IsAlive() || pBot->GetBotFaction() != BOTFACTION_HOSTILE)
		return;
	m_LastAttackerBot = pBot->GetBotID();
	m_RetaliateUntil = Server()->Tick() + Server()->TickSpeed() * 10;
}

void CNpcEntity::TickAuras()
{
	if(!m_pDef || !m_Alive)
		return;

	if(m_pDef->m_HealAura.m_Enabled)
	{
		if(m_HealTimer > 0)
			m_HealTimer--;
		else
		{
			m_HealTimer = m_pDef->m_HealAura.m_Interval;
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
	}

	if(m_pDef->m_ArmorAura.m_Enabled)
	{
		if(m_ArmorTimer > 0)
			m_ArmorTimer--;
		else
		{
			m_ArmorTimer = m_pDef->m_ArmorAura.m_Interval;
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
}

void CNpcEntity::SayEvent(int Event, bool Force)
{
	if(!m_pDef || Event < 0 || Event >= NUM_MOBDIALOGUE_EVENTS)
		return;
	const SDialogueSet &Set = m_pDef->m_Dialogue.m_aEvents[Event];
	if(Set.m_NumLines <= 0)
		return;

	const int Now = Server()->Tick();
	// Town NPCs are ambient, not narrators. They stay much quieter than a
	// monster's combat callouts: one greeting per 45s and at most one line per
	// 8s even across events.
	static const int s_aCooldown[NUM_MOBDIALOGUE_EVENTS] = {
		0, // spawn
		2250, // sight, 45s at 50Hz
		600, // combat, 12s
		600, // hurt, 12s
		900, // kill, 18s
		0, // death
		2400, // idle, 48s
		1200, // low health, 24s
	};
	if(!Force)
	{
		if(s_aCooldown[Event] > 0 && Now - m_aLastEventTick[Event] < s_aCooldown[Event])
			return;
		if(Now - m_LastSpeakTick < 400)
			return;
	}

	const char *pLine = Set.m_aaLines[random_int() % Set.m_NumLines];
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		if(!pPlayer || pPlayer->GameWorld() != GameWorld())
			continue;
		if(distance(pPlayer->m_ViewPos, m_Pos) > 700.0f)
			continue;
		CLanguageScope LanguageScope(pPlayer->Language());
		// Translate before substituting: the translation table is keyed by the
		// source line, which still carries {player}. The chat is sent as the
		// NPC's own tee id so the client shows its name and skin.
		char aMsg[MAX_MOB_DIALOGUE_LENGTH * 2];
		str_copy(aMsg, Localize(pLine, "Mob Dialogue"), sizeof(aMsg));
		ReplaceDialoguePlayerToken(aMsg, sizeof(aMsg), Server()->ClientName(i));
		GameServer()->SendBotChat(this, i, aMsg);
	}

	m_LastSpeakTick = Now;
	m_aLastEventTick[Event] = Now;
}

void CNpcEntity::OnAction()
{
	// A quiet NPC is the rule, not the exception: it creeps while it wanders
	// or watches, and only drops the silent step to run down a target.
	m_Core.m_SilentStep = true;

	// the AI is suspended while the NPC is teleporting home
	if(m_State != NPCSTATE_ALIVE)
	{
		m_Input.m_Direction = 0;
		m_Input.m_Jump = 0;
		return;
	}

	// A player over the aggro threshold is the top priority: the NPC chases and
	// attacks them, out to the chase radius.
	const int Hated = FindHatedPlayer();
	if(Hated >= 0)
	{
		m_Core.m_SilentStep = false;
		Chase(Hated);
		AimCursor();
		return;
	}

	// Hostile bots are attacked on sight: the NPC drops its silent step to
	// charge them just like it does for a hated player.
	CEntity *pHostile = FindHostileBot();
	if(pHostile)
	{
		m_Core.m_SilentStep = false;
		ChaseEntity(pHostile);
		AimCursor();
		return;
	}

	CEntity *pPlayer = ClosestPlayer();
	if(pPlayer)
	{
		// A visitor is close: stop moving and turn to look at them. The NPC
		// never follows or attacks, it just watches until they leave.
		m_Input.m_Direction = 0;
		m_Input.m_Jump = 0;
		m_CursorTarget = pPlayer->GetPos() - m_Pos;
		AimCursor();
		return;
	}

	Wander();
	AimCursor();
}

void CNpcEntity::Wander()
{
	const float HalfSize = GetProximityRadius() * 0.5f;
	bool ShouldJump = false;

	// Outside the spawn radius: head back before anything else. The direction
	// is forced so it cannot keep wandering further out.
	if(distance(m_Pos, m_SpawnPos) > NPC_ROAM_RADIUS)
	{
		const float dx = m_SpawnPos.x - m_Pos.x;
		if(dx < -1.0f)
			m_WanderDir = -1;
		else if(dx > 1.0f)
			m_WanderDir = 1;
		else
			m_WanderDir = random_int() % 2 ? 1 : -1;
		m_Input.m_Direction = m_WanderDir;

		// the spawn is above, or a wall stands in the way: a jump is warranted
		if(m_SpawnPos.y < m_Pos.y - CCharacterCore::PHYS_SIZE || WallAhead(GameWorld(), m_Pos, m_WanderDir, HalfSize))
			ShouldJump = true;
	}
	else
	{
		// Inside the radius: change direction once in a while and keep walking;
		// the previous direction otherwise persists.
		if(random_int() % 1000 < 55)
		{
			m_WanderDir = random_int() % 3 - 1;
			m_Input.m_Direction = m_WanderDir;
		}

		// no aimless hopping: only a wall actually blocking the way is worth one
		if(WallAhead(GameWorld(), m_Pos, m_Input.m_Direction, HalfSize))
			ShouldJump = true;
	}

	// the NPC looks where it walks, so body and head agree
	if(m_Input.m_Direction != 0)
		m_CursorTarget = vec2(100.0f * m_Input.m_Direction, 0.0f);

	if(ShouldJump && OnGround(GameWorld(), m_Pos, HalfSize) && !(m_Core.m_Jumped & 1))
		m_Input.m_Jump = 1;
}
