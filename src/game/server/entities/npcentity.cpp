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
