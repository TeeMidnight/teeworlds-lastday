/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_GAMEMENU_H
#define GAME_SERVER_GAMEMENU_H

#include <base/system.h>
#include <base/tl/hashtable.h>

#include <engine/shared/protocol.h>

#include <game/voting.h>

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
	CGameMenu() : m_pGameServer(nullptr) {}
	CGameMenu(CGameContext *pGameServer);
	~CGameMenu() = default;

	// registers the menu pages; must be called once the game server is set
	void Init(CGameContext *pGameServer);

	void Register(const char *pPageName, const char *pTitle, FMenuCallback pfnFunc, void *pUser, const char *pParent = "MAIN");

	void OnClientEntered(int ClientID);
	void OnMenuVote(int ClientID, CCallVoteStatus &VoteStatus, bool Sound = false);
	void SendMenuChat(int ClientID, const char *pChat);

	void ClearOptions(int ClientID);
	void RefreshMenu(int ClientID);
	void SetPlayerPage(int ClientID, unsigned Page);
	void SetPlayerPage(int ClientID, const char *pPage);

	// generate menu
	void AddPageTitle();
	void AddSpace();
	void AddHorizontalRule();
	void AddOption(const char *pDesc, const char *pCommand, const char *pPrefix = "");

	void AddOptionFormat(const char *pDesc, const char *pCommand, const char *pPrefix, ...);

private:
	int m_CurrentClientID;
	static bool MenuMain(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);
	static bool MenuInventory(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData);

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
