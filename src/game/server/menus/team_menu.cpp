/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/localization_format.h>
#include <game/server/gamecontext.h>
#include <game/server/gamemenu.h>
#include <game/server/player.h>
#include <game/server/team.h>

static const char *OnlineName(CGameContext *pGame, const Uuid &Account)
{
	if(!pGame)
		return "";
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = pGame->m_apPlayers[i];
		if(pPlayer && pPlayer->m_AccountUuid == Account)
			return pGame->Server()->ClientName(i);
	}
	return "";
}

static void SendTeamStatus(CGameContext *pGame, int ClientID, const char *pText, const char *pError)
{
	CLanguageScope LanguageScope(pGame->LanguageOf(ClientID));
	char aBuf[256];
	if(pError && pError[0])
		str_format(aBuf, sizeof(aBuf), "%s: %s", Localize(pText, "Team"), Localize(pError, "Team"));
	else
		str_copy(aBuf, Localize(pText, "Team"), sizeof(aBuf));
	pGame->SendChat(-1, CHAT_ALL, ClientID, aBuf);
}

bool CGameMenu::MenuTeam(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CGameContext *pGameServer = pMenu->GameServer();
	CPlayer *pPlayer = pGameServer->m_apPlayers[ClientID];
	CTeamSystem *pTeams = pGameServer->Teams();
	if(!pPlayer || !pTeams)
		return false;

	const Uuid &Account = pPlayer->m_AccountUuid;
	const SGameTeam *pTeam = pTeams->TeamFor(Account);
	char aError[128] = "";
	char aStatus[128] = "";

	if(str_comp(VoteStatus.m_aCmd, "TEAM CREATE") == 0)
	{
		// The vote reason box is short, so the menu offers it as a quick path
		// while /team create <name> supports longer names.
		if(pTeams->CreateTeam(Account, VoteStatus.m_aReason, aError, sizeof(aError)))
			str_copy(aStatus, "Team created.", sizeof(aStatus));
	}
	else if(str_startswith(VoteStatus.m_aCmd, "TEAM JOIN "))
	{
		const int TeamId = str_toint(VoteStatus.m_aCmd + str_length("TEAM JOIN "));
		if(pTeams->JoinTeam(Account, TeamId, aError, sizeof(aError)))
			str_copy(aStatus, "Joined the team.", sizeof(aStatus));
	}
	else if(str_comp(VoteStatus.m_aCmd, "TEAM LEAVE") == 0)
	{
		if(pTeams->LeaveTeam(Account, aError, sizeof(aError)))
			str_copy(aStatus, "Left the team.", sizeof(aStatus));
	}
	else if(str_comp(VoteStatus.m_aCmd, "TEAM FF") == 0)
	{
		pTeam = pTeams->TeamFor(Account);
		if(pTeam && pTeam->m_Leader == Account)
		{
			if(pTeams->SetFriendlyFire(Account, !pTeam->m_FriendlyFire, aError, sizeof(aError)))
				str_copy(aStatus, "Friendly fire toggled.", sizeof(aStatus));
		}
		else
			str_copy(aError, "Only the leader can change friendly fire.", sizeof(aError));
	}
	else if(str_comp(VoteStatus.m_aCmd, "TEAM DISBAND") == 0)
	{
		if(pTeams->DisbandTeam(Account, aError, sizeof(aError)))
			str_copy(aStatus, "Team disbanded.", sizeof(aStatus));
	}
	else if(str_startswith(VoteStatus.m_aCmd, "TEAM KICK "))
	{
		Uuid Target = UUID_ZEROED;
		if(parse_uuid(&Target, VoteStatus.m_aCmd + str_length("TEAM KICK ")) == 0)
		{
			if(pTeams->KickMember(Account, Target, aError, sizeof(aError)))
				str_copy(aStatus, "Member kicked.", sizeof(aStatus));
		}
	}

	if(aStatus[0])
		SendTeamStatus(pGameServer, ClientID, aStatus, nullptr);
	if(aError[0])
		SendTeamStatus(pGameServer, ClientID, "Team action failed", aError);

	pTeam = pTeams->TeamFor(Account);
	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	if(!pTeam)
	{
		pMenu->AddOption(Localize("You are not in a team.", "Menu Team"), "DISPLAY", "-");
		pMenu->AddOption(Localize("Use /team create <name> or type a name in the reason box below.", "Menu Team"), "DISPLAY", "-");
		pMenu->AddOption(Localize("Create team (reason box = name)", "Menu Team"), "TEAM CREATE", "+");
	}
	else
	{
		pMenu->AddOption(LocalizeFormat("Menu Team", "Team: {}", pTeam->m_aName).c_str(), "DISPLAY", "-");
		const char *pLeaderName = OnlineName(pGameServer, pTeam->m_Leader);
		if(pLeaderName[0])
			pMenu->AddOption(LocalizeFormat("Menu Team", "Leader: {}", pLeaderName).c_str(), "DISPLAY", "-");
		else
			pMenu->AddOption(Localize("Leader: (offline)", "Menu Team"), "DISPLAY", "-");
		pMenu->AddOption(LocalizeFormat("Menu Team", "Friendly fire: {}", pTeam->m_FriendlyFire ? Localize("ON", "Menu Team") : Localize("OFF", "Menu Team")).c_str(), "DISPLAY", "-");
		pMenu->AddOption(LocalizeFormat("Menu Team", "Members: {} ({} online)", (int) pTeam->m_lMembers.size(), pTeams->OnlineMemberCount(pTeam)).c_str(), "DISPLAY", "-");
		if(pTeam->m_Leader == Account)
			pMenu->AddOption(Localize("Toggle friendly fire", "Menu Team"), "TEAM FF", "★");

		pMenu->AddHorizontalRule();
		for(int i = 0; i < pTeam->m_lMembers.size(); i++)
		{
			const Uuid &Member = pTeam->m_lMembers[i];
			const char *pName = OnlineName(pGameServer, Member);
			char aUuid[UUID_MAXSTRSIZE];
			format_uuid(Member, aUuid, sizeof(aUuid));
			if(pName[0])
				pMenu->AddOption(LocalizeFormat("Menu Team", "{} {}", Member == pTeam->m_Leader ? "[L]" : "[M]", pName).c_str(), "DISPLAY", "-");
			else
				pMenu->AddOption(LocalizeFormat("Menu Team", "{} (offline) {}", Member == pTeam->m_Leader ? "[L]" : "[M]", aUuid).c_str(), "DISPLAY", "-");
			if(pTeam->m_Leader == Account && Member != Account)
			{
				char aCmd[96];
				str_format(aCmd, sizeof(aCmd), "TEAM KICK %s", aUuid);
				pMenu->AddOption(Localize("  Kick this member", "Menu Team"), aCmd, "x");
			}
		}

		pMenu->AddHorizontalRule();
		pMenu->AddOption(Localize("Leave team", "Menu Team"), "TEAM LEAVE", "<");
		if(pTeam->m_Leader == Account)
			pMenu->AddOption(Localize("Disband team", "Menu Team"), "TEAM DISBAND", "x");
	}

	pMenu->AddHorizontalRule();
	pMenu->AddOption(Localize("Online teams:", "Menu Team"), "DISPLAY", "-");
	bool AnyTeam = false;
	for(int i = 0; i < pTeams->NumTeams(); i++)
	{
		const SGameTeam *pOther = pTeams->TeamAt(i);
		if(!pOther || !pTeams->HasOnlineMember(pOther))
			continue;
		AnyTeam = true;
		pMenu->AddOption(LocalizeFormat("Menu Team", "{}  ({} online / {} members)", pOther->m_aName,
			pTeams->OnlineMemberCount(pOther), (int) pOther->m_lMembers.size()).c_str(), "DISPLAY", "-");
		if(!pTeam)
		{
			char aCmd[64];
			str_format(aCmd, sizeof(aCmd), "TEAM JOIN %d", pOther->m_Id);
			pMenu->AddOption(LocalizeFormat("Menu Team", "  Join {}", pOther->m_aName).c_str(), aCmd, "+");
		}
	}
	if(!AnyTeam)
		pMenu->AddOption(Localize("No team has an online member right now.", "Menu Team"), "DISPLAY", "-");

	pMenu->AddHorizontalRule();
	pMenu->AddOption(Localize("Online players:", "Menu Team"), "DISPLAY", "-");
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pOtherPlayer = pGameServer->m_apPlayers[i];
		if(!pOtherPlayer || !pGameServer->Server()->ClientIngame(i))
			continue;
		const char *pOtherTeam = pTeams->TeamNameFor(pOtherPlayer->m_AccountUuid);
		pMenu->AddOption(LocalizeFormat("Menu Team", "{}  {}", pGameServer->Server()->ClientName(i), pOtherTeam[0] ? pOtherTeam : "-").c_str(), "DISPLAY", "-");
	}

	return true;
}
