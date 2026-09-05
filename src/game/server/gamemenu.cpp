#include <engine/shared/memheap.h>

#include "entities/character.h"
#include "gamecontext.h"
#include "gamemenu.h"
#include "item.h"
#include "player.h"
#include "weapon.h"
#include "weaponmanager.h"

#include <cstdarg>
#include <cstdio>

CConfig *CGameMenu::Config() const { return GameServer()->Config(); }
IServer *CGameMenu::Server() const { return GameServer()->Server(); }

CGameMenu::CGameMenu(CGameContext *pGameServer) :
	m_pGameServer(pGameServer)
{
	Init(pGameServer);
}

void CGameMenu::Init(CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
	m_CurrentClientID = -1;

	for(auto &Data : m_aPlayerData)
	{
		Data.Reset();
	}

	m_MenuPages.clear();
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		m_aLoadoutSlot[i] = -1;
		m_aItemViewId[i][0] = '\0';
	}

	Register("MAIN", "Main Menu", MenuMain, nullptr); // Localize("Main Menu", "MAIN")
	Register("INVENTORY", "Inventory", MenuInventory, nullptr); // Localize("Inventory", "INVENTORY")
	Register("ITEMVIEW", "Item", MenuItemView, nullptr, "INVENTORY"); // Localize("Item", "ITEMVIEW")
	Register("CRAFT", "Craft", MenuCraft, nullptr); // Localize("Craft", "CRAFT")
	Register("LOADOUT", "Loadout", MenuLoadout, nullptr); // Localize("Loadout", "LOADOUT")
	Register("WEAPONPICK", "Choose Weapon", MenuWeaponPick, nullptr, "LOADOUT"); // Localize("Choose Weapon", "WEAPONPICK")
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
	GameServer()->SendVoteClearOptions(ClientID);

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
			pMenu->AddOptionFormat(Localize("Level: %d", "Menu Main"), "DISPLAY", "-", pPlayer->m_Status.m_Level);
	}
	pMenu->AddHorizontalRule();
	// options
	{
		pMenu->AddOption(Localize("Inventory", "Menu Main"), "PAGE INVENTORY", "★");
		pMenu->AddOption(Localize("Craft", "Menu Main"), "PAGE CRAFT", "★");
		pMenu->AddOption(Localize("Loadout", "Menu Main"), "PAGE LOADOUT", "★");
		pMenu->AddOption(Localize("Server Vote", "Menu Main"), "PAGE VOTE", "★");
	}

	return true;
}

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
		if(pItem->GetItemCount(ClientID, VoteStatus.m_aCmd) > 0)
		{
			str_copy(pMenu->m_aItemViewId[ClientID], VoteStatus.m_aCmd, sizeof(pMenu->m_aItemViewId[ClientID]));
			pMenu->SetPlayerPage(ClientID, "ITEMVIEW");
			return false;
		}
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	const CItemSystem::CInventory &Inventory = pMenu->GameServer()->Item()->GetInventory(ClientID);
	for(int i = 0; i < CItemSystem::CInventory::MAX_ITEMS; i++)
	{
		if(!Inventory.IsEmpty(i))
		{
			const char *pName = pMenu->GameServer()->Item()->GetName(Inventory.m_aItems[i].m_aResId);
			pMenu->AddOptionFormat("%d. %s: %d", Inventory.m_aItems[i].m_aResId, "-",
				i + 1, Localize(pName, "Item Name"), Inventory.m_aItems[i].m_Count);
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
	const char *pResId = pMenu->m_aItemViewId[ClientID];
	if(!pResId[0] || pItem->GetItemCount(ClientID, pResId) <= 0)
	{
		// the item is gone (used up / removed): go back to the inventory
		pMenu->SetPlayerPage(ClientID, "INVENTORY");
		return false;
	}

	const int Count = pItem->GetItemCount(ClientID, pResId);
	const char *pName = pItem->GetName(pResId);
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
		const int Used = pItem->UseItem(ClientID, pResId, UseCount);
		if(Used > 0)
		{
			char aMsg[128];
			str_format(aMsg, sizeof(aMsg), Localize("Used: %s x%d", "Menu Inventory"), pLocalizedName, Used);
			pMenu->GameServer()->SendChat(-1, CHAT_WHISPER, ClientID, aMsg);
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
		pMenu->GameServer()->DropItem(ClientID, pResId, DropCount, Direction);

		// refresh: back to the list, or keep viewing if still owned
		pMenu->SetPlayerPage(ClientID, "INVENTORY");
		return false;
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	char aHeader[VOTE_DESC_LENGTH];
	str_format(aHeader, sizeof(aHeader), "%s x%d", pLocalizedName, Count);
	pMenu->AddOption(aHeader, "DISPLAY", "=");

	// description (localized), wrapped over several rows
	const char *pDesc = pItem->GetDesc(pResId);
	if(pDesc && pDesc[0])
	{
		char aCtx[128];
		str_format(aCtx, sizeof(aCtx), "Item Desc: %s", pName);
		pMenu->AddWrappedLinesOption(Localize(pDesc, aCtx));
	}

	// item types (e.g. "weapon"), localized, listed below the description
	SItemTypeLineData TypeLine = {{'\0'}, 0, 0};
	pItem->ForEachItemType(pResId, ItemTypeLineCallback, &TypeLine);
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
	if(pItem->IsUsable(pResId))
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
	int m_Index;
};

// renders one recipe as a block of menu rows (also used only from MenuCraft)
static void CraftListCallback(CItemSystem::SCraftDef &Craft, void *pUser)
{
	CCraftMenuData *pData = static_cast<CCraftMenuData *>(pUser);
	CGameMenu *pMenu = pData->m_pMenu;
	const int ClientID = pData->m_ClientID;

	const char *pResultName = pData->m_pItem->GetName(Craft.m_aResultItemId);
	pData->m_Index++;
	pMenu->AddOptionFormat("%d. %s x%d", Craft.m_aCraftId, "→",
		pData->m_Index, Localize(pResultName, "Item Name"), Craft.m_ResultCount);

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
					Localize(pData->m_pItem->GetName(Need.m_aItemId), "Item Name"), pMark);
		}
		else
		{
			if(Need.m_MatchByType)
				pMenu->AddOptionFormat(Localize("   any %s %d/%d", "Menu Craft"), "DISPLAY", "-",
					Localize(Need.m_aType, "Item Type"), Have, Need.m_Count);
			else
				pMenu->AddOptionFormat("   %s %d/%d", "DISPLAY", "-",
					Localize(pData->m_pItem->GetName(Need.m_aItemId), "Item Name"), Have, Need.m_Count);
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
					Localize(pItem->GetName(pCraft->m_aResultItemId), "Item Name"), pCraft->m_ResultCount);
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

	CCraftMenuData Data = {pItem, pMenu, ClientID, 0};
	pItem->ForEachCraft(CraftListCallback, &Data);

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
		const unsigned Item = pChr ? pChr->WeaponAtSlot(i) : 0;
		const IWeaponInterface *pWeapon = Item ? WeaponManager()->GetWeapon(Item) : nullptr;

		// resolve the display name through the item system (translated), the
		// weapon class name is only a res_id fallback
		const char *pResId = Item ? pItem->GetResIdByHash(ClientID, Item) : nullptr;
		const char *pRawName = pResId ? pItem->GetName(pResId) : (pWeapon ? pWeapon->Name() : nullptr);

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
		else if(Item != 0)
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
		if(pChr && Slot >= 0 && Slot < NUM_WEAPONS && pChr->EquipWeaponSlot(Slot, str_quickhash(VoteStatus.m_aCmd)))
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
		const char *pResId = Inventory.m_aItems[i].m_aResId;
		const char *pName = pItem->GetName(pResId);
		if(!pName[0])
			pName = pResId;
		pMenu->AddOption(Localize(pName, "Item Name"), pResId);
	}

	return true;
}
