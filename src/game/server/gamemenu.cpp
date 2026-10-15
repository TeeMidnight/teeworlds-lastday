/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <engine/shared/config.h>
#include <engine/shared/memheap.h>

#include <game/localization_format.h>

#include "entities/character.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "gamemenu.h"
#include "item.h"
#include "player.h"
#include "trade.h"
#include "weapon.h"
#include "weaponmanager.h"

#include <cstdarg>
#include <cstdio>

CConfig *CGameMenu::Config() const { return GameServer()->Config(); }
IServer *CGameMenu::Server() const { return GameServer()->Server(); }

// A menu option command has to be a stable string; the namespaced source name is
// readable in logs, the canonical uuid is the fallback for uuid-only items.
static const char *ItemCommand(const CItemID &Id, char *pBuf, int Size)
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

CGameMenu::CGameMenu(CGameContext *pGameServer) :
	m_pGameServer(pGameServer)
{
	Init(pGameServer);
}

CGameMenu::~CGameMenu()
{
	delete m_pServerVoteOptionHeap;
}

void CGameMenu::Init(CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
	m_CurrentClientID = -1;

	// reset the vote state and allocate the server vote option list
	m_VoteCreator = -1;
	m_VoteType = -1;
	m_VoteCloseTime = 0;
	m_VoteCancelTime = 0;
	m_VoteUpdate = false;
	m_VotePos = 0;
	m_VoteClientID = -1;
	m_VoteEnforce = VOTE_CHOICE_PASS;
	m_aVoteDescription[0] = '\0';
	m_aVoteCommand[0] = '\0';
	m_aVoteReason[0] = '\0';
	m_pServerVoteOptionHeap = new CHeap();
	m_pServerVoteOptionFirst = nullptr;
	m_pServerVoteOptionLast = nullptr;
	m_NumServerVoteOptions = 0;

	for(auto &Data : m_aPlayerData)
	{
		Data.Reset();
	}

	m_MenuPages.clear();
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		m_aLoadoutSlot[i] = -1;
		m_aItemViewId[i] = CItemID();
	}

	Register("MAIN", "Main Menu", MenuMain, nullptr); // Localize("Main Menu", "MAIN")
	Register("INVENTORY", "Inventory", MenuInventory, nullptr); // Localize("Inventory", "INVENTORY")
	Register("ITEMVIEW", "Item", MenuItemView, nullptr, "INVENTORY"); // Localize("Item", "ITEMVIEW")
	Register("CRAFT", "Craft", MenuCraft, nullptr); // Localize("Craft", "CRAFT")
	Register("LOADOUT", "Loadout", MenuLoadout, nullptr); // Localize("Loadout", "LOADOUT")
	Register("WEAPONPICK", "Choose Weapon", MenuWeaponPick, nullptr, "LOADOUT"); // Localize("Choose Weapon", "WEAPONPICK")
	Register("TRADE", "Player Shops", MenuTrade, nullptr); // Localize("Player Shops", "TRADE")
	Register("VOTE", "Server Vote", MenuVote, nullptr); // Localize("Server Vote", "VOTE")
}

void CGameMenu::Register(const char *pPageName, const char *pTitle, FMenuCallback pfnFunc, void *pUser, const char *pParent)
{
	dbg_assert(pPageName && pPageName[0], "Page must have a name");
	dbg_assert(pTitle && pTitle[0], "Page must have a title");

	CMenuPage Page;
	Page.m_pfnCallback = pfnFunc;
	Page.m_pUserData = pUser;
	Page.m_Hash = str_quickhash(pPageName);
	Page.m_ParentHash = str_quickhash(pParent);
	str_copy(Page.m_aTitle, pTitle, sizeof(Page.m_aTitle));
	str_copy(Page.m_aContext, pPageName, sizeof(Page.m_aContext));
	m_MenuPages.set(Page.m_Hash, Page);
}

void CGameMenu::OnClientEntered(int ClientID)
{
	m_aPlayerData[ClientID].Reset(true);
	SetPlayerPage(ClientID, "MAIN");
	// the voter set changed
	m_VoteUpdate = true;
}

void CGameMenu::OnMenuVote(int ClientID, CCallVoteStatus &VoteStatus, bool Sound)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;

	if(VoteStatus.m_Force && !Server()->IsAuthed(ClientID))
		return;

	m_CurrentClientID = ClientID;

	unsigned &CurrentPage = m_aPlayerData[ClientID].m_CurrentPage;
	if(!m_MenuPages.get(CurrentPage))
	{
		CurrentPage = MENU_MAIN_PAGE_ID;
		VoteStatus.m_aDesc[0] = '\0';
		VoteStatus.m_aReason[0] = '\0';
	}

	// find command
	str_copy(VoteStatus.m_aCmd, "DISPLAY", sizeof(VoteStatus.m_aCmd));
	bool FoundOption = false;
	if(VoteStatus.m_aDesc[0])
	{
		for(CVoteOptionServer *pOption = m_aPlayerData[ClientID].m_pVoteOptionFirst; pOption; pOption = pOption->m_pNext)
		{
			if(str_comp_nocase(VoteStatus.m_aDesc, pOption->m_aDescription) == 0)
			{
				str_format(VoteStatus.m_aCmd, sizeof(VoteStatus.m_aCmd), "%s", pOption->m_aCommand);
				FoundOption = true;
				break;
			}
		}
		// the clicked option does not belong to the menu, let the caller
		// handle it (e.g. a regular server vote option)
		if(!FoundOption)
			return;
	}

	if(Sound)
		GameServer()->SendSoundTarget(ClientID, SOUND_WEAPON_NOAMMO);

	if(str_comp(VoteStatus.m_aCmd, "PREPAGE") == 0)
	{
		SetPlayerPage(ClientID, m_MenuPages.get(CurrentPage)->m_ParentHash);
		return;
	}
	else if(str_comp(VoteStatus.m_aCmd, "NONE") == 0)
	{
		return;
	}

	// call back
	CMenuPage *pPage = m_MenuPages.get(CurrentPage);
	if(!pPage->m_pfnCallback(ClientID, VoteStatus, this, pPage->m_pUserData))
		return;
	// add back page
	if(CurrentPage != MENU_MAIN_PAGE_ID)
	{
		AddHorizontalRule();
		AddOption(Localize("Previous Page", "Menu"), "PREPAGE", "=");
	}

	CVoteOptionServer *pCurrent = m_aPlayerData[ClientID].m_pVoteOptionFirst;
	while(pCurrent)
	{
		// count options for actual packet
		int NumOptions = 0;
		for(CVoteOptionServer *p = pCurrent; p && NumOptions < MAX_VOTE_OPTION_ADD; p = p->m_pNext, ++NumOptions)
			;

		// pack and send vote list packet
		CMsgPacker Msg(NETMSGTYPE_SV_VOTEOPTIONLISTADD);
		Msg.AddInt(NumOptions);
		while(pCurrent && NumOptions--)
		{
			Msg.AddString(pCurrent->m_aDescription, VOTE_DESC_LENGTH);
			pCurrent = pCurrent->m_pNext;
		}
		Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
	}
}

void CGameMenu::SendMenuChat(int ClientID, const char *pChat)
{
	if(ClientID == -1)
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(Server()->ClientIngame(i))
				SendMenuChat(i, pChat);
		}
		return;
	}

	str_copy(m_aPlayerData[ClientID].m_aMenuChat, pChat, sizeof(m_aPlayerData[ClientID].m_aMenuChat));
	CCallVoteStatus VoteStatus;
	OnMenuVote(ClientID, VoteStatus);
}

void CGameMenu::ClearOptions(int ClientID)
{
	SendVoteClearOptions(ClientID);

	m_aPlayerData[ClientID].m_pVoteOptionHeap->Reset();
	m_aPlayerData[ClientID].m_pVoteOptionFirst = nullptr;
	m_aPlayerData[ClientID].m_pVoteOptionLast = nullptr;
	m_aPlayerData[ClientID].m_NumVoteOptions = 0;
}

void CGameMenu::RefreshMenu(int ClientID)
{
	SetPlayerPage(ClientID, m_aPlayerData[ClientID].m_CurrentPage);
}

void CGameMenu::SetPlayerPage(int ClientID, unsigned Page)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;
	m_aPlayerData[ClientID].m_CurrentPage = Page;
	CCallVoteStatus VoteStatus;
	OnMenuVote(ClientID, VoteStatus);
}

void CGameMenu::SetPlayerPage(int ClientID, const char *pPage)
{
	if(!pPage || !pPage[0])
		return;
	SetPlayerPage(ClientID, str_quickhash(pPage));
}
// static
bool CGameMenu::MenuMain(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	// refresh
	if(VoteStatus.m_aCmd[0])
	{
		if(str_comp(VoteStatus.m_aCmd, "PAGE INVENTORY") == 0)
		{
			pMenu->SetPlayerPage(ClientID, "INVENTORY");
			return false;
		}
		else if(str_comp(VoteStatus.m_aCmd, "PAGE VOTE") == 0)
		{
			pMenu->SetPlayerPage(ClientID, "VOTE");
			return false;
		}
		else if(str_comp(VoteStatus.m_aCmd, "PAGE CRAFT") == 0)
		{
			pMenu->SetPlayerPage(ClientID, "CRAFT");
			return false;
		}
		else if(str_comp(VoteStatus.m_aCmd, "PAGE LOADOUT") == 0)
		{
			pMenu->SetPlayerPage(ClientID, "LOADOUT");
			return false;
		}
		else if(str_comp(VoteStatus.m_aCmd, "PAGE TRADE") == 0)
		{
			pMenu->SetPlayerPage(ClientID, "TRADE");
			return false;
		}
		else if(str_comp(VoteStatus.m_aCmd, "HIDDEN") == 0)
		{
			pMenu->GameServer()->m_apPlayers[ClientID]->m_Status.m_HideTip = true;
		}
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();
	// TIP
	if(!pMenu->GameServer()->m_apPlayers[ClientID]->m_Status.m_HideTip)
	{
		pMenu->AddOption(Localize("If you don't want to close menu when you use a option,", "Menu Main"), "DISPLAY");
		pMenu->AddOption(Localize("then you can input this in your console:", "Menu Main"), "DISPLAY");
		pMenu->AddOption("ui_close_window_after_changing_setting 0", "DISPLAY");

		pMenu->AddOption(Localize("(Click this to hide this tip)", "Menu Main"), "HIDDEN");

		pMenu->AddHorizontalRule();
	}
	// player stats
	{
		CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
		pMenu->AddOptionFormat(Localize("Name: %s", "Menu Main"), "DISPLAY", "-", pMenu->Server()->ClientName(ClientID));
		if(pPlayer)
		{
			pMenu->AddOptionFormat(Localize("Level: %d", "Menu Main"), "DISPLAY", "-", pPlayer->m_Status.m_Level);
			// mercury chapter: the forgetting meter (0 = clear mind)
			pMenu->AddOptionFormat(Localize("Forgetting: %d", "Menu Main"), "DISPLAY", "-", pPlayer->m_Status.m_Forgetting);
		}
	}
	pMenu->AddHorizontalRule();
	// options
	{
		pMenu->AddOption(Localize("Inventory", "Menu Main"), "PAGE INVENTORY", "★");
		pMenu->AddOption(Localize("Craft", "Menu Main"), "PAGE CRAFT", "★");
		pMenu->AddOption(Localize("Loadout", "Menu Main"), "PAGE LOADOUT", "★");
		pMenu->AddOption(Localize("Player Shops", "Menu Main"), "PAGE TRADE", "★");
		pMenu->AddOption(Localize("Server Vote", "Menu Main"), "PAGE VOTE", "★");
	}

	return true;
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
			pMenu->AddOptionFormat(Localize("%d. None", "Menu Inventory"), "DISPLAY", "-", i + 1);
		}
	}

	return true;
}

void CGameMenu::AddWrappedLinesOption(const char *pText)
{
	const int MaxCharsPerLine = (VOTE_DESC_LENGTH - 2) / 2; // conservative for CJK
	const char *pCursor = pText;
	while(pCursor && pCursor[0])
	{
		char aLine[VOTE_DESC_LENGTH];
		str_utf8_copy_num(aLine, pCursor, sizeof(aLine), MaxCharsPerLine);
		if(!aLine[0])
			break;
		AddOption(aLine, "DISPLAY");
		const int Advance = str_length(aLine);
		if(Advance <= 0)
			break;
		pCursor += Advance;
	}
}

void CGameMenu::AddWrappedLinesOptionFormat(const char *pDesc, ...)
{
	va_list List;
	va_start(List, pDesc);
	char aBuf[VOTE_DESC_LENGTH * 4];
	vsnprintf(aBuf, sizeof(aBuf), pDesc, List);
	va_end(List);
	AddWrappedLinesOption(aBuf);
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

	char aHeader[VOTE_DESC_LENGTH];
	str_format(aHeader, sizeof(aHeader), "%s x%d", pLocalizedName, Count);
	pMenu->AddOption(aHeader, "DISPLAY", "=");

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
			char aCtx[128];
			str_format(aCtx, sizeof(aCtx), "Item Desc: %s", pName);
			pMenu->AddWrappedLinesOption(Localize(pDesc, aCtx));
		}
	}

	// item types (e.g. "weapon"), localized, listed below the description
	SItemTypeLineData TypeLine = {{'\0'}, 0, 0};
	pItem->ForEachItemType(Id, ItemTypeLineCallback, &TypeLine);
	if(TypeLine.m_Count > 0)
	{
		char aTypeRow[VOTE_DESC_LENGTH * 4];
		if(TypeLine.m_Count == 1)
			str_format(aTypeRow, sizeof(aTypeRow), Localize("Type: %s", "Menu Inventory"), TypeLine.m_aTypes);
		else
			str_format(aTypeRow, sizeof(aTypeRow), Localize("Types: %s", "Menu Inventory"), TypeLine.m_aTypes);
		pMenu->AddWrappedLinesOption(aTypeRow);
	}

	pMenu->AddHorizontalRule();

	// actions
	if(pItem->IsUsable(Id))
	{
		pMenu->AddOption(Localize("Use", "Menu Inventory"), "USE", "★");
		pMenu->AddOption(Localize("Use a number as the vote reason to use that many.", "Menu Inventory"), "DISPLAY", "-");
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
};

// renders one recipe as a block of menu rows (also used only from MenuCraft)
static void CraftListCallback(CItemSystem::SCraftDef &Craft, void *pUser)
{
	CCraftMenuData *pData = static_cast<CCraftMenuData *>(pUser);
	CGameMenu *pMenu = pData->m_pMenu;
	const int ClientID = pData->m_ClientID;

	// amnesia: a forgotten recipe is simply not listed anymore
	if(pData->m_pItem->IsRecipeForgotten(pData->m_Forgetting, Craft.m_aCraftId))
		return;

	pData->m_Index++;
	// one row per output; a recipe may have more than one
	for(int g = 0; g < Craft.m_NumGives; g++)
	{
		const CItemSystem::SResult &Give = Craft.m_aGives[g];
		const char *pResultName = pData->m_pItem->GetName(Give.m_Id);
		if(g == 0)
			pMenu->AddOptionFormat("%d. %s x%d", Craft.m_aCraftId, "→",
				pData->m_Index, Localize(pResultName, "Item Name"), Give.m_Count);
		else
			pMenu->AddOptionFormat(Localize("   + %s x%d", "Menu Craft"), "DISPLAY", "-",
				Localize(pResultName, "Item Name"), Give.m_Count);
	}

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
				pMenu->AddOptionFormat(Localize("   [tool] any %s %s", "Menu Craft"), "DISPLAY", "-",
					Localize(Need.m_aType, "Item Type"), pMark);
			else
				pMenu->AddOptionFormat(Localize("   [tool] %s %s", "Menu Craft"), "DISPLAY", "-",
					Localize(pData->m_pItem->GetName(Need.m_Id), "Item Name"), pMark);
		}
		else
		{
			if(Need.m_MatchByType)
				pMenu->AddOptionFormat(Localize("   any %s %d/%d", "Menu Craft"), "DISPLAY", "-",
					Localize(Need.m_aType, "Item Type"), Have, Need.m_Count);
			else
				pMenu->AddOptionFormat("   %s %d/%d", "DISPLAY", "-",
					Localize(pData->m_pItem->GetName(Need.m_Id), "Item Name"), Have, Need.m_Count);
		}
	}
}

bool CGameMenu::MenuCraft(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CItemSystem *pItem = pMenu->GameServer()->Item();

	// if the player clicked a recipe, try to craft it
	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "DISPLAY") != 0 && str_comp(VoteStatus.m_aCmd, "NONE") != 0)
	{
		const CItemSystem::ECraftResult Result = pItem->Craft(ClientID, VoteStatus.m_aCmd);
		if(Result == CItemSystem::CRAFT_OK)
		{
			const CItemSystem::SCraftDef *pCraft = pItem->GetCraft(VoteStatus.m_aCmd);
			if(pCraft)
			{
				char aMsg[128];
				str_format(aMsg, sizeof(aMsg), Localize("Crafted: %s x%d", "Menu Craft"),
					Localize(pItem->GetName(pCraft->m_aGives[0].m_Id), "Item Name"), pCraft->m_aGives[0].m_Count);
				pMenu->GameServer()->SendChat(-1, CHAT_WHISPER, ClientID, aMsg);
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
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	if(pItem->m_Crafts.size() == 0)
	{
		pMenu->AddOption(Localize("No recipes available.", "Menu Craft"), "DISPLAY", "-");
		return true;
	}

	pMenu->AddOption(Localize("Click a recipe to craft it. Tools are not consumed.", "Menu Craft"), "DISPLAY", "-");
	pMenu->AddHorizontalRule();

	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	const int Forgetting = pPlayer ? pPlayer->m_Status.m_Forgetting : 0;
	CCraftMenuData Data = {pItem, pMenu, ClientID, Forgetting, 0};
	pItem->ForEachCraft(CraftListCallback, &Data);
	if(Data.m_Index == 0)
		pMenu->AddOption(Localize("You can't remember any crafting recipes.", "Menu Craft"), "DISPLAY", "-");

	return true;
}

void CGameMenu::AddPageTitle()
{
	if(m_CurrentClientID < 0 || m_CurrentClientID >= MAX_CLIENTS)
		return;
	CMenuPage *pPage = m_MenuPages.get(m_aPlayerData[m_CurrentClientID].m_CurrentPage);
	if(!pPage)
		return;

	AddOption("===============================", "NONE");
	AddOption(Localize(pPage->m_aTitle, pPage->m_aContext), "NONE", "=");
	AddOption("===============================", "NONE");
}

void CGameMenu::AddSpace()
{
	AddOption(" ", "NONE");
}

void CGameMenu::AddHorizontalRule()
{
	AddOption("---------------------------------", "NONE");
}

void CGameMenu::AddOption(const char *pDesc, const char *pCommand, const char *pPrefix)
{
	if(m_CurrentClientID < 0 || m_CurrentClientID >= MAX_CLIENTS)
		return;
	if(!pDesc || !pCommand)
		return;
	if(!pDesc[0] || !pCommand[0])
		return;
	// add the option
	++m_aPlayerData[m_CurrentClientID].m_NumVoteOptions;
	int Len = str_length(pCommand);

	CVoteOptionServer *pOption = (CVoteOptionServer *) m_aPlayerData[m_CurrentClientID].m_pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len);
	pOption->m_pNext = 0;
	pOption->m_pPrev = m_aPlayerData[m_CurrentClientID].m_pVoteOptionLast;
	if(pOption->m_pPrev)
		pOption->m_pPrev->m_pNext = pOption;
	m_aPlayerData[m_CurrentClientID].m_pVoteOptionLast = pOption;
	if(!m_aPlayerData[m_CurrentClientID].m_pVoteOptionFirst)
		m_aPlayerData[m_CurrentClientID].m_pVoteOptionFirst = pOption;

	if(pPrefix && pPrefix[0])
		str_format(pOption->m_aDescription, sizeof(pOption->m_aDescription), "%s %s", pPrefix, pDesc);
	else
		str_copy(pOption->m_aDescription, pDesc, sizeof(pOption->m_aDescription));
	mem_copy(pOption->m_aCommand, pCommand, Len + 1);
}

void CGameMenu::AddOptionFormat(const char *pDesc, const char *pCommand, const char *pPrefix, ...)
{
	va_list List;
	va_start(List, pPrefix);
	char aBuf[VOTE_DESC_LENGTH];
	vsnprintf(aBuf, sizeof(aBuf), pDesc, List);
	va_end(List);
	AddOption(aBuf, pCommand, pPrefix);
}

CGameMenu::CPlayerData::CPlayerData()
{
	m_pVoteOptionHeap = new CHeap();
	Reset(true);
}

CGameMenu::CPlayerData::~CPlayerData()
{
	delete m_pVoteOptionHeap;
}

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

		char aDesc[VOTE_DESC_LENGTH];
		if(pRawName && pRawName[0])
		{
			const char *pName = Localize(pRawName, "Item Name");
			if(pWeapon)
				str_format(aDesc, sizeof(aDesc), "%d. %s", i + 1, pName);
			else
				// an item without a corresponding weapon (e.g. a resource):
				// falls back to the hand
				str_format(aDesc, sizeof(aDesc), Localize("%d. %s (hand)", "Menu Loadout"), i + 1, pName);
		}
		else if(!Item.IsZero())
			str_format(aDesc, sizeof(aDesc), Localize("%d. (hand)", "Menu Loadout"), i + 1);
		else
			str_format(aDesc, sizeof(aDesc), Localize("%d. (empty)", "Menu Loadout"), i + 1);

		char aCmd[16];
		str_format(aCmd, sizeof(aCmd), "PICK %d", i);
		pMenu->AddOption(aDesc, aCmd);
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

bool CGameMenu::MenuTrade(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	// clicking a listing buys it. The command is "BUY <id>".
	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "NONE") != 0 &&
		str_comp(VoteStatus.m_aCmd, "DISPLAY") != 0 && str_startswith(VoteStatus.m_aCmd, "BUY "))
	{
		const int ListingId = str_toint(VoteStatus.m_aCmd + 4);
		pMenu->GameServer()->Trade()->Buy(ClientID, ListingId);
		pMenu->SetPlayerPage(ClientID, "TRADE");
		return false;
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();
	pMenu->AddOption(Localize("Click a listing to buy it.", "Menu Trade"), "DISPLAY", "-");
	pMenu->AddOption(Localize("List your own goods with: /sell <item> <count> <price item> <price count>", "Menu Trade"), "DISPLAY", "-");
	pMenu->AddHorizontalRule();

	CTradeSystem *pTrade = pMenu->GameServer()->Trade();
	if(!pTrade || pTrade->NumListings() == 0)
	{
		pMenu->AddOption(Localize("No goods are for sale.", "Menu Trade"), "DISPLAY", "-");
		return true;
	}

	for(int i = 0; i < pTrade->NumListings(); i++)
	{
		const CTradeSystem::SListing *pListing = pTrade->ListingAt(i);
		if(!pListing)
			continue;
		const std::string Desc = LocalizeFormat("Menu Trade", "#{} {}: {} x{} for {} x{}",
			pListing->m_Id, pMenu->Server()->ClientName(pListing->m_Seller),
			pListing->m_OfferItem, pListing->m_OfferCount, pListing->m_PriceItem, pListing->m_PriceCount);
		char aCmd[32];
		str_format(aCmd, sizeof(aCmd), "BUY %d", pListing->m_Id);
		pMenu->AddOption(Desc.c_str(), aCmd, "$");
	}

	return true;
}

// -- server vote ---------------------------------------------------------
//
// The menu and the server vote share one protocol: the vote option list packet
// carries both the menu pages and the server's vote options. Keeping the vote
// state here means the two cannot drift apart.

void CGameMenu::StartVote(const char *pDesc, const char *pCommand, const char *pReason)
{
	// check if a vote is already running
	if(m_VoteCloseTime)
		return;

	// reset votes
	m_VoteEnforce = VOTE_CHOICE_PASS;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[i];
		if(pPlayer)
		{
			pPlayer->m_Vote = VOTE_CHOICE_PASS;
			pPlayer->m_VotePos = 0;
		}
	}

	// start vote
	m_VoteCloseTime = time_get() + time_freq() * VOTE_TIME;
	m_VoteCancelTime = time_get() + time_freq() * VOTE_CANCEL_TIME;
	str_copy(m_aVoteDescription, pDesc, sizeof(m_aVoteDescription));
	str_copy(m_aVoteCommand, pCommand, sizeof(m_aVoteCommand));
	str_copy(m_aVoteReason, pReason, sizeof(m_aVoteReason));
	SendVoteSet(m_VoteType, -1);
	m_VoteUpdate = true;
}

void CGameMenu::EndVote(int Type, bool Force)
{
	m_VoteCloseTime = 0;
	m_VoteCancelTime = 0;
	if(Force)
		m_VoteCreator = -1;
	SendVoteSet(Type, -1);
}

void CGameMenu::SendForceVote(int Type, const char *pDescription, const char *pReason)
{
	CNetMsg_Sv_VoteSet Msg;
	Msg.m_Type = Type;
	Msg.m_Timeout = 0;
	Msg.m_ClientID = -1;
	Msg.m_pDescription = pDescription;
	Msg.m_pReason = pReason;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

void CGameMenu::SendVoteSet(int Type, int ToClientID)
{
	CNetMsg_Sv_VoteSet Msg;
	if(m_VoteCloseTime)
	{
		Msg.m_ClientID = m_VoteCreator;
		Msg.m_Type = Type;
		Msg.m_Timeout = (m_VoteCloseTime - time_get()) / time_freq();
		Msg.m_pDescription = m_aVoteDescription;
		Msg.m_pReason = m_aVoteReason;
	}
	else
	{
		Msg.m_Type = Type;
		Msg.m_Timeout = 0;
		Msg.m_ClientID = m_VoteCreator;
		Msg.m_pDescription = "";
		Msg.m_pReason = "";
	}
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ToClientID);
}

void CGameMenu::SendVoteStatus(int ClientID, int Total, int Yes, int No)
{
	CNetMsg_Sv_VoteStatus Msg = {0};
	Msg.m_Total = Total;
	Msg.m_Yes = Yes;
	Msg.m_No = No;
	Msg.m_Pass = Total - (Yes + No);

	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameMenu::SendVoteClearOptions(int ClientID)
{
	CNetMsg_Sv_VoteClearOptions ClearMsg;
	Server()->SendPackMsg(&ClearMsg, MSGFLAG_VITAL, ClientID);
}

void CGameMenu::SendActiveVote(int ClientID)
{
	if(m_VoteCloseTime)
		SendVoteSet(m_VoteType, ClientID);
}

void CGameMenu::AbortVoteOnDisconnect(int ClientID)
{
	if(m_VoteCloseTime && ClientID == m_VoteClientID && (str_startswith(m_aVoteCommand, "kick ") || str_startswith(m_aVoteCommand, "set_team ") || (str_startswith(m_aVoteCommand, "ban ") && Server()->IsBanned(ClientID))))
		m_VoteCloseTime = -1;
}

void CGameMenu::AbortVoteOnTeamChange(int ClientID)
{
	if(m_VoteCloseTime && ClientID == m_VoteClientID && str_startswith(m_aVoteCommand, "set_team "))
		m_VoteCloseTime = -1;
}

void CGameMenu::TickVote()
{
	if(!m_VoteCloseTime)
		return;

	// abort the kick-vote on player-leave
	if(m_VoteCloseTime == -1)
		EndVote(VOTE_END_ABORT, false);
	else
	{
		int Total = 0, Yes = 0, No = 0;
		if(m_VoteUpdate)
		{
			// count votes
			char aaBuf[MAX_CLIENTS][NETADDR_MAXSTRSIZE] = {{0}};
			for(int i = 0; i < MAX_CLIENTS; i++)
				if(GameServer()->m_apPlayers[i])
					Server()->GetClientAddr(i, aaBuf[i], NETADDR_MAXSTRSIZE);
			bool aVoteChecked[MAX_CLIENTS] = {0};
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				CPlayer *pPlayer = GameServer()->m_apPlayers[i];
				if(!pPlayer || (pPlayer->GetTeam() == TEAM_SPECTATORS && !Config()->m_SvAllowSpecVoting) || aVoteChecked[i]) // don't count in votes by spectators
					continue;

				int ActVote = pPlayer->m_Vote;
				int ActVotePos = pPlayer->m_VotePos;

				// check for more players with the same ip (only use the vote of the one who voted first)
				for(int j = i + 1; j < MAX_CLIENTS; ++j)
				{
					CPlayer *pOther = GameServer()->m_apPlayers[j];
					if(!pOther || aVoteChecked[j] || str_comp(aaBuf[j], aaBuf[i]))
						continue;

					aVoteChecked[j] = true;
					if(pOther->m_Vote && (!ActVote || ActVotePos > pOther->m_VotePos))
					{
						ActVote = pOther->m_Vote;
						ActVotePos = pOther->m_VotePos;
					}
				}

				Total++;
				if(ActVote > 0)
					Yes++;
				else if(ActVote < 0)
					No++;
			}
		}

		if(m_VoteEnforce == VOTE_CHOICE_YES || (m_VoteUpdate && Yes >= Total / 2 + 1))
		{
			Server()->SetRconCID(IServer::RCON_CID_VOTE);
			GameServer()->Console()->ExecuteLine(m_aVoteCommand);
			Server()->SetRconCID(IServer::RCON_CID_SERV);
			if(m_VoteCreator != -1 && GameServer()->m_apPlayers[m_VoteCreator])
				GameServer()->m_apPlayers[m_VoteCreator]->m_LastVoteCallTick = 0;

			EndVote(VOTE_END_PASS, m_VoteEnforce == VOTE_CHOICE_YES);
		}
		else if(m_VoteEnforce == VOTE_CHOICE_NO || (m_VoteUpdate && No >= (Total + 1) / 2) || time_get() > m_VoteCloseTime)
			EndVote(VOTE_END_FAIL, m_VoteEnforce == VOTE_CHOICE_NO);
		else if(m_VoteUpdate)
		{
			m_VoteUpdate = false;
			SendVoteStatus(-1, Total, Yes, No);
		}
	}
}

void CGameMenu::OnCallVote(int ClientID, CNetMsg_Cl_CallVote *pMsg)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return;
	int64 Now = Server()->Tick();

	if(str_comp_nocase(pMsg->m_Type, "option") == 0)
	{
		CCallVoteStatus VoteStatus;
		str_format(VoteStatus.m_aDesc, sizeof(VoteStatus.m_aDesc), "%s", pMsg->m_Value);
		str_format(VoteStatus.m_aReason, sizeof(VoteStatus.m_aReason), "%s", pMsg->m_Reason);
		VoteStatus.m_Force = pMsg->m_Force;

		OnMenuVote(ClientID, VoteStatus, true);
		return;
	}

	if(pMsg->m_Force)
	{
		if(!Server()->IsAuthed(ClientID))
			return;
	}
	else
	{
		if((Config()->m_SvSpamprotection && ((pPlayer->m_LastVoteTryTick && pPlayer->m_LastVoteTryTick + Server()->TickSpeed() * 3 > Now) ||
							    (pPlayer->m_LastVoteCallTick && pPlayer->m_LastVoteCallTick + Server()->TickSpeed() * VOTE_COOLDOWN > Now))) ||
			(pPlayer->GetTeam() == TEAM_SPECTATORS && !Config()->m_SvAllowSpecVoting) || m_VoteCloseTime)
			return;

		pPlayer->m_LastVoteTryTick = Now;
	}

	// 0.8: VOTE_UNKNOWN was removed from the enum; the reference uses
	// -1 as the "no vote decided yet" sentinel.
	m_VoteType = -1;
	char aDesc[VOTE_DESC_LENGTH] = {0};
	char aCmd[VOTE_CMD_LENGTH] = {0};
	const char *pReason = pMsg->m_Reason[0] ? pMsg->m_Reason : "No reason given";

	if(str_comp_nocase(pMsg->m_Type, "kick") == 0)
	{
		if(!Config()->m_SvVoteKick || GameServer()->m_pController->GetRealPlayerNum() < Config()->m_SvVoteKickMin)
			return;

		int KickID = str_toint(pMsg->m_Value);
		if(KickID < 0 || KickID >= MAX_CLIENTS || !GameServer()->m_apPlayers[KickID] || KickID == ClientID || Server()->IsAuthed(KickID))
			return;

		str_format(aDesc, sizeof(aDesc), "%2d: %s", KickID, Server()->ClientName(KickID));
		if(!Config()->m_SvVoteKickBantime)
			str_format(aCmd, sizeof(aCmd), "kick %d Kicked by vote", KickID);
		else
		{
			char aAddrStr[NETADDR_MAXSTRSIZE] = {0};
			Server()->GetClientAddr(KickID, aAddrStr, sizeof(aAddrStr));
			str_format(aCmd, sizeof(aCmd), "ban %s %d Banned by vote", aAddrStr, Config()->m_SvVoteKickBantime);
		}
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf),
			"'%d:%s' voted %s '%d:%s' reason='%s' cmd='%s' force=%d",
			ClientID, Server()->ClientName(ClientID), pMsg->m_Type,
			KickID, Server()->ClientName(KickID), pReason, aCmd, pMsg->m_Force);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		if(pMsg->m_Force)
		{
			Server()->SetRconCID(ClientID);
			GameServer()->Console()->ExecuteLine(aCmd);
			Server()->SetRconCID(IServer::RCON_CID_SERV);
			return;
		}
		m_VoteType = VOTE_START_KICK;
		m_VoteClientID = KickID;
	}
	else if(str_comp_nocase(pMsg->m_Type, "spectate") == 0)
	{
		if(!Config()->m_SvVoteSpectate)
			return;

		int SpectateID = str_toint(pMsg->m_Value);
		if(SpectateID < 0 || SpectateID >= MAX_CLIENTS || !GameServer()->m_apPlayers[SpectateID] || GameServer()->m_apPlayers[SpectateID]->GetTeam() == TEAM_SPECTATORS || SpectateID == ClientID)
			return;

		str_format(aDesc, sizeof(aDesc), "%2d: %s", SpectateID, Server()->ClientName(SpectateID));
		str_format(aCmd, sizeof(aCmd), "set_team %d -1 %d", SpectateID, Config()->m_SvVoteSpectateRejoindelay);
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf),
			"'%d:%s' voted %s '%d:%s' reason='%s' cmd='%s' force=%d",
			ClientID, Server()->ClientName(ClientID), pMsg->m_Type,
			SpectateID, Server()->ClientName(SpectateID), pReason, aCmd, pMsg->m_Force);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		if(pMsg->m_Force)
		{
			Server()->SetRconCID(ClientID);
			GameServer()->Console()->ExecuteLine(aCmd);
			Server()->SetRconCID(IServer::RCON_CID_SERV);
			SendForceVote(VOTE_START_SPEC, aDesc, pReason);
			return;
		}
		m_VoteType = VOTE_START_SPEC;
		m_VoteClientID = SpectateID;
	}

	if(m_VoteType != -1)
	{
		m_VoteCreator = ClientID;
		StartVote(aDesc, aCmd, pReason);
		pPlayer->m_Vote = VOTE_CHOICE_YES;
		pPlayer->m_VotePos = m_VotePos = 1;
		pPlayer->m_LastVoteCallTick = Now;
	}
}

void CGameMenu::OnVote(int ClientID, CNetMsg_Cl_Vote *pMsg)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return;

	if(!m_VoteCloseTime)
		return;

	if(pPlayer->m_Vote == VOTE_CHOICE_PASS)
	{
		if(pMsg->m_Vote == VOTE_CHOICE_PASS)
			return;

		pPlayer->m_Vote = pMsg->m_Vote;
		pPlayer->m_VotePos = ++m_VotePos;
		m_VoteUpdate = true;
	}
	else if(m_VoteCreator == pPlayer->GetCID())
	{
		if(pMsg->m_Vote != VOTE_CHOICE_NO || m_VoteCancelTime < time_get())
			return;

		m_VoteCloseTime = -1;
	}
}

void CGameMenu::AddVoteOption(const char *pDescription, const char *pCommand)
{
	if(m_NumServerVoteOptions == MAX_VOTE_OPTIONS)
	{
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "maximum number of vote options reached");
		return;
	}

	// check for valid option
	if(!GameServer()->Console()->LineIsValid(pCommand) || str_length(pCommand) >= VOTE_CMD_LENGTH)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid command '%s'", pCommand);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	pDescription = str_skip_whitespaces_const(pDescription);
	if(str_length(pDescription) >= VOTE_DESC_LENGTH || *pDescription == 0)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid option '%s'", pDescription);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// check for duplicate entry
	for(CVoteOptionServer *pOption = m_pServerVoteOptionFirst; pOption; pOption = pOption->m_pNext)
	{
		if(str_comp_nocase(pDescription, pOption->m_aDescription) == 0)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "option '%s' already exists", pDescription);
			GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
			return;
		}
	}

	// add the option
	++m_NumServerVoteOptions;
	int Len = str_length(pCommand);

	CVoteOptionServer *pOption = (CVoteOptionServer *) m_pServerVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len, alignof(CVoteOptionServer));
	pOption->m_pNext = 0;
	pOption->m_pPrev = m_pServerVoteOptionLast;
	if(pOption->m_pPrev)
		pOption->m_pPrev->m_pNext = pOption;
	m_pServerVoteOptionLast = pOption;
	if(!m_pServerVoteOptionFirst)
		m_pServerVoteOptionFirst = pOption;

	str_copy(pOption->m_aDescription, pDescription, sizeof(pOption->m_aDescription));
	mem_copy(pOption->m_aCommand, pCommand, Len + 1);
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "added option '%s' '%s'", pOption->m_aDescription, pOption->m_aCommand);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	// inform clients about added option
	CNetMsg_Sv_VoteOptionAdd OptionMsg;
	OptionMsg.m_pDescription = pOption->m_aDescription;
	Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, -1);
}

void CGameMenu::RemoveVoteOption(const char *pDescription)
{
	// check for valid option
	CVoteOptionServer *pOption = m_pServerVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(pDescription, pOption->m_aDescription) == 0)
			break;
		pOption = pOption->m_pNext;
	}
	if(!pOption)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "option '%s' does not exist", pDescription);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// inform clients about removed option
	CNetMsg_Sv_VoteOptionRemove OptionMsg;
	OptionMsg.m_pDescription = pOption->m_aDescription;
	Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, -1);

	// TODO: improve this
	--m_NumServerVoteOptions;
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "removed option '%s' '%s'", pOption->m_aDescription, pOption->m_aCommand);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	CHeap *pVoteOptionHeap = new CHeap();
	CVoteOptionServer *pVoteOptionFirst = 0;
	CVoteOptionServer *pVoteOptionLast = 0;
	for(CVoteOptionServer *pSrc = m_pServerVoteOptionFirst; pSrc; pSrc = pSrc->m_pNext)
	{
		if(pSrc == pOption)
			continue;

		// copy option
		int Len = str_length(pSrc->m_aCommand);
		CVoteOptionServer *pDst = (CVoteOptionServer *) pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len, alignof(CVoteOptionServer));
		pDst->m_pNext = 0;
		pDst->m_pPrev = pVoteOptionLast;
		if(pDst->m_pPrev)
			pDst->m_pPrev->m_pNext = pDst;
		pVoteOptionLast = pDst;
		if(!pVoteOptionFirst)
			pVoteOptionFirst = pDst;

		str_copy(pDst->m_aDescription, pSrc->m_aDescription, sizeof(pDst->m_aDescription));
		mem_copy(pDst->m_aCommand, pSrc->m_aCommand, Len + 1);
	}

	// clean up
	delete m_pServerVoteOptionHeap;
	m_pServerVoteOptionHeap = pVoteOptionHeap;
	m_pServerVoteOptionFirst = pVoteOptionFirst;
	m_pServerVoteOptionLast = pVoteOptionLast;
}

void CGameMenu::ClearVoteOptions()
{
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "cleared votes");
	SendVoteClearOptions(-1);
	m_pServerVoteOptionHeap->Reset();
	m_pServerVoteOptionFirst = 0;
	m_pServerVoteOptionLast = 0;
	m_NumServerVoteOptions = 0;
}

void CGameMenu::ForceVote(const char *pChoice)
{
	// check if there is a vote running
	if(!m_VoteCloseTime)
		return;

	if(str_comp_nocase(pChoice, "yes") == 0)
		m_VoteEnforce = VOTE_CHOICE_YES;
	else if(str_comp_nocase(pChoice, "no") == 0)
		m_VoteEnforce = VOTE_CHOICE_NO;
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "forcing vote %s", pChoice);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
}

bool CGameMenu::MenuVote(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUser)
{
	(void) pUser;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return false;

	if(VoteStatus.m_aCmd[0])
	{
		if(str_comp(VoteStatus.m_aCmd, "DISPLAY") == 0)
		{
		}
		else if(VoteStatus.m_aDesc[0])
		{
			int64 Now = pMenu->Server()->Tick();
			if((pMenu->Config()->m_SvSpamprotection && ((pPlayer->m_LastVoteTryTick && pPlayer->m_LastVoteTryTick + pMenu->Server()->TickSpeed() * 3 > Now) ||
								   (pPlayer->m_LastVoteCallTick && pPlayer->m_LastVoteCallTick + pMenu->Server()->TickSpeed() * VOTE_COOLDOWN > Now))) ||
				pPlayer->GetTeam() == TEAM_SPECTATORS || pMenu->m_VoteCloseTime)
				return false;

			pPlayer->m_LastVoteTryTick = Now;

			const char *pReason = VoteStatus.m_aReason[0] ? VoteStatus.m_aReason : "No reason given";
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf),
				"'%d:%s' voted %s '%s' reason='%s' cmd='%s' force=%d",
				ClientID, pMenu->Server()->ClientName(ClientID), "option",
				VoteStatus.m_aDesc, pReason, VoteStatus.m_aCmd, VoteStatus.m_Force ? 1 : 0);
			pMenu->GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

			if(VoteStatus.m_Force)
			{
				pMenu->Server()->SetRconCID(ClientID);
				pMenu->GameServer()->Console()->ExecuteLine(VoteStatus.m_aCmd);
				pMenu->Server()->SetRconCID(IServer::RCON_CID_SERV);
				pMenu->SendForceVote(VOTE_START_OP, VoteStatus.m_aDesc, pReason);
				return false;
			}

			pMenu->m_VoteType = VOTE_START_OP;
			pMenu->m_VoteCreator = ClientID;
			pMenu->StartVote(VoteStatus.m_aDesc, VoteStatus.m_aCmd, pReason);
			pPlayer->m_Vote = VOTE_CHOICE_YES;
			pPlayer->m_VotePos = pMenu->m_VotePos = 1;
			pPlayer->m_LastVoteCallTick = Now;
			return false;
		}
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();
	{
		if(pMenu->m_pServerVoteOptionFirst)
		{
			for(CVoteOptionServer *pOption = pMenu->m_pServerVoteOptionFirst; pOption; pOption = pOption->m_pNext)
			{
				pMenu->AddOption(pOption->m_aDescription, pOption->m_aCommand);
			}
		}
		else
		{
			pMenu->AddOption(Localize("There's no any server vote", "Vote Menu Server Vote"), "NONE");
		}
	}

	return true;
}

