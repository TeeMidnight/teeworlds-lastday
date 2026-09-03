#include "entities/character.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "player.h"

#include <game/server/database/playerdb.h>
#include <game/server/database/playerdb_util.h>

void CPlayer::SaveStatus(CPlayerDB *pDB)
{
	if(!pDB || m_AccountUuid == UUID_ZEROED)
		return;

	SetJsonField(pDB, m_AccountUuid, CJsonPath().Key("sanity"), m_Status.m_Sanity);
	SetJsonField(pDB, m_AccountUuid, CJsonPath().Key("level"), m_Status.m_Level);
	SetJsonField(pDB, m_AccountUuid, CJsonPath().Key("hide_tip"), m_Status.m_HideTip);

	// characters
	if(m_pCharacter)
	{
		SetJsonField(pDB, m_AccountUuid, CJsonPath().Key("character").Key("health"), m_pCharacter->m_Health);
		SetJsonField(pDB, m_AccountUuid, CJsonPath().Key("character").Key("armor"), m_pCharacter->m_Armor);
	}
	// clear the previous inventory, then store each item as an array element
	CItemSystem::CInventory &Inventory = GameServer()->Item()->GetInventory(GetCID());
	pDB->DelJson(m_AccountUuid, CJsonPath().Key("inventory"));
	for(int i = 0; i < Inventory.m_NumItems; i++)
	{
		const CItemSystem::CInventory::SItem &Item = Inventory.m_aItems[i];
		SetJsonField(pDB, m_AccountUuid, CJsonPath().Key("inventory").Index(i).Key("res_id"), Item.m_aResId);
		SetJsonField(pDB, m_AccountUuid, CJsonPath().Key("inventory").Index(i).Key("count"), Item.m_Count);
	}

	// store the loadout slot layout (res_id per slot, "" for empty)
	SetJsonField(pDB, m_AccountUuid, CJsonPath().Key("loadout_set"), m_Status.m_LoadoutSet);
	pDB->DelJson(m_AccountUuid, CJsonPath().Key("loadout"));
	for(int i = 0; i < NUM_WEAPONS; i++)
		SetJsonField(pDB, m_AccountUuid, CJsonPath().Key("loadout").Index(i), m_Status.m_aLoadout[i]);
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

void CPlayer::LoadStatus(CPlayerDB *pDB)
{
	if(!pDB || m_AccountUuid == UUID_ZEROED)
		return;

	// reset to defaults
	m_Status.m_Sanity = 100;
	m_Status.m_Level = 0;
	m_Status.m_LoadoutSet = false;
	for(int i = 0; i < NUM_WEAPONS; i++)
		m_Status.m_aLoadout[i][0] = '\0';
	CItemSystem::CInventory &Inventory = GameServer()->Item()->GetInventory(GetCID());
	Inventory.m_NumItems = 0;
	mem_zero(Inventory.m_aItems, sizeof(Inventory.m_aItems));

	GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("sanity"), &m_Status.m_Sanity);
	GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("level"), &m_Status.m_Level);
	GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("hide_tip"), &m_Status.m_HideTip);

	// character infos
	GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("character").Key("health"), &m_pCharacter->m_Health);
	GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("character").Key("armor"), &m_pCharacter->m_Armor);

	// loadout slot layout (res_id per slot, "" = empty)
	GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("loadout_set"), &m_Status.m_LoadoutSet);
	int NumSlots = 0;
	if(pDB->GetJsonLength(m_AccountUuid, CJsonPath().Key("loadout"), &NumSlots))
	{
		for(int i = 0; i < NumSlots && i < NUM_WEAPONS; i++)
			GetJsonFieldRaw(pDB, m_AccountUuid, CJsonPath().Key("loadout").Index(i), m_Status.m_aLoadout[i], sizeof(m_Status.m_aLoadout[i]));
	}

	// read the inventory item by item (no whole-json read)
	int NumItems = 0;
	if(pDB->GetJsonLength(m_AccountUuid, CJsonPath().Key("inventory"), &NumItems))
	{
		for(int i = 0; i < NumItems && Inventory.m_NumItems < CItemSystem::CInventory::MAX_ITEMS; i++)
		{
			char aResId[32];
			if(!GetJsonFieldRaw(pDB, m_AccountUuid, CJsonPath().Key("inventory").Index(i).Key("res_id"), aResId, sizeof(aResId)))
				continue;
			int Count = 0;
			GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("inventory").Index(i).Key("count"), &Count);
			CItemSystem::CInventory::SItem &Item = Inventory.m_aItems[Inventory.m_NumItems];
			str_copy(Item.m_aResId, aResId, sizeof(Item.m_aResId));
			Item.m_Count = Count;
			Inventory.m_NumItems++;
		}
	}

	ApplyLoadout();
	// refresh menu
	GameServer()->GameMenu()->RefreshMenu(m_ClientID);
}
