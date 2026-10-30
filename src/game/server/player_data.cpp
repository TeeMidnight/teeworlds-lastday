/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <engine/shared/jsonparser.h>
#include <game/server/database/database.h>
#include <game/server/database/database_util.h>

#include "entities/character.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "player.h"

void CPlayer::CaptureStatus(CDbJob &Job)
{
	DbSet(Job, CJsonPath().Key("forgetting"), m_Status.m_Forgetting);
	DbSet(Job, CJsonPath().Key("level"), m_Status.m_Level);
	DbSet(Job, CJsonPath().Key("hide_tip"), m_Status.m_HideTip);
	DbSet(Job, CJsonPath().Key("language"), m_Status.m_aLanguage);
	// Keep the last displayed nickname with the account so the team menu can
	// name an offline member instead of printing a bare UUID. The name is
	// refreshed on every save, including the one made when the client drops.
	DbSet(Job, CJsonPath().Key("nickname"), Server()->ClientName(GetCID()));

	// characters
	if(m_pCharacter)
	{
		DbSet(Job, CJsonPath().Key("character").Key("health"), m_pCharacter->m_Health);
		DbSet(Job, CJsonPath().Key("character").Key("armor"), m_pCharacter->m_Armor);
	}
	// clear the previous inventory, then store each item as an array element.
	// An item is stored as its canonical UUID string plus its namespaced source
	// name (the name is only a human readable fallback).
	CItemSystem::CInventory &Inventory = GameServer()->Item()->GetInventory(GetCID());
	DbDel(Job, CJsonPath().Key("inventory"));
	int i = 0;
	for(int Slot = 0; Slot < CItemSystem::CInventory::MAX_ITEMS; Slot++)
	{
		if(Inventory.IsEmpty(Slot))
			continue;
		const CItemSystem::CInventory::SItem &Item = Inventory.m_aItems[Slot];
		char aUuid[UUID_MAXSTRSIZE];
		Item.m_Id.FormatUuid(aUuid, sizeof(aUuid));
		DbSet(Job, CJsonPath().Key("inventory").Index(i).Key("id"), aUuid);
		DbSet(Job, CJsonPath().Key("inventory").Index(i).Key("name"), Item.m_Id.SourceName());
		DbSet(Job, CJsonPath().Key("inventory").Index(i).Key("count"), Item.m_Count);
		// A backpack owns its grid under the item's own "data" field. Write it
		// as one JSON object: nested json_set paths would need every intermediate
		// key to exist already (and PostgreSQL's jsonb_set refuses to create
		// them), while the parent inventory slot always exists.
		if(Item.m_Data.m_HasData)
		{
			const int Slots = minimum((int) CItemSystem::MAX_ITEM_DATA_SLOTS, GameServer()->Item()->GetBackpackSlots(Item.m_Id));
			char aData[4096];
			str_format(aData, sizeof(aData), "{\"slots\":%d,\"items\":[", Slots);
			int Len = str_length(aData);
			bool First = true;
			for(int Nested = 0; Nested < Slots; Nested++)
			{
				const CItemSystem::SItemStack &Stack = Item.m_Data.m_aSlots[Nested];
				if(Stack.m_Id.IsZero() || Stack.m_Count <= 0)
					continue;
				char aNestedUuid[UUID_MAXSTRSIZE];
				Stack.m_Id.FormatUuid(aNestedUuid, sizeof(aNestedUuid));
				if(Len + 1 >= (int) sizeof(aData))
					break;
				str_format(aData + Len, sizeof(aData) - Len, "%s{\"slot\":%d,\"id\":\"%s\",\"count\":%d}",
					First ? "" : ",", Nested, aNestedUuid, Stack.m_Count);
				const int Added = str_length(aData + Len);
				if(Added <= 0 || Len + Added + 3 >= (int) sizeof(aData))
					break;
				Len += Added;
				First = false;
			}
			if(Len + 3 < (int) sizeof(aData))
				str_format(aData + Len, sizeof(aData) - Len, "]}");
			DbSetRaw(Job, CJsonPath().Key("inventory").Index(i).Key("data"), aData);
		}
		i++;
	}

	// store the loadout slot layout (uuid per slot, "" for empty)
	DbSet(Job, CJsonPath().Key("loadout_set"), m_Status.m_LoadoutSet);
	DbDel(Job, CJsonPath().Key("loadout"));
	for(int i = 0; i < NUM_WEAPONS; i++)
	{
		char aUuid[UUID_MAXSTRSIZE] = "";
		if(!m_Status.m_aLoadout[i].IsZero())
			m_Status.m_aLoadout[i].FormatUuid(aUuid, sizeof(aUuid));
		DbSet(Job, CJsonPath().Key("loadout").Index(i), aUuid);
	}

	// store the accessory layout the same way (uuid per slot, "" for empty)
	DbDel(Job, CJsonPath().Key("accessories"));
	for(int i = 0; i < NUM_ACCESSORY_SLOTS; i++)
	{
		char aUuid[UUID_MAXSTRSIZE] = "";
		if(!m_Status.m_aAccessories[i].IsZero())
			m_Status.m_aAccessories[i].FormatUuid(aUuid, sizeof(aUuid));
		DbSet(Job, CJsonPath().Key("accessories").Index(i), aUuid);
	}
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

void CPlayer::SaveNickname(CDatabase *pDB)
{
	if(!pDB || m_AccountUuid == UUID_ZEROED)
		return;
	SetJsonField(pDB, m_AccountUuid, CJsonPath().Key("nickname"), Server()->ClientName(GetCID()));
}

void CPlayer::CaptureLoadout()
{
	if(!m_pCharacter)
		return;

	for(int i = 0; i < NUM_WEAPONS; i++)
		m_Status.m_aLoadout[i] = m_pCharacter->WeaponAtSlot(i);
	// the layout was customized by the player, keep it from now on
	m_Status.m_LoadoutSet = true;
}

void CPlayer::ApplyLoadout()
{
	if(!m_pCharacter)
		return;
	// (re)build the layout from scratch
	for(int i = 0; i < NUM_WEAPONS; i++)
		m_pCharacter->EquipWeaponSlot(i, CItemID());

	for(int i = 0; i < NUM_WEAPONS; i++)
	{
		if(m_Status.m_aLoadout[i].IsZero())
			continue;
		m_pCharacter->EquipWeaponSlot(i, m_Status.m_aLoadout[i]);
	}
}

void CPlayer::CaptureAccessories()
{
	if(!m_pCharacter)
		return;

	for(int i = 0; i < NUM_ACCESSORY_SLOTS; i++)
		m_Status.m_aAccessories[i] = m_pCharacter->AccessoryAtSlot(i);
}

void CPlayer::ApplyAccessories()
{
	if(!m_pCharacter)
		return;

	// (re)build the layout from scratch; only owned accessories are worn
	CItemID aLayout[NUM_ACCESSORY_SLOTS];
	for(int i = 0; i < NUM_ACCESSORY_SLOTS; i++)
	{
		if(m_Status.m_aAccessories[i].IsZero())
			continue;
		if(!GameServer()->Item()->HasItem(GetCID(), m_Status.m_aAccessories[i]))
			continue;
		aLayout[i] = m_Status.m_aAccessories[i];
	}
	m_pCharacter->SetAccessoryLayout(aLayout);
}

void CPlayer::LoadStatus(CDatabase *pDB, bool RefreshMenu)
{
	if(!pDB || m_AccountUuid == UUID_ZEROED)
		return;

	// reset to defaults
	m_Status.m_Forgetting = 0;
	m_Status.m_Level = 0;
	m_Status.m_LoadoutSet = false;
	for(int i = 0; i < NUM_WEAPONS; i++)
		m_Status.m_aLoadout[i] = CItemID();
	for(int i = 0; i < NUM_ACCESSORY_SLOTS; i++)
		m_Status.m_aAccessories[i] = CItemID();
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
	// an account-level language overrides the session choice; no saved value
	// keeps whatever the player selected before logging in
	char aLanguage[16] = "";
	if(GetJsonFieldRaw(pDB, m_AccountUuid, CJsonPath().Key("language"), aLanguage, sizeof(aLanguage)) && aLanguage[0])
		SetLanguage(aLanguage);

	// character infos: a player may log in while dead or spectating, when
	// there is no character to write the saved health back into
	if(m_pCharacter)
	{
		GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("character").Key("health"), &m_pCharacter->m_Health);
		GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("character").Key("armor"), &m_pCharacter->m_Armor);
	}

	// loadout slot layout (uuid per slot, "" = empty). Old profiles stored the
	// bare item name, so parsing accepts both.
	GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("loadout_set"), &m_Status.m_LoadoutSet);
	int NumSlots = 0;
	if(pDB->GetJsonLength(m_AccountUuid, CJsonPath().Key("loadout"), &NumSlots))
	{
		char aSlot[UUID_MAXSTRSIZE];
		for(int i = 0; i < NumSlots && i < NUM_WEAPONS; i++)
		{
			if(!GetJsonFieldRaw(pDB, m_AccountUuid, CJsonPath().Key("loadout").Index(i), aSlot, sizeof(aSlot)))
				continue;
			CItemID Id;
			if(CItemID::Parse(aSlot, &Id))
				m_Status.m_aLoadout[i] = Id;
		}
	}

	// accessory layout (uuid per slot, "" = empty). A profile saved before
	// accessories existed simply has no "accessories" array.
	int NumAccessories = 0;
	if(pDB->GetJsonLength(m_AccountUuid, CJsonPath().Key("accessories"), &NumAccessories))
	{
		char aSlot[UUID_MAXSTRSIZE];
		for(int i = 0; i < NumAccessories && i < NUM_ACCESSORY_SLOTS; i++)
		{
			if(!GetJsonFieldRaw(pDB, m_AccountUuid, CJsonPath().Key("accessories").Index(i), aSlot, sizeof(aSlot)))
				continue;
			CItemID Id;
			if(CItemID::Parse(aSlot, &Id))
				m_Status.m_aAccessories[i] = Id;
		}
	}

	// read the inventory item by item (no whole-json read); items are stored
	// as a compact array, so each entry goes into the first free slot
	int NumItems = 0;
	if(pDB->GetJsonLength(m_AccountUuid, CJsonPath().Key("inventory"), &NumItems))
	{
		for(int i = 0; i < NumItems; i++)
		{
			// new profiles store the canonical uuid under "id"; old ones stored
			// the bare res_id under "res_id"
			char aId[64] = "";
			if(!GetJsonFieldRaw(pDB, m_AccountUuid, CJsonPath().Key("inventory").Index(i).Key("id"), aId, sizeof(aId)))
			{
				if(!GetJsonFieldRaw(pDB, m_AccountUuid, CJsonPath().Key("inventory").Index(i).Key("res_id"), aId, sizeof(aId)))
					continue;
			}
			CItemID Id;
			if(!CItemID::Parse(aId, &Id) || Id.IsZero())
				continue;
			int Count = 0;
			GetJsonField(pDB, m_AccountUuid, CJsonPath().Key("inventory").Index(i).Key("count"), &Count);
			if(Count <= 0 || !GameServer()->Item()->IsKnownItem(Id))
				continue;

			// Each row is one stack: restoring it into its own slot preserves
			// stack limits and any per-stack data instead of merging it away.
			CItemSystem::SItemData Data;
			const int BackpackSlots = GameServer()->Item()->GetBackpackSlots(Id);
			Data.m_HasData = BackpackSlots > 0;
			// "data" is one JSON object ({"slots":N,"items":[...]}); parse it
			// directly instead of walking many database paths.
			char aData[4096] = "";
			if(GetJsonFieldRaw(pDB, m_AccountUuid, CJsonPath().Key("inventory").Index(i).Key("data"), aData, sizeof(aData)))
			{
				CJsonParser Parser;
				json_value *pData = Parser.ParseString(aData, "inventory data");
				if(pData && pData->type == json_object)
				{
					const json_value &rItems = (*pData)["items"];
					for(unsigned n = 0; rItems.type == json_array && n < rItems.u.array.length; n++)
					{
						const json_value &rStack = rItems[n];
						if(rStack.type != json_object)
							continue;
						const json_value &rNestedId = rStack["id"];
						const json_value &rNestedCount = rStack["count"];
						if(rNestedId.type != json_string || rNestedCount.type != json_integer)
							continue;
						CItemID NestedId;
						if(!CItemID::Parse(rNestedId.u.string.ptr, &NestedId) || NestedId.IsZero() || !GameServer()->Item()->IsKnownItem(NestedId))
							continue;
						const int NestedCount = (int) (json_int_t) rNestedCount;
						if(NestedCount <= 0)
							continue;
						// the slot index keeps the backpack's grid stable across saves
						const json_value &rSlot = rStack["slot"];
						int NestedSlot = rSlot.type == json_integer ? (int) (json_int_t) rSlot : (int) n;
						NestedSlot = clamp(NestedSlot, 0, (int) CItemSystem::MAX_ITEM_DATA_SLOTS - 1);
						if(BackpackSlots > 0 && NestedSlot >= BackpackSlots)
							continue;
						Data.m_HasData = true;
						Data.m_aSlots[NestedSlot].m_Id = NestedId;
						Data.m_aSlots[NestedSlot].m_Count = NestedCount;
					}
				}
			}

			const int Slot = Inventory.FindEmpty();
			if(Slot < 0)
				break; // inventory full
			CItemSystem::CInventory::SItem &Item = Inventory.m_aItems[Slot];
			Item.m_Id = Id;
			Item.m_Count = Count;
			Item.m_Data = Data;
		}
	}

	ApplyLoadout();
	ApplyAccessories();
	// A guest profile is loaded before the client enters the game; sending vote
	// options then would race the join UI. The menu is generated on demand.
	if(RefreshMenu && GameServer()->GameMenu())
		GameServer()->GameMenu()->RefreshMenu(m_ClientID);
}
