/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/math.h>

#include <engine/shared/config.h>
#include <engine/shared/memheap.h>

#include <game/localization_format.h>

#include "entities/character.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "gamemenu.h"
#include "item.h"
#include "npcdata.h"
#include "player.h"
#include "trade.h"
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
		m_aTradeDraft[i] = STradeDraft();
		m_aTradeOpen[i][0] = '\0';
		m_aCraftOpen[i][0] = '\0';
	}

	Register("MAIN", "Main Menu", MenuMain, nullptr); // Localize("Main Menu", "MAIN")
	Register("INVENTORY", "Inventory", MenuInventory, nullptr); // Localize("Inventory", "INVENTORY")
	Register("ITEMVIEW", "Item", MenuItemView, nullptr, "INVENTORY"); // Localize("Item", "ITEMVIEW")
	Register("CRAFT", "Craft", MenuCraft, nullptr); // Localize("Craft", "CRAFT")
	Register("CONTAINER", "Container", MenuContainer, nullptr); // Localize("Container", "CONTAINER")
	Register("LOADOUT", "Loadout", MenuLoadout, nullptr); // Localize("Loadout", "LOADOUT")
	Register("WEAPONPICK", "Choose Weapon", MenuWeaponPick, nullptr, "LOADOUT"); // Localize("Choose Weapon", "WEAPONPICK")
	Register("ACCESSORYPICK", "Choose Accessory", MenuWeaponPick, nullptr, "LOADOUT"); // Localize("Choose Accessory", "ACCESSORYPICK")
	Register("TRADE", "Shop", MenuTrade, nullptr); // Localize("Shop", "TRADE")
	Register("TRADE_SELL", "Sell Goods", MenuTradeSell, nullptr, "TRADE"); // Localize("Sell Goods", "TRADE_SELL")
	Register("TRADE_SELL_COUNT", "Amount", MenuTradeSellCount, nullptr, "TRADE_SELL"); // Localize("Amount", "TRADE_SELL_COUNT")
	Register("TRADE_SELL_PRICE", "Price", MenuTradeSellPrice, nullptr, "TRADE_SELL_COUNT"); // Localize("Price", "TRADE_SELL_PRICE")
	Register("TRADE_SELL_PRICE_COUNT", "Price Amount", MenuTradeSellPriceCount, nullptr, "TRADE_SELL_PRICE"); // Localize("Price Amount", "TRADE_SELL_PRICE_COUNT")
	Register("TRADE_MINE", "My Listings", MenuTradeMine, nullptr, "TRADE"); // Localize("My Listings", "TRADE_MINE")
	Register("VOTE", "Server Vote", MenuVote, nullptr); // Localize("Server Vote", "VOTE")
	Register("TRAVEL", "Travel", MenuTravel, nullptr); // Localize("Travel", "TRAVEL")
	Register("LANG", "Language", MenuLanguage, nullptr); // Localize("Language", "LANG")
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
	m_aTradeDraft[ClientID] = STradeDraft();
	m_aTradeOpen[ClientID][0] = '\0';
	m_aCraftOpen[ClientID][0] = '\0';
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
	// every Localize() in the page callback, including nested item names in
	// std::format, resolves against this player's language
	CLanguageScope LanguageScope(GameServer()->LanguageOf(ClientID));

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

void CGameMenu::RefreshShop(int ExceptClientID)
{
	// regenerating another client's page must not steal m_CurrentClientID from a
	// callback that is still running (AddOption uses it)
	const int SavedClient = m_CurrentClientID;
	const unsigned TradePage = str_quickhash("TRADE");
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == ExceptClientID)
			continue;
		if(!GameServer()->m_apPlayers[i] || !Server()->ClientIngame(i))
			continue;
		if(m_aPlayerData[i].m_CurrentPage == TradePage)
			RefreshMenu(i);
	}
	m_CurrentClientID = SavedClient;
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
		else if(str_comp(VoteStatus.m_aCmd, "PAGE CONTAINER") == 0)
		{
			pMenu->SetPlayerPage(ClientID, "CONTAINER");
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
		else if(str_comp(VoteStatus.m_aCmd, "PAGE TRAVEL") == 0)
		{
			pMenu->SetPlayerPage(ClientID, "TRAVEL");
			return false;
		}
		else if(str_comp(VoteStatus.m_aCmd, "PAGE LANG") == 0)
		{
			pMenu->SetPlayerPage(ClientID, "LANG");
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
		pMenu->AddOption(LocalizeFormat("Menu Main", "Name: {}", pMenu->Server()->ClientName(ClientID)).c_str(), "DISPLAY", "-");
		if(pPlayer)
		{
			pMenu->AddOption(LocalizeFormat("Menu Main", "Level: {}", pPlayer->m_Status.m_Level).c_str(), "DISPLAY", "-");
			// mercury chapter: the forgetting meter (0 = clear mind)
			pMenu->AddOption(LocalizeFormat("Menu Main", "Forgetting: {}", pPlayer->m_Status.m_Forgetting).c_str(), "DISPLAY", "-");
		}
	}
	pMenu->AddHorizontalRule();
	// options
	{
		pMenu->AddOption(Localize("Inventory", "Menu Main"), "PAGE INVENTORY", "★");
		pMenu->AddOption(Localize("Craft", "Menu Main"), "PAGE CRAFT", "★");
		// The entry is always present: the server cannot see when the client
		// opens the vote menu, so a page that only exists while a container is
		// aimed at could be missing from a list drawn earlier. Resolving the
		// target when the page opens (and telling the player to aim if there is
		// none) is reliable.
		pMenu->AddOption(Localize("Container", "Menu Main"), "PAGE CONTAINER", "★");
		pMenu->AddOption(Localize("Loadout", "Menu Main"), "PAGE LOADOUT", "★");
		pMenu->AddOption(Localize("Shop", "Menu Main"), "PAGE TRADE", "★");
		// A train only boards from a platform. The page stays listed as a
		// non-clickable hint elsewhere so the player can see why it is not
		// usable instead of wondering where it went.
		CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
		if(pPlayer && pMenu->GameServer()->IsOnTrainEntrance(pPlayer))
			pMenu->AddOption(Localize("Travel", "Menu Main"), "PAGE TRAVEL", "★");
		else
			pMenu->AddOption(Localize("Travel (stand on a train entrance)", "Menu Main"), "DISPLAY", "-");
		pMenu->AddOption(Localize("Server Vote", "Menu Main"), "PAGE VOTE", "★");
		pMenu->AddOption(Localize("Language", "Menu Main"), "PAGE LANG", "★");
	}

	return true;
}

// static
bool CGameMenu::MenuTravel(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CGameContext *pGameServer = pMenu->GameServer();
	CPlayer *pPlayer = pGameServer->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	const bool OnStation = pGameServer->IsOnTrainEntrance(pPlayer);

	// a destination was clicked: the train leaves at once and drops the player
	// on a random chunk of that euclidean world
	if(str_startswith(VoteStatus.m_aCmd, "TRAIN "))
	{
		const char *pName = VoteStatus.m_aCmd + str_length("TRAIN ");
		bool Known = false;
		for(int i = 0; i < pGameServer->Server()->GetTrainDestinationCount(); i++)
		{
			const char *pDestination = nullptr;
			if(pGameServer->Server()->GetTrainDestination(i, &pDestination, nullptr) &&
				pDestination && str_comp(pDestination, pName) == 0)
			{
				Known = true;
				break;
			}
		}

		// The platform and the destination list can change while the menu is
		// open, so re-check both instead of trusting the clicked command.
		if(!OnStation || !Known)
		{
			pMenu->SetPlayerPage(ClientID, "TRAVEL");
			return false;
		}

		// The engine consumed the chunk prepared for this trip and started
		// building the one for the following trip. A false result means that
		// chunk is still being generated; the player can board again shortly.
		char aMapName[64];
		if(!pGameServer->Server()->TakeTrainDestinationChunk(pName, aMapName, sizeof(aMapName)))
		{
			pMenu->SetPlayerPage(ClientID, "TRAVEL");
			CLanguageScope LanguageScope(pPlayer->Language());
			pGameServer->SendChat(-1, CHAT_ALL, ClientID,
				Localize("The train is still being prepared. Try again in a moment.", "Menu Travel"));
			return false;
		}

		// the engine only switches to a map name it knows; a chunk that has
		// never been visited is registered here
		pGameServer->Server()->RegisterMapName(aMapName);

		{
			CLanguageScope LanguageScope(pPlayer->Language());
			pGameServer->SendChat(-1, CHAT_ALL, ClientID,
				LocalizeFormat("Menu Travel", "The train takes you to {}.", Localize(pName, "World Name")).c_str());
		}
		pGameServer->SwitchPlayerWorld(pPlayer, str_quickhash(aMapName), vec2(0.0f, 0.0f), false, true);
		return false;
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	if(!OnStation)
	{
		pMenu->AddOption(Localize("You must stand on a train entrance to travel.", "Menu Travel"), "DISPLAY", "-");
		return true;
	}

	if(pGameServer->Server()->GetTrainDestinationCount() <= 0)
	{
		pMenu->AddOption(Localize("No train destinations are available.", "Menu Travel"), "DISPLAY", "-");
		return true;
	}

	pMenu->AddOption(Localize("Choose a destination:", "Menu Travel"), "DISPLAY", "-");
	for(int i = 0; i < pGameServer->Server()->GetTrainDestinationCount(); i++)
	{
		const char *pName = nullptr;
		if(!pGameServer->Server()->GetTrainDestination(i, &pName, nullptr) || !pName || !pName[0])
			continue;
		char aCommand[VOTE_CMD_LENGTH];
		str_format(aCommand, sizeof(aCommand), "TRAIN %s", pName);
		pMenu->AddOption(Localize(pName, "World Name"), aCommand, "★");
	}
	return true;
}

// static
bool CGameMenu::MenuLanguage(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	if(str_startswith(VoteStatus.m_aCmd, "SETLANG "))
	{
		const char *pCode = VoteStatus.m_aCmd + str_length("SETLANG ");
		if(pPlayer->SetLanguage(pCode))
		{
			pMenu->GameServer()->SavePlayerData(pPlayer);
			CLanguageScope LanguageScope(pPlayer->Language());
			pMenu->GameServer()->SendChat(-1, CHAT_ALL, ClientID, Localize("Language set.", "Language"));
		}
		pMenu->SetPlayerPage(ClientID, "LANG");
		return false;
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();
	pMenu->AddOption(Localize("Choose your language:", "Menu Language"), "DISPLAY", "-");
	for(int i = 0; i < g_Localization.NumLanguages(); i++)
	{
		char aCmd[32];
		str_format(aCmd, sizeof(aCmd), "SETLANG %s", g_Localization.LanguageCode(i));
		char aDesc[128];
		str_format(aDesc, sizeof(aDesc), "%s (%s)", g_Localization.LanguageName(i), g_Localization.LanguageCode(i));
		pMenu->AddOption(aDesc, aCmd, i == pPlayer->Language() ? "★" : "");
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

CGameMenu::CPlayerData::CPlayerData()
{
	m_pVoteOptionHeap = new CHeap();
	Reset(true);
}

CGameMenu::CPlayerData::~CPlayerData()
{
	delete m_pVoteOptionHeap;
}
