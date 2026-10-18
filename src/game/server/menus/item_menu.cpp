/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/localization_format.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gamemenu.h>
#include <game/server/item.h>
#include <game/server/player.h>

#include "item_menu.h"

const char *ItemCommand(const CItemID &Id, char *pBuf, int Size)
{
	const char *pSource = Id.SourceName();
	if(pSource && pSource[0])
	{
		str_copy(pBuf, pSource, Size);
		return pBuf;
	}
	Id.FormatUuid(pBuf, Size);
	return pBuf;
}

// mercury chapter amnesia: whether an item/recipe is forgotten at a given
// forgetting level is decided by CItemSystem (IsItemForgotten /
// IsRecipeForgotten), so any system can ask the same question. here the menus
// only render the result: a forgotten item shows "???" (count and type stay
// visible), a forgotten recipe is simply not listed.

bool CGameMenu::MenuInventory(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	// the player clicked an item row: open its detail view instead of
	// printing a one-line hint
	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "DISPLAY") != 0 && str_comp(VoteStatus.m_aCmd, "NONE") != 0)
	{
		CItemSystem *pItem = pMenu->GameServer()->Item();
		CItemID Clicked;
		if(CItemID::Parse(VoteStatus.m_aCmd, &Clicked) && !Clicked.IsZero() &&
			pItem->GetItemCount(ClientID, Clicked) > 0)
		{
			pMenu->m_aItemViewId[ClientID] = Clicked;
			pMenu->SetPlayerPage(ClientID, "ITEMVIEW");
			return false;
		}
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	CItemSystem *pItem = pMenu->GameServer()->Item();
	const CItemSystem::CInventory &Inventory = pItem->GetInventory(ClientID);
	for(int i = 0; i < CItemSystem::CInventory::MAX_ITEMS; i++)
	{
		if(!Inventory.IsEmpty(i))
		{
			const CItemID &Id = Inventory.m_aItems[i].m_Id;
			// amnesia: with high forgetting the player no longer remembers
			// what this item is (name hidden, count still shown)
			const char *pName = pItem->GetName(Id);
			if(pItem->IsItemForgotten(pPlayer->m_Status.m_Forgetting, Id))
				pName = "???";
			char aCmd[UUID_MAXSTRSIZE];
			ItemCommand(Id, aCmd, sizeof(aCmd));
			const std::string Desc = LocalizeFormat("Menu Inventory", "{}. {}: {}",
				i + 1, pName, Inventory.m_aItems[i].m_Count);
			pMenu->AddOption(Desc.c_str(), aCmd, "-");
		}
		else
		{
			pMenu->AddOption(LocalizeFormat("Menu Inventory", "{}. None", i + 1).c_str(), "DISPLAY", "-");
		}
	}

	return true;
}

// data threaded through CItemSystem::ForEachItemType (item view menu): joins
// the localized type tags of the inspected item into one comma separated line
struct SItemTypeLineData
{
	char m_aTypes[VOTE_DESC_LENGTH * 4];
	int m_Len;
	int m_Count;
};

static void ItemTypeLineCallback(const char *pType, void *pUser)
{
	SItemTypeLineData *pData = static_cast<SItemTypeLineData *>(pUser);
	const char *pLocalized = Localize(pType, "Item Type");
	const int SepLen = pData->m_Count > 0 ? 2 : 0; // ", " between types
	const int AddLen = str_length(pLocalized) + SepLen;
	if(pData->m_Len + AddLen < (int) sizeof(pData->m_aTypes))
	{
		if(pData->m_Count > 0)
		{
			pData->m_aTypes[pData->m_Len++] = ',';
			pData->m_aTypes[pData->m_Len++] = ' ';
		}
		str_copy(pData->m_aTypes + pData->m_Len, pLocalized, sizeof(pData->m_aTypes) - pData->m_Len);
		pData->m_Len += str_length(pLocalized);
	}
	pData->m_Count++;
}

bool CGameMenu::MenuItemView(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	CItemSystem *pItem = pMenu->GameServer()->Item();
	const CItemID Id = pMenu->m_aItemViewId[ClientID];
	if(Id.IsZero() || pItem->GetItemCount(ClientID, Id) <= 0)
	{
		// the item is gone (used up / removed): go back to the inventory
		pMenu->SetPlayerPage(ClientID, "INVENTORY");
		return false;
	}

	const int Count = pItem->GetItemCount(ClientID, Id);
	// amnesia: a forgotten item is only shown as "???"
	const bool Forgotten = pItem->IsItemForgotten(pPlayer->m_Status.m_Forgetting, Id);
	const char *pName = Forgotten ? "???" : pItem->GetName(Id);
	const char *pLocalizedName = Localize(pName, "Item Name");

	// place a drill as a building (the item is consumed by PlaceDrill)
	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "PLACE") == 0)
	{
		pMenu->GameServer()->PlaceDrill(ClientID, Id);
		pMenu->SetPlayerPage(ClientID, "INVENTORY");
		return false;
	}

	// "use" the item; the vote reason box can carry how many to use
	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "USE") == 0)
	{
		int UseCount = 1;
		if(VoteStatus.m_aReason[0])
		{
			UseCount = str_toint(VoteStatus.m_aReason);
			if(UseCount < 1)
				UseCount = 1;
		}
		const int Used = pItem->UseItem(ClientID, Id, UseCount);
		if(Used > 0)
		{
			const std::string Msg = LocalizeFormat("Menu Inventory", "Used: {} x{}", pLocalizedName, Used);
			pMenu->GameServer()->SendChat(-1, CHAT_WHISPER, ClientID, Msg.c_str());
		}
		else
		{
			pMenu->GameServer()->SendChat(-1, CHAT_WHISPER, ClientID,
				Localize("Nothing to restore.", "Menu Inventory"));
		}
		// refresh: back to the list, or keep viewing if still owned
		pMenu->SetPlayerPage(ClientID, "INVENTORY");
		return false;
	}

	// "drop" the item (thrown as a CDroppedPickup); the vote reason box can
	// carry how many to drop
	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "DROP") == 0)
	{
		int DropCount = 1;
		if(VoteStatus.m_aReason[0])
		{
			DropCount = str_toint(VoteStatus.m_aReason);
			if(DropCount < 1)
				DropCount = 1;
		}

		// throw the item in the direction the player is currently looking
		vec2 Direction(0.0f, 0.0f);
		CCharacter *pChr = pPlayer->GetCharacter();
		if(pChr)
			Direction = pChr->AimDirection() * 12.0f;
		char aCmd[UUID_MAXSTRSIZE];
		ItemCommand(Id, aCmd, sizeof(aCmd));
		pMenu->GameServer()->DropItem(ClientID, aCmd, DropCount, Direction);

		// refresh: back to the list, or keep viewing if still owned
		pMenu->SetPlayerPage(ClientID, "INVENTORY");
		return false;
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	const std::string Header = std::format("{} x{}", pLocalizedName, Count);
	pMenu->AddOption(Header.c_str(), "DISPLAY", "=");

	// description (localized), wrapped over several rows. a forgotten item
	// (high forgetting) shows no description: the player can't recall it
	if(Forgotten)
	{
		pMenu->AddWrappedLinesOption(Localize("You can't remember what this is.", "Menu Inventory"));
	}
	else
	{
		const char *pDesc = pItem->GetDesc(Id);
		if(pDesc && pDesc[0])
		{
			// the context is keyed by the untranslated display name, even when
			// the player sees the localized one
			const char *pRawName = pItem->GetRawName(Id);
			char aCtx[128];
			str_format(aCtx, sizeof(aCtx), "Item Desc: %s", pRawName[0] ? pRawName : pName);
			pMenu->AddWrappedLinesOption(Localize(pDesc, aCtx));
		}
	}

	// item types (e.g. "weapon"), localized, listed below the description
	SItemTypeLineData TypeLine = {{'\0'}, 0, 0};
	pItem->ForEachItemType(Id, ItemTypeLineCallback, &TypeLine);
	if(TypeLine.m_Count > 0)
	{
		if(TypeLine.m_Count == 1)
			pMenu->AddWrappedLinesOption(LocalizeFormat("Menu Inventory", "Type: {}", (const char *) TypeLine.m_aTypes).c_str());
		else
			pMenu->AddWrappedLinesOption(LocalizeFormat("Menu Inventory", "Types: {}", (const char *) TypeLine.m_aTypes).c_str());
	}

	pMenu->AddHorizontalRule();

	// actions
	if(pItem->IsUsable(Id))
	{
		pMenu->AddOption(Localize("Use", "Menu Inventory"), "USE", "★");
		pMenu->AddOption(Localize("Use a number as the vote reason to use that many.", "Menu Inventory"), "DISPLAY", "-");
	}
	if(pItem->IsDrill(Id))
	{
		pMenu->AddOption(Localize("Place drill", "Menu Inventory"), "PLACE", "★");
		pMenu->AddOption(Localize("Hold the scoreboard next to it to control the drill. Jump to exit.", "Menu Inventory"), "DISPLAY", "-");
	}
	pMenu->AddOption(Localize("Drop", "Menu Inventory"), "DROP", "★");
	pMenu->AddOption(Localize("Use a number as the vote reason to drop that many.", "Menu Inventory"), "DISPLAY", "-");

	return true;
}

// -- crafting menu -------------------------------------------------------

// data threaded through CItemSystem::ForEachCraft
struct CCraftMenuData
{
	CItemSystem *m_pItem;
	CGameMenu *m_pMenu;
	int m_ClientID;
	int m_Forgetting;
	int m_Index;
	const char *m_pOpen; // the recipe id currently expanded ("" = none)
};

// renders one recipe as a menu row; its materials, byproducts and the "Craft"
// action only appear while the recipe is the expanded one
static void CraftListCallback(CItemSystem::SCraftDef &Craft, void *pUser)
{
	CCraftMenuData *pData = static_cast<CCraftMenuData *>(pUser);
	CGameMenu *pMenu = pData->m_pMenu;
	const int ClientID = pData->m_ClientID;

	// amnesia: a forgotten recipe is simply not listed anymore
	if(pData->m_pItem->IsRecipeForgotten(pData->m_Forgetting, Craft.m_aCraftId))
		return;

	pData->m_Index++;
	const bool Open = pData->m_pOpen[0] && str_comp(pData->m_pOpen, Craft.m_aCraftId) == 0;

	// the header names the recipe by its main output; the click toggles the
	// recipe open instead of crafting it right away. CItemID formats straight
	// to its (already localized) display name, so no GetName/Localize pair.
	std::string Head;
	if(Craft.m_NumGives > 0)
		Head = std::format("{}. {} x{}", pData->m_Index, Craft.m_aGives[0].m_Id, Craft.m_aGives[0].m_Count);
	else
		Head = std::format("{}. {}", pData->m_Index, Craft.m_aCraftId);
	char aExpand[64];
	str_format(aExpand, sizeof(aExpand), "CRAFTEXPAND %s", Craft.m_aCraftId);
	pMenu->AddOption(Head.c_str(), aExpand, Open ? "▾" : "▸");

	if(!Open)
		return;

	// byproducts: the first output is the header, the rest are listed here
	for(int g = 1; g < Craft.m_NumGives; g++)
	{
		const CItemSystem::SResult &Give = Craft.m_aGives[g];
		pMenu->AddOption(LocalizeFormat("Menu Craft", "   + {} x{}", Give.m_Id, Give.m_Count).c_str(), "DISPLAY", "-");
	}

	// the materials the recipe consumes / the tools it only needs to have
	for(int i = 0; i < Craft.m_NumNeeded; i++)
	{
		const CItemSystem::SIngredient &Need = Craft.m_aNeeded[i];
		const int Have = pData->m_pItem->GetIngredientCount(ClientID, Need);

		if(Need.m_IsTool)
		{
			// a tool must simply be owned (it is never consumed): mark
			// whether the player owns it (☑) or not (☒)
			const char *pMark = Have > 0 ? "☑" : "☒";
			if(Need.m_MatchByType)
				pMenu->AddOption(LocalizeFormat("Menu Craft", "   [tool] any {} {}", Localize(Need.m_aType, "Item Type"), pMark).c_str(), "DISPLAY", "-");
			else
				pMenu->AddOption(LocalizeFormat("Menu Craft", "   [tool] {} {}", Need.m_Id, pMark).c_str(), "DISPLAY", "-");
		}
		else
		{
			if(Need.m_MatchByType)
				pMenu->AddOption(LocalizeFormat("Menu Craft", "   any {} {}/{}", Localize(Need.m_aType, "Item Type"), Have, Need.m_Count).c_str(), "DISPLAY", "-");
			else
				pMenu->AddOption(std::format("   {} {}/{}", Need.m_Id, Have, Need.m_Count).c_str(), "DISPLAY", "-");
		}
	}

	// the action itself lives inside the expanded recipe only
	char aCraft[64];
	str_format(aCraft, sizeof(aCraft), "CRAFTNOW %s", Craft.m_aCraftId);
	pMenu->AddOption(Localize("Craft", "Menu Craft"), aCraft, "★");
}

bool CGameMenu::MenuCraft(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CItemSystem *pItem = pMenu->GameServer()->Item();

	// clicking a recipe folds its materials and its "Craft" action open or shut
	if(str_startswith(VoteStatus.m_aCmd, "CRAFTEXPAND "))
	{
		const char *pCraftId = VoteStatus.m_aCmd + str_length("CRAFTEXPAND ");
		if(str_comp(pMenu->m_aCraftOpen[ClientID], pCraftId) == 0)
			pMenu->m_aCraftOpen[ClientID][0] = '\0';
		else
			str_copy(pMenu->m_aCraftOpen[ClientID], pCraftId, sizeof(pMenu->m_aCraftOpen[ClientID]));
		// fall through and redraw the page with the new expansion
	}
	// the "Craft" action only exists inside an expanded recipe
	else if(str_startswith(VoteStatus.m_aCmd, "CRAFTNOW "))
	{
		const char *pCraftId = VoteStatus.m_aCmd + str_length("CRAFTNOW ");
		const CItemSystem::ECraftResult Result = pItem->Craft(ClientID, pCraftId);
		if(Result == CItemSystem::CRAFT_OK)
		{
			const CItemSystem::SCraftDef *pCraft = pItem->GetCraft(pCraftId);
			if(pCraft && pCraft->m_NumGives > 0)
			{
				const std::string Msg = LocalizeFormat("Menu Craft", "Crafted: {} x{}",
					pCraft->m_aGives[0].m_Id, pCraft->m_aGives[0].m_Count);
				pMenu->GameServer()->SendChat(-1, CHAT_WHISPER, ClientID, Msg.c_str());
			}
		}
		else if(Result == CItemSystem::CRAFT_NO_MATERIALS)
		{
			pMenu->GameServer()->SendChat(-1, CHAT_WHISPER, ClientID,
				Localize("Not enough materials.", "Menu Craft"));
		}
		else // CRAFT_NO_SPACE
		{
			pMenu->GameServer()->SendChat(-1, CHAT_WHISPER, ClientID,
				Localize("Not enough inventory space.", "Menu Craft"));
		}
		// fall through and redraw with the updated ingredient counts
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	if(pItem->m_Crafts.size() == 0)
	{
		pMenu->AddOption(Localize("No recipes available.", "Menu Craft"), "DISPLAY", "-");
		return true;
	}

	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	const int Forgetting = pPlayer ? pPlayer->m_Status.m_Forgetting : 0;

	// drop an expansion whose recipe no longer exists or has been forgotten
	if(pMenu->m_aCraftOpen[ClientID][0])
	{
		if(!pItem->GetCraft(pMenu->m_aCraftOpen[ClientID]) || pItem->IsRecipeForgotten(Forgetting, pMenu->m_aCraftOpen[ClientID]))
			pMenu->m_aCraftOpen[ClientID][0] = '\0';
	}

	pMenu->AddOption(Localize("Click a recipe to see how to craft it. Tools are not consumed.", "Menu Craft"), "DISPLAY", "-");
	pMenu->AddHorizontalRule();

	CCraftMenuData Data = {pItem, pMenu, ClientID, Forgetting, 0, pMenu->m_aCraftOpen[ClientID]};
	pItem->ForEachCraft(CraftListCallback, &Data);
	if(Data.m_Index == 0)
		pMenu->AddOption(Localize("You can't remember any crafting recipes.", "Menu Craft"), "DISPLAY", "-");

	return true;
}
