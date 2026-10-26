/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/entities/container.h>

#include <base/math.h>

#include <climits>

#include <generated/protocol.h>

#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>

#include "container.h"

CContainerEntity::CContainerEntity(CGameWorld *pGameWorld, vec2 Pos, const char *pKind, const char *pName,
	const char *pLootTable, int Capacity, int Pickup, int StorageID) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_CONTAINER, 0, Pos, PHYS_SIZE)
{
	str_copy(m_aKind, pKind ? pKind : "", sizeof(m_aKind));
	str_copy(m_aName, pName && pName[0] ? pName : (pKind ? pKind : ""), sizeof(m_aName));
	str_copy(m_aLootTable, pLootTable ? pLootTable : "", sizeof(m_aLootTable));
	m_Capacity = clamp(Capacity, 1, (int) MAX_CONTAINER_SLOTS);
	m_Pickup = Pickup;
	m_StorageID = StorageID;
	m_LootInitialized = false;
	mem_zero(m_aSlots, sizeof(m_aSlots));

	for(int i = 0; i < NUM_BEAMS; i++)
		m_aBeamIDs[i] = Server()->SnapNewID(SnapIDWorld());

	GameWorld()->InsertEntity(this);
}

CContainerEntity::~CContainerEntity()
{
	for(int i = 0; i < NUM_BEAMS; i++)
		Server()->SnapFreeID(m_aBeamIDs[i], SnapIDWorld());
}

int CContainerEntity::UsedSlots() const
{
	int Used = 0;
	for(int i = 0; i < m_Capacity; i++)
	{
		if(!m_aSlots[i].m_Id.IsZero() && m_aSlots[i].m_Count > 0)
			Used++;
	}
	return Used;
}

int CContainerEntity::FindSlot(const CItemID &Id) const
{
	if(Id.IsZero())
		return -1;
	for(int i = 0; i < m_Capacity; i++)
	{
		if(m_aSlots[i].m_Id == Id && m_aSlots[i].m_Count > 0)
			return i;
	}
	return -1;
}

int CContainerEntity::FindEmptySlot() const
{
	for(int i = 0; i < m_Capacity; i++)
	{
		if(m_aSlots[i].m_Id.IsZero() || m_aSlots[i].m_Count <= 0)
			return i;
	}
	return -1;
}

const CContainerEntity::SSlot &CContainerEntity::Slot(int Index) const
{
	// An out of range index must read as an empty slot, not as slot 0: callers
	// act on the returned stack (take it, add it to an inventory), and silently
	// substituting a real slot could duplicate its contents.
	static const SSlot s_Empty = {CItemID(), 0};
	if(Index < 0 || Index >= m_Capacity)
		return s_Empty;
	return m_aSlots[Index];
}

int CContainerEntity::TotalCount() const
{
	int Total = 0;
	for(int i = 0; i < m_Capacity; i++)
	{
		if(!m_aSlots[i].m_Id.IsZero() && m_aSlots[i].m_Count > 0)
			Total += m_aSlots[i].m_Count;
	}
	return Total;
}

bool CContainerEntity::CanStore(const CItemID &Id) const
{
	if(Id.IsZero())
		return false;
	return FindSlot(Id) >= 0 || FindEmptySlot() >= 0;
}

bool CContainerEntity::Store(const CItemID &Id, int Count)
{
	if(Id.IsZero() || Count <= 0)
		return false;

	int Index = FindSlot(Id);
	if(Index < 0)
	{
		Index = FindEmptySlot();
		if(Index < 0)
			return false;
		m_aSlots[Index].m_Id = Id;
		m_aSlots[Index].m_Count = 0;
	}

	// keep a corrupt save document from overflowing the stack
	if(m_aSlots[Index].m_Count > INT_MAX - Count)
		return false;
	m_aSlots[Index].m_Count += Count;
	return true;
}

int CContainerEntity::Take(int SlotIndex, int Count)
{
	if(SlotIndex < 0 || SlotIndex >= m_Capacity || Count <= 0)
		return 0;
	SSlot &Slot = m_aSlots[SlotIndex];
	if(Slot.m_Id.IsZero() || Slot.m_Count <= 0)
		return 0;

	const int Removed = minimum(Count, Slot.m_Count);
	Slot.m_Count -= Removed;
	if(Slot.m_Count <= 0)
	{
		Slot.m_Id = CItemID();
		Slot.m_Count = 0;
	}
	return Removed;
}

bool CContainerEntity::RestoreSlot(const CItemID &Id, int Count)
{
	if(Id.IsZero() || Count <= 0)
		return false;

	int Index = FindSlot(Id);
	if(Index < 0)
	{
		Index = FindEmptySlot();
		if(Index < 0)
			return false;
		m_aSlots[Index].m_Id = Id;
		m_aSlots[Index].m_Count = 0;
	}
	if(m_aSlots[Index].m_Count > INT_MAX - Count)
		return false;
	m_aSlots[Index].m_Count += Count;
	return true;
}

void CContainerEntity::SnapBeam(int ID, vec2 From, vec2 To, int SnappingClient)
{
	if(ID < 0 || NetworkClippedLine(SnappingClient, From, To))
		return;

	CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(NETOBJTYPE_LASER, ID, sizeof(CNetObj_Laser)));
	if(!pObj)
		return;

	pObj->m_X = round_to_int(To.x);
	pObj->m_Y = round_to_int(To.y);
	pObj->m_FromX = round_to_int(From.x);
	pObj->m_FromY = round_to_int(From.y);
	pObj->m_StartTick = Server()->Tick();
}

void CContainerEntity::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	// the body sits on the tile's bottom edge; taller kinds simply get a taller
	// box, which makes a vault read differently from a crate in the world
	const float Bottom = m_Pos.y + 16.0f;
	const float Top = Bottom - (18.0f + 6.0f * m_Capacity);
	const float LidY = Top + 7.0f;
	const float Left = m_Pos.x - 16.0f;
	const float Right = m_Pos.x + 16.0f;

	SnapBeam(m_aBeamIDs[0], vec2(Left, Bottom), vec2(Right, Bottom), SnappingClient);
	SnapBeam(m_aBeamIDs[1], vec2(Left, Top), vec2(Left, Bottom), SnappingClient);
	SnapBeam(m_aBeamIDs[2], vec2(Right, Top), vec2(Right, Bottom), SnappingClient);
	SnapBeam(m_aBeamIDs[3], vec2(Left, Top), vec2(Right, Top), SnappingClient);
	SnapBeam(m_aBeamIDs[4], vec2(Left, LidY), vec2(Right, LidY), SnappingClient);
	SnapBeam(m_aBeamIDs[5], vec2(m_Pos.x, LidY), vec2(m_Pos.x, LidY + 8.0f), SnappingClient);

	// the icon in the middle names the kind of container; a real pickup entity
	// does not exist here, this is only a snapshot primitive
	CNetObj_Pickup *pP = static_cast<CNetObj_Pickup *>(Server()->SnapNewItem(NETOBJTYPE_PICKUP, GetID(), sizeof(CNetObj_Pickup)));
	if(!pP)
		return;
	pP->m_X = round_to_int(m_Pos.x);
	pP->m_Y = round_to_int(m_Pos.y + 4.0f);
	pP->m_Type = clamp(m_Pickup, 0, NUM_PICKUPS - 1);
}
