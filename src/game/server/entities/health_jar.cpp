/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/entities/health_jar.h>

#include <base/math.h>
#include <base/system.h>

#include <generated/protocol.h>

#include <game/server/entities/character.h>
#include <game/server/entities/dropped_pickup.h>
#include <game/server/entities/health_drop.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>

CHealthJarEntity::CHealthJarEntity(CGameWorld *pGameWorld, vec2 Pos, const CItemID &ItemId, int Cooldown, int Range, int Health, const Uuid &OwnerAccount, int OwnerClient, const char *pOwnerName) :
	CHitableEntity(pGameWorld, CGameWorld::ENTTYPE_HEALTH_JAR, 0, Pos, JAR_WIDTH / 2)
{
	m_ItemId = ItemId;
	m_OwnerAccount = OwnerAccount;
	m_OwnerClient = OwnerClient;
	str_copy(m_aOwnerName, pOwnerName ? pOwnerName : "", sizeof(m_aOwnerName));
	m_CooldownTicks = maximum(1, Cooldown) * Server()->TickSpeed();
	m_Range = maximum(32, Range);
	m_Health = maximum(1, Health);
	// a fresh jar is ready immediately, so placing one next to yourself pays off
	m_NextDropTick = Server()->Tick();

	for(int i = 0; i < NUM_BEAMS; i++)
		m_aBeamIDs[i] = Server()->SnapNewID();

	GameWorld()->InsertEntity(this);
}

CHealthJarEntity::~CHealthJarEntity()
{
	for(int i = 0; i < NUM_BEAMS; i++)
		Server()->SnapFreeID(m_aBeamIDs[i]);
}

bool CHealthJarEntity::IsOwner(const CPlayer *pPlayer) const
{
	if(!pPlayer || pPlayer->IsDummy())
		return false;

	// an account is stable across reconnects and client slot reuse; only fall
	// back to the slot when the jar was placed by someone who was not logged in
	if(m_OwnerAccount != UUID_ZEROED)
		return pPlayer->m_LoggedIn && pPlayer->m_AccountUuid == m_OwnerAccount;
	return m_OwnerClient >= 0 && pPlayer->GetCID() == m_OwnerClient;
}

bool CHealthJarEntity::OwnerNear()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		if(!pPlayer || !IsOwner(pPlayer))
			continue;
		// an owner in another ring instance is not standing next to this jar
		if(pPlayer->GameWorld() != GameWorld())
			continue;

		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;

		if(distance(pChr->GetPos(), m_Pos) <= (float) m_Range)
			return true;
	}
	return false;
}

bool CHealthJarEntity::DropNearby()
{
	array<CEntity *> lEntities;
	const int Num = GameWorld()->FindEntities(m_Pos, (float) m_Range, lEntities, CGameWorld::ENTTYPE_HEALTH_DROP);
	return Num > 0;
}

void CHealthJarEntity::Tick()
{
	const int Now = Server()->Tick();
	if(Now < m_NextDropTick)
		return;

	// the cooldown is measured in seconds, so ten owner checks per second are
	// plenty and a base full of jars stays cheap. The id staggers the checks so
	// two jars never scan the client list on the same tick.
	if((Now + GetID()) % 10 != 0)
		return;

	// one unclaimed drop per jar: leaving the old one on the floor must not
	// stack pickups on top of each other
	if(DropNearby() || !OwnerNear())
		return;

	const vec2 Jitter((float) (random_int() % 17) - 8.0f, 0.0f);
	new CHealthDropEntity(GameWorld(), m_Pos + Jitter, vec2((float) (random_int() % 3) - 1.0f, 1.0f));
	m_NextDropTick = Now + m_CooldownTicks;
}

void CHealthJarEntity::TickPaused()
{
	// keep the cooldown on the same wall clock as the rest of the game
	++m_NextDropTick;
}

void CHealthJarEntity::SnapBeam(int ID, vec2 From, vec2 To, int SnappingClient)
{
	if(NetworkClippedLine(SnappingClient, From, To))
		return;

	CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, ID, sizeof(CNetObj_Laser)));
	if(!pObj)
		return;

	pObj->m_X = round_to_int(To.x);
	pObj->m_Y = round_to_int(To.y);
	pObj->m_FromX = round_to_int(From.x);
	pObj->m_FromY = round_to_int(From.y);
	// a fresh start tick keeps the beam at full brightness instead of fading
	pObj->m_StartTick = Server()->Tick();
}

void CHealthJarEntity::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	// the health pickup held in the middle of the jar. It is a snap only, so
	// it cannot be collected twice and needs no respawn bookkeeping.
	CNetObj_Pickup *pP = static_cast<CNetObj_Pickup *>(Server()->SnapNewItem(NETOBJTYPE_PICKUP, GetID(), sizeof(CNetObj_Pickup)));
	if(pP)
	{
		pP->m_X = round_to_int(m_Pos.x);
		pP->m_Y = round_to_int(m_Pos.y);
		pP->m_Type = PICKUP_HEALTH;
	}

	// a jar: closed bottom, straight walls, sloped shoulders and a narrow lid
	const float HalfW = (float) JAR_WIDTH / 2.0f;
	const float HalfH = (float) JAR_HEIGHT / 2.0f;
	const float ShoulderY = -24.0f;
	const float NeckW = 14.0f;

	SnapBeam(m_aBeamIDs[0], m_Pos + vec2(-HalfW, HalfH), m_Pos + vec2(HalfW, HalfH), SnappingClient);
	SnapBeam(m_aBeamIDs[1], m_Pos + vec2(-HalfW, HalfH), m_Pos + vec2(-HalfW, ShoulderY), SnappingClient);
	SnapBeam(m_aBeamIDs[2], m_Pos + vec2(HalfW, HalfH), m_Pos + vec2(HalfW, ShoulderY), SnappingClient);
	SnapBeam(m_aBeamIDs[3], m_Pos + vec2(-HalfW, ShoulderY), m_Pos + vec2(-NeckW, -HalfH), SnappingClient);
	SnapBeam(m_aBeamIDs[4], m_Pos + vec2(HalfW, ShoulderY), m_Pos + vec2(NeckW, -HalfH), SnappingClient);
	SnapBeam(m_aBeamIDs[5], m_Pos + vec2(-NeckW, -HalfH), m_Pos + vec2(NeckW, -HalfH), SnappingClient);
}

bool CHealthJarEntity::TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	(void) Force;
	(void) Source;
	(void) pFrom;
	(void) Weapon;

	m_Health -= maximum(1, Dmg);
	if(m_Health > 0)
	{
		GameWorld()->CreateHammerHit(m_Pos);
		return true;
	}

	// dismantle: the jar item falls out where it stood, and any drop already on
	// the floor stays there for whoever reaches it first
	if(!m_ItemId.IsZero())
		new CDroppedPickup(GameWorld(), m_Pos + vec2(0.0f, -16.0f), vec2(0.0f, 0.0f), m_ItemId, 1);

	GameWorld()->DestroyEntity(this);
	return true;
}
