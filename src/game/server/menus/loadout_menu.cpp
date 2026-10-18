/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/localization_format.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gamemenu.h>
#include <game/server/item.h>
#include <game/server/player.h>
#include <game/server/weapon.h>
#include <game/server/weaponmanager.h>

#include "item_menu.h"

// -- loadout (weapon + accessory equipment) menu -------------------------

// a compact effect summary for an accessory, e.g. "+4 HP +20% range"
static std::string AccessoryEffectSummary(const CItemSystem::SAccessory &Acc)
{
	std::string Summary;
	if(Acc.m_Health > 0)
		Summary += std::format("+{} HP ", Acc.m_Health);
	if(Acc.m_Range > 0.0f)
		Summary += std::format("+{}% range ", (int) (Acc.m_Range * 100.0f));
	if(Acc.m_Speed > 0.0f)
		Summary += std::format("+{}% speed ", (int) (Acc.m_Speed * 100.0f));
	// drop the trailing space so the parenthesised summary reads cleanly
	while(!Summary.empty() && Summary.back() == ' ')
		Summary.pop_back();
	return Summary;
}

bool CGameMenu::MenuLoadout(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	// a slot was clicked: remember it and show the matching item picker
	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "NONE") != 0 && str_comp(VoteStatus.m_aCmd, "DISPLAY") != 0)
	{
		int Slot = -1;
		if(sscanf(VoteStatus.m_aCmd, "PICK %d", &Slot) == 1 && Slot >= 0 && Slot < (int) NUM_WEAPONS + (int) NUM_ACCESSORY_SLOTS)
		{
			pMenu->m_aLoadoutSlot[ClientID] = Slot;
			pMenu->SetPlayerPage(ClientID, Slot < NUM_WEAPONS ? "WEAPONPICK" : "ACCESSORYPICK");
			return false;
		}
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	CCharacter *pChr = pMenu->GameServer()->GetPlayerChar(ClientID);
	if(pChr)
	{
		pChr->UnequipMissingWeapons(); // drop slots whose item is gone
		pChr->UnequipMissingAccessories();
	}

	CItemSystem *pItem = pMenu->GameServer()->Item();
	for(int i = 0; i < NUM_WEAPONS; i++)
	{
		const CItemID Item = pChr ? pChr->WeaponAtSlot(i) : CItemID();
		const char *pWeaponName = Item.IsZero() ? "" : pItem->GetWeaponName(Item);
		const IWeaponInterface *pWeapon = pWeaponName[0] ? WeaponManager()->GetWeaponByName(pWeaponName) : nullptr;

		// resolve the display name through the item system (translated)
		const char *pRawName = !Item.IsZero() ? pItem->GetName(Item) : (pWeapon ? pWeapon->Name() : nullptr);

		std::string Desc;
		if(pRawName && pRawName[0])
		{
			const char *pName = Localize(pRawName, "Item Name");
			if(pWeapon)
				Desc = std::format("{}. {}", i + 1, pName);
			else
				// an item without a corresponding weapon (e.g. a resource):
				// falls back to the hand
				Desc = LocalizeFormat("Menu Loadout", "{}. {} (hand)", i + 1, pName);
		}
		else if(!Item.IsZero())
			Desc = LocalizeFormat("Menu Loadout", "{}. (hand)", i + 1);
		else
			Desc = LocalizeFormat("Menu Loadout", "{}. (empty)", i + 1);

		char aCmd[16];
		str_format(aCmd, sizeof(aCmd), "PICK %d", i);
		pMenu->AddOption(Desc.c_str(), aCmd);
	}

	// accessory slots: passive items, shown with their effect summary
	for(int i = 0; i < NUM_ACCESSORY_SLOTS; i++)
	{
		const int SlotNumber = NUM_WEAPONS + i + 1;
		const CItemID Item = pChr ? pChr->AccessoryAtSlot(i) : CItemID();

		std::string Desc;
		if(!Item.IsZero())
		{
			const char *pName = Localize(pItem->GetName(Item), "Item Name");
			const CItemSystem::SAccessory Acc = pItem->GetAccessory(Item);
			const std::string Summary = AccessoryEffectSummary(Acc);
			if(Summary.empty())
				Desc = std::format("{}. {} (accessory)", SlotNumber, pName);
			else
				Desc = std::format("{}. {} ({})", SlotNumber, pName, Summary);
		}
		else
			Desc = LocalizeFormat("Menu Loadout", "{}. (empty accessory slot)", SlotNumber);

		char aCmd[16];
		str_format(aCmd, sizeof(aCmd), "PICK %d", NUM_WEAPONS + i);
		pMenu->AddOption(Desc.c_str(), aCmd);
	}

	return true;
}

bool CGameMenu::MenuWeaponPick(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	// one picker serves both the weapon slots (0..NUM_WEAPONS-1) and the
	// accessory slots (NUM_WEAPONS..NUM_WEAPONS+NUM_ACCESSORY_SLOTS-1)
	const int Slot = pMenu->m_aLoadoutSlot[ClientID];
	const bool IsAccessory = Slot >= NUM_WEAPONS;
	const int LocalSlot = IsAccessory ? Slot - NUM_WEAPONS : Slot;
	const int NumSlots = IsAccessory ? (int) NUM_ACCESSORY_SLOTS : (int) NUM_WEAPONS;

	// an item was clicked: put it onto the pending slot, then go back
	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "NONE") != 0 && str_comp(VoteStatus.m_aCmd, "DISPLAY") != 0)
	{
		CCharacter *pChr = pMenu->GameServer()->GetPlayerChar(ClientID);
		bool Placed = false;
		if(pChr && LocalSlot >= 0 && LocalSlot < NumSlots)
		{
			CItemID Id;
			const bool IsClear = str_comp(VoteStatus.m_aCmd, "CLEAR") == 0;
			if(IsClear)
			{
				Id = CItemID();
				Placed = IsAccessory ? pChr->EquipAccessorySlot(LocalSlot, Id) : pChr->EquipWeaponSlot(LocalSlot, Id);
			}
			else if(CItemID::Parse(VoteStatus.m_aCmd, &Id))
				Placed = IsAccessory ? pChr->EquipAccessorySlot(LocalSlot, Id) : pChr->EquipWeaponSlot(LocalSlot, Id);
		}

		if(Placed)
		{
			// persist the new layout for the next spawn
			if(IsAccessory)
				pPlayer->CaptureAccessories();
			else
				pPlayer->CaptureLoadout();
			pMenu->GameServer()->SendChat(-1, CHAT_WHISPER, ClientID, Localize("Item placed on the slot.", "Menu Loadout"));
		}
		else
			pMenu->GameServer()->SendChat(-1, CHAT_WHISPER, ClientID, Localize("Failed to place that item.", "Menu Loadout"));
		pMenu->SetPlayerPage(ClientID, "LOADOUT");
		return false;
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	const char *pPrompt = IsAccessory
		? "Select an accessory to wear on this slot."
		: "Select an item to place on this slot.";
	pMenu->AddOption(Localize(pPrompt, "Menu Loadout"), "DISPLAY", "-");
	pMenu->AddHorizontalRule();
	pMenu->AddOption(Localize("Clear this slot", "Menu Loadout"), "CLEAR", "-");
	pMenu->AddHorizontalRule();

	// any owned item can be placed on a weapon slot; only accessory items can
	// be placed on an accessory slot
	CItemSystem *pItem = pMenu->GameServer()->Item();
	const CItemSystem::CInventory &Inventory = pItem->GetInventory(ClientID);
	for(int i = 0; i < CItemSystem::CInventory::MAX_ITEMS; i++)
	{
		if(Inventory.IsEmpty(i))
			continue;
		const CItemID &Id = Inventory.m_aItems[i].m_Id;
		if(IsAccessory && !pItem->IsAccessory(Id))
			continue;
		const char *pName = pItem->GetName(Id);
		char aCmd[UUID_MAXSTRSIZE];
		ItemCommand(Id, aCmd, sizeof(aCmd));
		pMenu->AddOption(Localize(pName, "Item Name"), aCmd);
	}

	return true;
}
