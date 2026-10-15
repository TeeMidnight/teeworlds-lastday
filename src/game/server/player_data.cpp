/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include "entities/character.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "player.h"

#include <game/server/database/database.h>
#include <game/server/database/database_util.h>

void CPlayer::CaptureStatus(CDbJob &Job)
{
	DbSet(Job, CJsonPath().Key("forgetting"), m_Status.m_Forgetting);
	DbSet(Job, CJsonPath().Key("level"), m_Status.m_Level);
	DbSet(Job, CJsonPath().Key("hide_tip"), m_Status.m_HideTip);

	// characters
	if(m_pCharacter)
	{
		DbSet(Job, CJsonPath().Key("character").Key("health"), m_pCharacter->m_Health);
		DbSet(Job, CJsonPath().Key("character").Key("armor"), m_pCharacter->m_Armor);
	}
	// clear the previous inventory, then store each item as an array element
	CItemSystem::CInventory &Inventory = GameServer()->Item()->GetInventory(GetCID());
	DbDel(Job, CJsonPath().Key("inventory"));
	int i = 0;
	for(int Slot = 0; Slot < CItemSystem::CInventory::MAX_ITEMS; Slot++)
	{
		if(Inventory.IsEmpty(Slot))
			continue;
		const CItemSystem::CInventory::SItem &Item = Inventory.m_aItems[Slot];
		DbSet(Job, CJsonPath().Key("inventory").Index(i).Key("res_id"), Item.m_aResId);
		DbSet(Job, CJsonPath().Key("inventory").Index(i).Key("count"), Item.m_Count);
		i++;
	}

	// store the loadout slot layout (res_id per slot, "" for empty)
	DbSet(Job, CJsonPath().Key("loadout_set"), m_Status.m_LoadoutSet);
	DbDel(Job, CJsonPath().Key("loadout"));
	for(int i = 0; i < NUM_WEAPONS; i++)
		DbSet(Job, CJsonPath().Key("loadout").Index(i), m_Status.m_aLoadout[i]);
}

void CPlayer::SaveStatus(CDatabase *pDB)
{
	if(!pDB || m_AccountUuid == UUID_ZEROED)
		return;

	// login/register need the write to have happened before they continue, so
	// they run the same snapshot synchronously; everything else goes through
	// CGameContext::SavePlayerData and the thread pool
	CDbJob Job;
	Job.m_Type = CDbJob::TYPE_PLAYER;
	Job.m_pDB = pDB;
	Job.m_Uuid = m_AccountUuid;
	CaptureStatus(Job);
	RunDbJob(&Job);
}

void CPlayer::CaptureLoadout()
{
	if(!m_pCharacter)
		return;

	for(int i = 0; i < NUM_WEAPONS; i++)
	{
		m_Status.m_aLoadout[i][0] = '\0';
		const unsigned ItemHash = m_pCharacter->WeaponAtSlot(i);
		if(ItemHash != 0)
		{
			// translate the slot hash back to the item res_id
			const char *pResId = GameServer()->Item()->GetResIdByHash(GetCID(), ItemHash);
			if(pResId)
				str_copy(m_Status.m_aLoadout[i], pResId, sizeof(m_Status.m_aLoadout[i]));
		}
	}
	// the layout was customized by the player, keep it from now on
	m_Status.m_LoadoutSet = true;
}

void CPlayer::ApplyLoadout()
{
	if(!m_pCharacter)
		return;
	// (re)build the layout from scratch
	for(int i = 0; i < NUM_WEAPONS; i++)
		m_pCharacter->EquipWeaponSlot(i, 0);

	for(int i = 0; i < NUM_WEAPONS; i++)
	{
		const char *pResId = m_Status.m_aLoadout[i];
		if(!pResId[0])
			continue;
		m_pCharacter->EquipWeaponSlot(i, str_quickhash(pResId));
	}
}

void CPlayer::LoadStatus(CDatabase *pDB)
{
	if(!pDB || m_AccountUuid == UUID_ZEROED)
		return;

	// reset to defaults
	m_Status.m_Forgetting = 0;
	m_Status.m_Level = 0;
	m_Status.m_LoadoutSet = false;
	for(int i = 0; i < NUM_WEAPONS; i++)
		m_Status.m_aLoadout[i][0] = '\0';
	CItemSystem::CInventory &Inventory = GameServer()->Item()->GetInventory(GetCID());
	mem_zero(Inventory.m_aItems, sizeof(Inventory.m_aItems));

	// mercury chapter: the forgetting meter (0 = clear mind). profiles saved
	// by the sulfur chapter stored a "sanity" value instead (100 = sane);
	// migrate it by inverting it into forgetting.
	if(!GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("forgetting"), &m_Status.m_Forgetting))
	{
		int LegacySanity = -1;
		if(GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("sanity"), &LegacySanity))
			m_Status.m_Forgetting = clamp(100 - LegacySanity, 0, 100);
	}
	GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("level"), &m_Status.m_Level);
	GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("hide_tip"), &m_Status.m_HideTip);

	// character infos: a player may log in while dead or spectating, when
	// there is no character to write the saved health back into
	if(m_pCharacter)
	{
		GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("character").Key("health"), &m_pCharacter->m_Health);
		GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("character").Key("armor"), &m_pCharacter->m_Armor);
	}

	// loadout slot layout (res_id per slot, "" = empty)
	GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("loadout_set"), &m_Status.m_LoadoutSet);
	int NumSlots = 0;
	if(pDB->GetJsonLength(m_AccountUuid, CJsonPath().Key("loadout"), &NumSlots))
	{
		for(int i = 0; i < NumSlots && i < NUM_WEAPONS; i++)
			GetJsonFieldRaw(pDB, m_AccountUuid, CJsonPath().Key("loadout").Index(i), m_Status.m_aLoadout[i], sizeof(m_Status.m_aLoadout[i]));
	}

	// read the inventory item by item (no whole-json read); items are stored
	// as a compact array, so each entry goes into the first free slot
	int NumItems = 0;
	if(pDB->GetJsonLength(m_AccountUuid, CJsonPath().Key("inventory"), &NumItems))
	{
		for(int i = 0; i < NumItems; i++)
		{
			char aResId[32];
			if(!GetJsonFieldRaw(pDB, m_AccountUuid, CJsonPath().Key("inventory").Index(i).Key("res_id"), aResId, sizeof(aResId)))
				continue;
			int Count = 0;
			GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("inventory").Index(i).Key("count"), &Count);
			if(Count <= 0 || !GameServer()->Item()->IsKnownItem(aResId))
				continue;
			// merge into an existing stack first: a legacy or hand-edited row can
			// list the same res_id twice, and a second slot would be invisible to
			// Find/AddItem and could never be used up
			const int Existing = Inventory.Find(aResId);
			if(Existing >= 0)
			{
				Inventory.m_aItems[Existing].m_Count += Count;
				continue;
			}
			const int Slot = Inventory.FindEmpty();
			if(Slot < 0)
				break; // inventory full
			CItemSystem::CInventory::SItem &Item = Inventory.m_aItems[Slot];
			str_copy(Item.m_aResId, aResId, sizeof(Item.m_aResId));
			Item.m_Count = Count;
		}
	}

	ApplyLoadout();
	// refresh menu
	GameServer()->GameMenu()->RefreshMenu(m_ClientID);
}
