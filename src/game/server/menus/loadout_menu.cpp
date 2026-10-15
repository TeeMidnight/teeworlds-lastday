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

// -- loadout (weapon equipment) menu ------------------------------------

bool CGameMenu::MenuLoadout(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	// a slot was clicked: remember it and show the weapon picker
	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "NONE") != 0 && str_comp(VoteStatus.m_aCmd, "DISPLAY") != 0)
	{
		int Slot = -1;
		if(sscanf(VoteStatus.m_aCmd, "PICK %d", &Slot) == 1 && Slot >= 0 && Slot < NUM_WEAPONS)
		{
			pMenu->m_aLoadoutSlot[ClientID] = Slot;
			pMenu->SetPlayerPage(ClientID, "WEAPONPICK");
			return false;
		}
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	CCharacter *pChr = pMenu->GameServer()->GetPlayerChar(ClientID);
	if(pChr)
		pChr->UnequipMissingWeapons(); // drop slots whose item is gone

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

	return true;
}

bool CGameMenu::MenuWeaponPick(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	const int Slot = pMenu->m_aLoadoutSlot[ClientID];

	// an item was clicked: put it onto the pending slot, then go back
	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "NONE") != 0 && str_comp(VoteStatus.m_aCmd, "DISPLAY") != 0)
	{
		CCharacter *pChr = pMenu->GameServer()->GetPlayerChar(ClientID);
		CItemID Id;
		if(pChr && Slot >= 0 && Slot < NUM_WEAPONS && CItemID::Parse(VoteStatus.m_aCmd, &Id) &&
			pChr->EquipWeaponSlot(Slot, Id))
		{
			// persist the new slot layout for the next spawn
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
	pMenu->AddOption(Localize("Select an item to place on this slot.", "Menu Loadout"), "DISPLAY", "-");
	pMenu->AddHorizontalRule();

	// any owned item can be placed on a slot; only weapon items actually fire
	CItemSystem *pItem = pMenu->GameServer()->Item();
	const CItemSystem::CInventory &Inventory = pItem->GetInventory(ClientID);
	for(int i = 0; i < CItemSystem::CInventory::MAX_ITEMS; i++)
	{
		if(Inventory.IsEmpty(i))
			continue;
		const CItemID &Id = Inventory.m_aItems[i].m_Id;
		const char *pName = pItem->GetName(Id);
		char aCmd[UUID_MAXSTRSIZE];
		ItemCommand(Id, aCmd, sizeof(aCmd));
		pMenu->AddOption(Localize(pName, "Item Name"), aCmd);
	}

	return true;
}
