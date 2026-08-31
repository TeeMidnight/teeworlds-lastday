/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <engine/shared/memheap.h>

#include "gamecontext.h"
#include "gamemenu.h"
#include "player.h"

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

	Register("MAIN", "Main Menu", MenuMain, nullptr); // Localize("Main Menu", "MAIN")
	Register("INVENTORY", "Inventory", MenuInventory, nullptr); // Localize("Inventory", "INVENTORY")
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
		pMenu->AddOption(Localize("Server Vote", "Menu Main"), "PAGE VOTE", "★");
	}

	return true;
}

bool CGameMenu::MenuInventory(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	// if the player clicked an item, show its description
	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "DISPLAY") != 0 && str_comp(VoteStatus.m_aCmd, "NONE") != 0)
	{
		const char *pDesc = pMenu->GameServer()->Item()->GetDesc(VoteStatus.m_aCmd);
		if(pDesc && pDesc[0])
		{
			const char *pName = pMenu->GameServer()->Item()->GetName(VoteStatus.m_aCmd);
			char aCtx[128];
			str_format(aCtx, sizeof(aCtx), "Item Desc: %s", pName);
			pMenu->GameServer()->SendChat(-1, CHAT_ALL, ClientID, Localize(pDesc, aCtx));
		}
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	const CItemSystem::CInventory &Inventory = pMenu->GameServer()->Item()->GetInventory(ClientID);
	for(int i = 0; i < CItemSystem::CInventory::MAX_ITEMS; i++)
	{
		if(i < Inventory.m_NumItems)
		{
			const char *pName = pMenu->GameServer()->Item()->GetName(Inventory.m_aItems[i].m_aResId);
			pMenu->AddOptionFormat(Localize("%d. %s: %d", "Menu Inventory"), Inventory.m_aItems[i].m_aResId, "-",
				i + 1, Localize(pName, "Item Name"), Inventory.m_aItems[i].m_Count);
		}
		else
		{
			pMenu->AddOptionFormat(Localize("%d. None", "Menu Inventory"), "DISPLAY", "-", i + 1);
		}
	}

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
