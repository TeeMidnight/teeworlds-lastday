/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/localization_format.h>
#include <game/server/entities/container.h>
#include <game/server/gamecontext.h>
#include <game/server/gamemenu.h>
#include <game/server/item.h>
#include <game/server/player.h>

#include "item_menu.h"

// The vote reason box is reused as the amount field: empty (or non positive)
// means "the whole stack", exactly like the inventory menu's use/drop actions.
static int ContainerRequestedCount(const CCallVoteStatus &VoteStatus)
{
	if(!VoteStatus.m_aReason[0])
		return 0;
	const int Count = str_toint(VoteStatus.m_aReason);
	return Count > 0 ? Count : 0;
}

// A container is opened from the world: the player aims at it and the main menu
// offers the page. The same resolver is used by the menu and by /container, so
// both always point at the same prop.
bool CGameMenu::MenuContainer(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CGameContext *pGameServer = pMenu->GameServer();
	CPlayer *pPlayer = pGameServer->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	CItemSystem *pItems = pGameServer->Item();
	CContainerEntity *pContainer = pGameServer->AimedContainer(ClientID);
	if(!pContainer)
	{
		pMenu->ClearOptions(ClientID);
		pMenu->AddPageTitle();
		pMenu->AddWrappedLinesOption(Localize("Aim at a container first.", "Menu Container"));
		return true;
	}

	// ----- actions -----
	// Take one stack back into the inventory. The container is emptied only
	// after the inventory accepted the whole requested amount.
	if(str_startswith(VoteStatus.m_aCmd, "CTAKE "))
	{
		const int Index = str_toint(VoteStatus.m_aCmd + str_length("CTAKE "));
		const CContainerEntity::SSlot &Slot = pContainer->Slot(Index);
		if(!Slot.m_Id.IsZero() && Slot.m_Count > 0)
		{
			const int Requested = ContainerRequestedCount(VoteStatus);
			const int Count = Requested > 0 ? minimum(Requested, Slot.m_Count) : Slot.m_Count;
			if(pItems->AddItem(ClientID, Slot.m_Id, Count, true))
			{
				pContainer->Take(Index, Count);
				pGameServer->SavePlayerData(pPlayer);
			}
			else
				pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("No inventory space!", "Item Pickup"));
		}
	}
	// Store one inventory stack in the container. A container only needs a free
	// slot for a new item kind; a matching kind always fits.
	else if(str_startswith(VoteStatus.m_aCmd, "CSTORE "))
	{
		CItemID Id;
		if(CItemID::Parse(VoteStatus.m_aCmd + str_length("CSTORE "), &Id) && !Id.IsZero())
		{
			const int Owned = pItems->GetItemCount(ClientID, Id);
			if(Owned > 0)
			{
				const int Requested = ContainerRequestedCount(VoteStatus);
				const int Count = Requested > 0 ? minimum(Requested, Owned) : Owned;
				if(!pContainer->CanStore(Id))
				{
					pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("The container is full.", "Container"));
				}
				else if(pItems->RemoveItem(ClientID, Id, Count))
				{
					if(!pContainer->Store(Id, Count))
					{
						// only a stack near INT_MAX can fail here; give the
						// items back instead of losing them
						pItems->AddItem(ClientID, Id, Count, true);
					}
					else
					{
						pGameServer->SavePlayerData(pPlayer);
						const std::string Msg = LocalizeFormat("Container", "Stored: {} x{}",
							pItems->GetName(Id), Count);
						pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Msg.c_str());
					}
				}
			}
		}
	}
	// Take every stack, stopping as soon as the inventory cannot take more.
	else if(str_comp(VoteStatus.m_aCmd, "CTAKEALL") == 0)
	{
		int Moved = 0;
		bool NoSpace = false;
		for(int i = 0; i < pContainer->Capacity(); i++)
		{
			const int Count = pContainer->Slot(i).m_Count;
			if(pContainer->Slot(i).m_Id.IsZero() || Count <= 0)
				continue;
			if(!pItems->AddItem(ClientID, pContainer->Slot(i).m_Id, Count, true))
			{
				NoSpace = true;
				break;
			}
			pContainer->Take(i, Count);
			Moved += Count;
		}
		if(Moved > 0)
		{
			pGameServer->SavePlayerData(pPlayer);
			const std::string Msg = LocalizeFormat("Container", "Took back {} item(s).", Moved);
			pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Msg.c_str());
		}
		else if(NoSpace)
			pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("No inventory space!", "Item Pickup"));
		else
			pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("That container is empty.", "Container"));
	}
	// Store every inventory stack that still fits, then report the total.
	else if(str_comp(VoteStatus.m_aCmd, "CSTOREALL") == 0)
	{
		int Moved = 0;
		bool Full = false;
		const CItemSystem::CInventory &Inventory = pItems->GetInventory(ClientID);
		for(int i = 0; i < CItemSystem::CInventory::MAX_ITEMS; i++)
		{
			if(Inventory.IsEmpty(i))
				continue;
			const CItemID Id = Inventory.m_aItems[i].m_Id;
			const int Count = Inventory.m_aItems[i].m_Count;
			if(Count <= 0)
				continue;
			if(!pContainer->CanStore(Id))
			{
				Full = true;
				continue;
			}
			if(!pItems->RemoveItem(ClientID, Id, Count))
				continue;
			if(!pContainer->Store(Id, Count))
			{
				pItems->AddItem(ClientID, Id, Count, true);
				continue;
			}
			Moved += Count;
		}
		if(Moved > 0)
		{
			pGameServer->SavePlayerData(pPlayer);
			const std::string Msg = LocalizeFormat("Container", "Stored {} item(s).", Moved);
			pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Msg.c_str());
		}
		else if(Full)
			pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("The container is full.", "Container"));
		else
			pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("You have nothing to store.", "Container"));
	}

	// ----- page -----
	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	const char *pKindName = Localize(pContainer->Name(), "Container Name");
	pMenu->AddOption(LocalizeFormat("Menu Container", "{}: {} of {} slots used",
		pKindName, pContainer->UsedSlots(), pContainer->Capacity()).c_str(), "DISPLAY", "=");
	pMenu->AddHorizontalRule();

	for(int i = 0; i < pContainer->Capacity(); i++)
	{
		const CContainerEntity::SSlot &Slot = pContainer->Slot(i);
		if(Slot.m_Id.IsZero() || Slot.m_Count <= 0)
		{
			pMenu->AddOption(LocalizeFormat("Menu Container", "{}. (empty)", i + 1).c_str(), "DISPLAY", "-");
			continue;
		}
		char aCmd[32];
		str_format(aCmd, sizeof(aCmd), "CTAKE %d", i);
		pMenu->AddOption(LocalizeFormat("Menu Container", "{}. {} x{}",
			i + 1, pItems->GetName(Slot.m_Id), Slot.m_Count).c_str(), aCmd, "-");
	}
	pMenu->AddOption(Localize("Take everything", "Menu Container"), "CTAKEALL", "★");
	pMenu->AddOption(Localize("Click a container slot to take its stack back.", "Menu Container"), "DISPLAY", "-");
	pMenu->AddHorizontalRule();

	pMenu->AddOption(Localize("Click one of your items to store it.", "Menu Container"), "DISPLAY", "-");
	const CItemSystem::CInventory &Inventory = pItems->GetInventory(ClientID);
	bool HasItems = false;
	for(int i = 0; i < CItemSystem::CInventory::MAX_ITEMS; i++)
	{
		if(Inventory.IsEmpty(i))
			continue;
		HasItems = true;
		const CItemID &Id = Inventory.m_aItems[i].m_Id;
		char aId[UUID_MAXSTRSIZE];
		ItemCommand(Id, aId, sizeof(aId));
		char aCmd[VOTE_CMD_LENGTH];
		str_format(aCmd, sizeof(aCmd), "CSTORE %s", aId);
		pMenu->AddOption(LocalizeFormat("Menu Container", "   {} x{}",
			pItems->GetName(Id), Inventory.m_aItems[i].m_Count).c_str(), aCmd, "-");
	}
	if(!HasItems)
		pMenu->AddOption(Localize("Your inventory is empty.", "Menu Container"), "DISPLAY", "-");
	pMenu->AddOption(Localize("Store everything", "Menu Container"), "CSTOREALL", "★");
	pMenu->AddOption(Localize("Use a number as the vote reason to move that many.", "Menu Container"), "DISPLAY", "-");

	return true;
}
