/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_GAMEMENU_H
#define GAME_SERVER_GAMEMENU_H

#include <base/system.h>
#include <base/tl/hashtable.h>

#include <engine/shared/protocol.h>

#include <game/item_id.h>
#include <game/voting.h>

class CHeap;
class CNetMsg_Cl_CallVote;
class CNetMsg_Cl_Vote;

#define MENU_MAIN_PAGE_ID str_quickhash("MAIN")
#define MENU_OPTIONS_NUM 12

struct CCallVoteStatus
{
	char m_aDesc[VOTE_DESC_LENGTH] = {'\0'};
	char m_aCmd[VOTE_CMD_LENGTH] = {'\0'};
	char m_aReason[VOTE_REASON_LENGTH] = {'\0'};
	bool m_Force = false; // admin
};

// return: true means that need to send vote msg
typedef bool (*FMenuCallback)(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);

struct CMenuPage
{
	unsigned m_Hash = 0;
	unsigned m_ParentHash = 0;
	FMenuCallback m_pfnCallback = nullptr;
	char m_aTitle[VOTE_DESC_LENGTH] = {'\0'};
	char m_aContext[VOTE_DESC_LENGTH] = {'\0'};
	void *m_pUserData = nullptr;
};

class CGameMenu
{
	class CGameContext *m_pGameServer;
	CGameContext *GameServer() const { return m_pGameServer; }
	class CConfig *Config() const;
	class IServer *Server() const;

public:
	CGameMenu() : m_pGameServer(nullptr), m_pServerVoteOptionHeap(nullptr) {}
	CGameMenu(CGameContext *pGameServer);
	~CGameMenu();

	// registers the menu pages; must be called once the game server is set
	void Init(CGameContext *pGameServer);

	void Register(const char *pPageName, const char *pTitle, FMenuCallback pfnFunc, void *pUser, const char *pParent = "MAIN");

	void OnClientEntered(int ClientID);
	void OnMenuVote(int ClientID, CCallVoteStatus &VoteStatus, bool Sound = false);
	void SendMenuChat(int ClientID, const char *pChat);

	// ----- server vote -----
	// The menu is carried over the vote protocol, so the vote state lives on this
	// class too: the active vote, its tally and the server vote option list.
	// Implemented in menus/vote_menu.cpp.
	void TickVote();
	void SendActiveVote(int ClientID);
	void AbortVoteOnDisconnect(int ClientID);
	void AbortVoteOnTeamChange(int ClientID);
	void OnCallVote(int ClientID, class CNetMsg_Cl_CallVote *pMsg);
	void OnVote(int ClientID, class CNetMsg_Cl_Vote *pMsg);
	// a client arriving or switching team changes who may vote
	void SetVoteUpdate() { m_VoteUpdate = true; }

	// console command bodies (the command callbacks forward to these)
	void AddVoteOption(const char *pDescription, const char *pCommand);
	void RemoveVoteOption(const char *pDescription);
	void ClearVoteOptions();
	void ForceVote(const char *pChoice);
	// send the message that clears the client's vote/menu option list
	void SendVoteClearOptions(int ClientID);

	void ClearOptions(int ClientID);
	void RefreshMenu(int ClientID);
	// Regenerate the shop page of every client currently looking at it (stock
	// changed). ExceptClientID is skipped: its callback is the caller and will
	// regenerate itself.
	void RefreshShop(int ExceptClientID = -1);
	void SetPlayerPage(int ClientID, unsigned Page);
	void SetPlayerPage(int ClientID, const char *pPage);

	// generate menu
	void AddPageTitle();
	void AddSpace();
	void AddHorizontalRule();
	void AddOption(const char *pDesc, const char *pCommand, const char *pPrefix = "");
	// wraps long text into DISPLAY rows (safe for multibyte utf-8)
	void AddWrappedLinesOption(const char *pText);

private:
	int m_CurrentClientID;
	static bool MenuMain(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	// item menus: inventory, item view and crafting (implemented in menus/item_menu.cpp)
	static bool MenuInventory(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	// shows one item: description + actions ("use", ...)
	static bool MenuItemView(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	static bool MenuCraft(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	// the "Server Vote" page (implemented in menus/vote_menu.cpp)
	static bool MenuVote(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	// the "Travel" page: the configured euclidean floors the train can reach
	// (implemented in gamemenu.cpp)
	static bool MenuTravel(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	// the "Language" page (implemented in gamemenu.cpp)
	static bool MenuLanguage(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	// loadout (weapon equipment) menu (implemented in menus/loadout_menu.cpp)
	static bool MenuLoadout(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	static bool MenuWeaponPick(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	// player owned shops (listing / buying goods). The whole flow is driven
	// from the vote menu so no chat command is needed.
	// Implemented in menus/trade_menu.cpp.
	static bool MenuTrade(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	static bool MenuTradeSell(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	static bool MenuTradeSellCount(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	static bool MenuTradeSellPrice(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	static bool MenuTradeSellPriceCount(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	static bool MenuTradeMine(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);

	// ----- server vote state (implemented in menus/vote_menu.cpp) -----
	enum
	{
		VOTE_TIME = 25,
		VOTE_CANCEL_TIME = 10,
	};
	int m_VoteCreator;
	int m_VoteType;
	int64 m_VoteCloseTime;
	int64 m_VoteCancelTime;
	bool m_VoteUpdate;
	int m_VotePos;
	char m_aVoteDescription[VOTE_DESC_LENGTH];
	char m_aVoteCommand[VOTE_CMD_LENGTH];
	char m_aVoteReason[VOTE_REASON_LENGTH];
	int m_VoteClientID;
	int m_VoteEnforce;

	// global server vote option list; per-client menu options live in CPlayerData
	class CHeap *m_pServerVoteOptionHeap;
	CVoteOptionServer *m_pServerVoteOptionFirst;
	CVoteOptionServer *m_pServerVoteOptionLast;
	int m_NumServerVoteOptions;

	void StartVote(const char *pDesc, const char *pCommand, const char *pReason);
	void EndVote(int Type, bool Force);
	void SendForceVote(int Type, const char *pDescription, const char *pReason);
	void SendVoteSet(int Type, int ToClientID);
	void SendVoteStatus(int ClientID, int Total, int Yes, int No);

	// slot the player picked in MenuLoadout, before picking the weapon in
	// MenuWeaponPick; -1 = none
	int m_aLoadoutSlot[MAX_CLIENTS];
	// item the player is currently inspecting in MenuItemView
	CItemID m_aItemViewId[MAX_CLIENTS];

	// the in-progress listing of a player, filled step by step by the trade
	// pages (offer item -> amount -> price item -> price amount)
	struct STradeDraft
	{
		CItemID m_OfferItem;
		int m_OfferCount = 0;
		CItemID m_PriceItem;
	};
	STradeDraft m_aTradeDraft[MAX_CLIENTS];

	// The trade provider a player has expanded on the shop page ("" = none).
	// The key is "N<defindex>" for an NPC shelf or "P<clientid>" for a player's
	// listings. Only the provider named here is drawn expanded, so the shop
	// stays one page and clicking a name folds the list in place.
	char m_aTradeOpen[MAX_CLIENTS][16];
	// The crafting recipe a player has expanded on the craft page ("" = none);
	// the recipe's materials and its "Craft" action only show while it is open.
	char m_aCraftOpen[MAX_CLIENTS][32];

	hash_table<unsigned, CMenuPage, 8> m_MenuPages;

	class CPlayerData
	{
	public:
		class CHeap *m_pVoteOptionHeap;
		CVoteOptionServer *m_pVoteOptionFirst;
		CVoteOptionServer *m_pVoteOptionLast;
		int m_NumVoteOptions;

		unsigned m_CurrentPage;
		char m_aMenuChat[48];

		void Reset(bool Clear = false)
		{
			if(Clear)
			{
				m_pVoteOptionFirst = nullptr;
				m_pVoteOptionLast = nullptr;
				m_NumVoteOptions = 0;
			}

			m_CurrentPage = MENU_MAIN_PAGE_ID;
			m_aMenuChat[0] = '\0';
		}

		CPlayerData();
		~CPlayerData();
	};
	CPlayerData m_aPlayerData[MAX_CLIENTS];
};

#endif
