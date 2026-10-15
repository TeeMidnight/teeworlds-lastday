/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
/* Bot layer ported from teeworlds-carbon (c) TeeMidnight, zlib license.                     */
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/npcdata.h>
#include <game/server/player.h>

#include "npcentity.h"

// How far (in tiles) an NPC may wander away from its spawn point. Outside this
// radius it drops whatever it was doing and walks back.
static const int NPC_ROAM_RADIUS_TILES = 5;
static const float NPC_ROAM_RADIUS = NPC_ROAM_RADIUS_TILES * 32.0f;

// The identity the client is shown, taken straight from the definition.
static CBotTeeInfo NpcTeeInfoFromDef(const SNpcDef *pDef)
{
	CBotTeeInfo Info;
	mem_zero(&Info, sizeof(Info));
	Info.m_Skin = pDef->m_Skin;
	str_copy(Info.m_aName, pDef->m_aName, sizeof(Info.m_aName));
	return Info;
}

CNpcEntity::CNpcEntity(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const CBotTeeInfo &TeeInfo, int Team) :
	CBotEntity(pWorld, Pos, BotID, TeeInfo, Team)
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
	CBotEntity(pWorld, pDef->m_Pos, pDef->m_BotId, NpcTeeInfoFromDef(pDef), Team)
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
}

void CNpcEntity::Tick()
{
	CBotEntity::Tick();

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
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientID];
	if(!pPlayer || !pPlayer->m_LoggedIn || pPlayer->m_AccountUuid == UUID_ZEROED)
		return;
	GameServer()->NpcAddAggro(m_pDef->m_BotId, pPlayer->m_AccountUuid, Delta);
}

void CNpcEntity::ShowHealth(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;
	const char *pName = m_pDef ? m_pDef->m_aName : m_TeeInfo.m_aName;
	char aBuf[160];
	str_format(aBuf, sizeof(aBuf), "%s: %d/%d", pName, maximum(0, m_Health), m_MaxHealth);
	GameServer()->SendChat(-1, CHAT_WHISPER, ClientID, aBuf);
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
		ShowHealth(ClientID);
		if(m_pDef)
			AddAggro(ClientID, m_pDef->m_Aggro.m_PerHit);
	}
	return true;
}

void CNpcEntity::OnBotHit(CEntity *pTarget, bool Killed)
{
	// the NPC killing a player decays the aggro it holds for that player
	if(!Killed || !m_pDef || !pTarget || pTarget->ObjType() != CGameWorld::ENTTYPE_CHARACTER)
		return;
	CCharacter *pChr = static_cast<CCharacter *>(pTarget);
	AddAggro(pChr->GetCID(), -m_pDef->m_Aggro.m_KillDecay);
}

void CNpcEntity::Die(CEntity *pKiller, int Weapon)
{
	if(!m_Alive)
		return;

	// a player that lands the killing blow gains per-kill aggro and sees the
	// final health; the world or any non-player source adds nothing
	const int ClientID = HitClientID(pKiller);
	if(ClientID >= 0)
	{
		ShowHealth(ClientID);
		if(m_pDef)
			AddAggro(ClientID, m_pDef->m_Aggro.m_PerKill);
	}

	CBotEntity::Die(pKiller, Weapon);
}

bool CNpcEntity::TakeDamage(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	// NPCs are immovable: a hit lands and damages, but its knockback impulse is
	// dropped so a player cannot shove the NPC around.
	(void) Force;
	return CBotEntity::TakeDamage(vec2(0.0f, 0.0f), Source, Dmg, pFrom, Weapon);
}

CEntity *CNpcEntity::ClosestPlayer()
{
	CEntity *pClosest = nullptr;
	float ClosestDist = NPC_ROAM_RADIUS;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		if(!pPlayer)
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

void CNpcEntity::OnAction()
{
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
	// occasionally hop, so a small ledge does not trap it
	if(random_int() % 1000 < 25)
		m_Input.m_Jump = 1;

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
		// climb back if the spawn point is above us
		if(m_SpawnPos.y < m_Pos.y - CCharacterCore::PHYS_SIZE)
			m_Input.m_Jump = 1;
		return;
	}

	// Inside the radius: change direction once in a while and keep walking; the
	// previous direction otherwise persists, exactly like the default wander.
	if(random_int() % 1000 < 55)
	{
		m_WanderDir = random_int() % 3 - 1;
		m_Input.m_Direction = m_WanderDir;
		if(m_WanderDir)
			m_CursorTarget = vec2(length(m_CursorTarget) * m_WanderDir, 0.0f);
	}
}
