/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/entities/drill.h>

#include <game/server/gamecontext.h>
#include <game/server/item.h>
#include <game/server/player.h>

#include <generated/protocol.h>

#include "character.h"
#include "dropped_pickup.h"
#include "resource.h"

CDrillEntity::CDrillEntity(CGameWorld *pGameWorld, vec2 Pos, const CItemID &ItemId, int MaxLength, int Power, int Owner) :
	CHitableEntity(pGameWorld, CGameWorld::ENTTYPE_DRILL, 0, Pos, 24)
{
	m_ItemId = ItemId;
	m_Owner = Owner;
	m_Controller = -1;
	m_MaxLength = maximum(64, MaxLength);
	m_Power = maximum(1, Power);
	m_Length = 0.0f;
	m_Dir = vec2(0.0f, 1.0f);
	m_State = STATE_IDLE;
	m_WaitRelease = false;

	for(int i = 0; i < 4; i++)
		m_aBeamIDs[i] = Server()->SnapNewID(SnapIDWorld());

	GameWorld()->InsertEntity(this);
}

CDrillEntity::~CDrillEntity()
{
	for(int i = 0; i < 4; i++)
		Server()->SnapFreeID(m_aBeamIDs[i], SnapIDWorld());
}

void CDrillEntity::SetDirection(vec2 Dir)
{
	if(length(Dir) > 0.0001f)
		m_Dir = normalize(Dir);
}

void CDrillEntity::SetController(int ClientID)
{
	if(m_Controller == ClientID)
		return;

	// release the previous controller's frozen character
	if(m_Controller >= 0)
	{
		CPlayer *pOld = GameServer()->m_apPlayers[m_Controller];
		CCharacter *pOldChr = pOld ? pOld->GetCharacter() : nullptr;
		if(pOldChr)
			pOldChr->SetDrillControl(false);
	}

	m_Controller = ClientID;
	// wind back smoothly rather than snapping the beam away, and require a
	// fresh fire press for the next pass
	m_State = m_Length > 0.0f ? STATE_RETRACTING : STATE_IDLE;
	m_WaitRelease = true;
}

CResourceEntity *CDrillEntity::FindResource(vec2 *pOutPos)
{
	if(m_Length <= 1.0f)
		return nullptr;

	// Collect the nodes around the whole beam, then keep the nearest one that
	// is actually harvestable. Respawning nodes are invisible but still exist,
	// so letting them win the "nearest hit" race would make a live node behind
	// one look unreachable until the drill is moved and the line changes.
	const vec2 TipPos = Tip();
	array<CEntity *> lCandidates;
	const int Num = GameWorld()->FindEntities(m_Pos, distance(m_Pos, TipPos) + 32.0f, lCandidates, CGameWorld::ENTTYPE_RESOURCE);

	float ClosestLen = distance(m_Pos, TipPos) * 100.0f;
	CResourceEntity *pClosest = nullptr;
	vec2 ClosestHit = TipPos;
	for(int i = 0; i < Num; i++)
	{
		CResourceEntity *pRes = static_cast<CResourceEntity *>(lCandidates[i]);
		if(pRes->IsRespawning())
			continue;

		// FindEntities also sees a live ring neighbour, where the entity needs
		// the boundary offset applied before its position can be compared
		const vec2 ResPos = GameWorld()->PosInFrame(pRes);
		const vec2 IntersectPos = closest_point_on_line(m_Pos, TipPos, ResPos);
		if(distance(ResPos, IntersectPos) >= pRes->GetProximityRadius() + 16.0f)
			continue;

		const float Len = distance(m_Pos, IntersectPos);
		if(Len < ClosestLen)
		{
			ClosestLen = Len;
			pClosest = pRes;
			ClosestHit = IntersectPos;
		}
	}

	if(pOutPos)
		*pOutPos = ClosestHit;
	return pClosest;
}

void CDrillEntity::Harvest(CResourceEntity *pRes, vec2 HitPos)
{
	if(!pRes || pRes->IsRespawning())
		return;

	// the player currently steering gets the ore; an unmanned drill cannot
	// extend, so the placer slot is only a safety fallback
	const int Collector = m_Controller >= 0 ? m_Controller : m_Owner;
	if(Collector < 0)
		return;

	// a stronger drill removes more hardness per pass; a node is depleted when
	// this pass will take its last point, so the drop is banked before it goes
	const bool Depleted = pRes->RemainingHardness() <= m_Power;
	if(Depleted)
	{
		// bank the drop first: a full inventory must not consume the node for
		// an item nobody receives
		if(!GameServer()->Item()->AddItem(Collector, pRes->Item(), 1))
			return;
		pRes->TakeHit(m_Power);
		GameWorld()->CreateSound(HitPos, SOUND_PICKUP_ARMOR);
	}
	else
	{
		pRes->TakeHit(m_Power);
		GameWorld()->CreateHammerHit(HitPos);
	}
}

void CDrillEntity::Tick()
{
	if(m_Controller >= 0)
	{
		CCharacter *pChr = GameServer()->GetPlayerChar(m_Controller);
		if(!pChr)
		{
			SetController(-1);
		}
		else
		{
			// The cursor aims the beam, but the control target is expressed
			// relative to the character while the beam starts at the drill. The
			// player may stand up to CONTROL_RADIUS away, so using the tee-relative
			// direction would skew the beam and leave a resource that is in reach
			// and under the cursor out of the beam's grasp. Aim at the cursor's
			// actual world position instead.
			const vec2 CursorPos = pChr->GetPos() + pChr->AimVector();
			const vec2 ToCursor = CursorPos - m_Pos;
			if(length(ToCursor) > 0.0001f)
				m_Dir = normalize(ToCursor);
			else
				m_Dir = pChr->AimDirection();
			const bool Firing = pChr->IsFiring();
			// after an automatic retraction the player has to let go before the
			// next pass, otherwise holding fire would chain boring runs forever
			if(!Firing)
				m_WaitRelease = false;
			if(m_State == STATE_IDLE && Firing && !m_WaitRelease)
			{
				m_State = STATE_EXTENDING;
				GameWorld()->CreateSound(m_Pos, SOUND_LASER_FIRE);
			}
			else if(m_State == STATE_EXTENDING && !Firing)
			{
				m_State = STATE_RETRACTING;
			}
		}
	}
	else if(m_State != STATE_IDLE)
	{
		// an unmanned drill never stays extended
		m_State = STATE_RETRACTING;
	}

	if(m_State == STATE_EXTENDING)
	{
		m_Length += EXTEND_SPEED;

		vec2 HitPos = Tip();
		CResourceEntity *pRes = FindResource(&HitPos);
		if(pRes && !pRes->IsRespawning())
		{
			// touching ore mines this drill's power and pulls the beam back
			Harvest(pRes, HitPos);
			m_State = STATE_RETRACTING;
			m_WaitRelease = true;
		}
		else if(m_Length >= (float) m_MaxLength)
		{
			// reached the drill's limit: wind back in
			m_Length = (float) m_MaxLength;
			m_State = STATE_RETRACTING;
			m_WaitRelease = true;
		}
	}
	else if(m_State == STATE_RETRACTING)
	{
		m_Length -= RETRACT_SPEED;
		if(m_Length <= 0.0f)
		{
			m_Length = 0.0f;
			m_State = STATE_IDLE;
		}
	}
}

void CDrillEntity::SnapBeam(int ID, vec2 From, vec2 To, int SnappingClient)
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

void CDrillEntity::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	// body: the pickup at the centre of the tripod
	CNetObj_Pickup *pP = static_cast<CNetObj_Pickup *>(Server()->SnapNewItem(NETOBJTYPE_PICKUP, GetID(), sizeof(CNetObj_Pickup)));
	if(pP)
	{
		pP->m_X = round_to_int(m_Pos.x);
		pP->m_Y = round_to_int(m_Pos.y);
		pP->m_Type = PICKUP_HAMMER;
	}

	// three fixed legs make the drill read as a standing tripod rather than a
	// floating dot; they do not follow the cursor
	SnapBeam(m_aBeamIDs[0], m_Pos, m_Pos + vec2(-22.0f, 18.0f), SnappingClient);
	SnapBeam(m_aBeamIDs[1], m_Pos, m_Pos + vec2(22.0f, 18.0f), SnappingClient);
	SnapBeam(m_aBeamIDs[2], m_Pos, m_Pos + vec2(0.0f, 22.0f), SnappingClient);

	// the boring beam. A controller always sees a short stub in the aim
	// direction, so the cursor visibly turns the drill even before it bores.
	if(m_Length > 0.5f || m_Controller >= 0)
	{
		const float BeamLength = maximum(m_Length, 20.0f);
		SnapBeam(m_aBeamIDs[3], m_Pos, m_Pos + m_Dir * BeamLength, SnappingClient);
	}
}

bool CDrillEntity::TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	(void) Force;
	(void) Source;
	(void) Dmg;
	(void) pFrom;
	(void) Weapon;

	// dismantle: drop the drill item where the building stood, so whoever
	// breaks it can pick it up (and a recycled client slot can never receive
	// another player's item)
	if(!m_ItemId.IsZero())
		new CDroppedPickup(GameWorld(), m_Pos + vec2(0.0f, -16.0f), vec2(0.0f, 0.0f), m_ItemId, 1);

	if(m_Controller >= 0)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[m_Controller];
		if(pPlayer)
			pPlayer->ExitDrillControl();
	}

	GameWorld()->DestroyEntity(this);
	return true;
}
