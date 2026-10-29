/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/localization_format.h>
#include <game/server/gamecontext.h>
#include <game/server/gamemenu.h>
#include <game/server/player.h>
#include <game/server/team.h>

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
	else if(str_comp(VoteStatus.m_aCmd, "TEAM ACCEPT") == 0)
	{
		if(pTeams->AcceptInvite(Account, aError, sizeof(aError)))
			str_copy(aStatus, "Joined the team.", sizeof(aStatus));
	}
	else if(str_comp(VoteStatus.m_aCmd, "TEAM DECLINE") == 0)
	{
		if(pTeams->DeclineInvite(Account, aError, sizeof(aError)))
			str_copy(aStatus, "Invitation declined.", sizeof(aStatus));
	}
	else if(str_startswith(VoteStatus.m_aCmd, "TEAM INVITE "))
	{
		Uuid Target = UUID_ZEROED;
		if(parse_uuid(&Target, VoteStatus.m_aCmd + str_length("TEAM INVITE ")) == 0)
		{
			if(pTeams->InviteMember(Account, Target, aError, sizeof(aError)))
				str_copy(aStatus, "Invitation sent.", sizeof(aStatus));
		}
	}
	else if(str_startswith(VoteStatus.m_aCmd, "TEAM UNINVITE "))
	{
		Uuid Target = UUID_ZEROED;
		if(parse_uuid(&Target, VoteStatus.m_aCmd + str_length("TEAM UNINVITE ")) == 0)
		{
			if(pTeams->CancelInvite(Account, Target, aError, sizeof(aError)))
				str_copy(aStatus, "Invitation cancelled.", sizeof(aStatus));
		}
	}
	else if(str_comp(VoteStatus.m_aCmd, "TEAM LEAVE") == 0)
	{
		const bool WasLeader = pTeam && pTeam->m_Leader == Account;
		if(pTeams->LeaveTeam(Account, aError, sizeof(aError)))
			str_copy(aStatus, WasLeader ? "The team was disbanded because the leader left." : "Left the team.", sizeof(aStatus));
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
	const SGameTeam *pInvite = pTeams->InviteFrom(Account);
	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();

	if(!pTeam)
	{
		if(pInvite)
		{
			char aLeaderName[128];
			pTeams->MemberName(pInvite->m_Leader, aLeaderName, sizeof(aLeaderName));
			pMenu->AddOption(LocalizeFormat("Menu Team", "Invitation to team {}", pInvite->m_aName).c_str(), "DISPLAY", "!");
			pMenu->AddOption(LocalizeFormat("Menu Team", "From leader: {}", aLeaderName).c_str(), "DISPLAY", "-");
			pMenu->AddOption(Localize("Accept invitation", "Menu Team"), "TEAM ACCEPT", "+");
			pMenu->AddOption(Localize("Decline invitation", "Menu Team"), "TEAM DECLINE", "x");
			pMenu->AddHorizontalRule();
		}
		else
		{
			pMenu->AddOption(Localize("You are not in a team.", "Menu Team"), "DISPLAY", "-");
			pMenu->AddOption(Localize("Teams are invitation-only. Ask a team leader to invite you.", "Menu Team"), "DISPLAY", "-");
		}
		pMenu->AddOption(Localize("Create team (reason box = name)", "Menu Team"), "TEAM CREATE", "+");
	}
	else
	{
		char aLeaderName[128];
		pTeams->MemberName(pTeam->m_Leader, aLeaderName, sizeof(aLeaderName));
		pMenu->AddOption(LocalizeFormat("Menu Team", "Team: {}", pTeam->m_aName).c_str(), "DISPLAY", "-");
		if(pTeams->ClientIDFor(pTeam->m_Leader) >= 0)
			pMenu->AddOption(LocalizeFormat("Menu Team", "Leader: {}", aLeaderName).c_str(), "DISPLAY", "-");
		else
			pMenu->AddOption(LocalizeFormat("Menu Team", "Leader: {} (offline)", aLeaderName).c_str(), "DISPLAY", "-");
		pMenu->AddOption(LocalizeFormat("Menu Team", "Friendly fire: {}", pTeam->m_FriendlyFire ? Localize("ON", "Menu Team") : Localize("OFF", "Menu Team")).c_str(), "DISPLAY", "-");
		pMenu->AddOption(LocalizeFormat("Menu Team", "Members: {} ({} online)", (int) pTeam->m_lMembers.size(), pTeams->OnlineMemberCount(pTeam)).c_str(), "DISPLAY", "-");
		if(pTeam->m_Leader == Account)
			pMenu->AddOption(Localize("Toggle friendly fire", "Menu Team"), "TEAM FF", "★");

		pMenu->AddHorizontalRule();
		for(int i = 0; i < pTeam->m_lMembers.size(); i++)
		{
			const Uuid &Member = pTeam->m_lMembers[i];
			char aName[128];
			pTeams->MemberName(Member, aName, sizeof(aName));
			const bool Online = pTeams->ClientIDFor(Member) >= 0;
			if(Online)
				pMenu->AddOption(LocalizeFormat("Menu Team", "{} {}", Member == pTeam->m_Leader ? "[L]" : "[M]", aName).c_str(), "DISPLAY", "-");
			else
				pMenu->AddOption(LocalizeFormat("Menu Team", "{} {} (offline)", Member == pTeam->m_Leader ? "[L]" : "[M]", aName).c_str(), "DISPLAY", "-");
			if(pTeam->m_Leader == Account && Member != Account)
			{
				char aUuid[UUID_MAXSTRSIZE];
				format_uuid(Member, aUuid, sizeof(aUuid));
				char aCmd[96];
				str_format(aCmd, sizeof(aCmd), "TEAM KICK %s", aUuid);
				pMenu->AddOption(LocalizeFormat("Menu Team", "  Kick {}: {}", i + 1, aName).c_str(), aCmd, "x");
			}
		}

		if(pTeam->m_Leader == Account)
		{
			pMenu->AddHorizontalRule();
			if(pTeam->m_lInvites.size() > 0)
			{
				pMenu->AddOption(Localize("Pending invitations:", "Menu Team"), "DISPLAY", "-");
				for(int i = 0; i < pTeam->m_lInvites.size(); i++)
				{
					const Uuid &Target = pTeam->m_lInvites[i];
					char aName[128];
					pTeams->MemberName(Target, aName, sizeof(aName));
					char aUuid[UUID_MAXSTRSIZE];
					format_uuid(Target, aUuid, sizeof(aUuid));
					char aCmd[96];
					str_format(aCmd, sizeof(aCmd), "TEAM UNINVITE %s", aUuid);
					pMenu->AddOption(LocalizeFormat("Menu Team", "  Cancel invite {}: {}", i + 1, aName).c_str(), aCmd, "x");
				}
			}

			pMenu->AddOption(Localize("Invite online players:", "Menu Team"), "DISPLAY", "-");
			bool AnyCandidate = false;
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				CPlayer *pOtherPlayer = pGameServer->m_apPlayers[i];
				if(!pOtherPlayer || !pGameServer->Server()->ClientIngame(i))
					continue;
				const Uuid &OtherAccount = pOtherPlayer->m_AccountUuid;
				if(OtherAccount == UUID_ZEROED || OtherAccount == Account)
					continue;
				if(pTeams->TeamFor(OtherAccount) || pTeam->IsInvited(OtherAccount))
					continue;
				AnyCandidate = true;
				char aUuid[UUID_MAXSTRSIZE];
				format_uuid(OtherAccount, aUuid, sizeof(aUuid));
				char aCmd[96];
				str_format(aCmd, sizeof(aCmd), "TEAM INVITE %s", aUuid);
				pMenu->AddOption(LocalizeFormat("Menu Team", "  Invite #{}: {}", i, pGameServer->Server()->ClientName(i)).c_str(), aCmd, "+");
			}
			if(!AnyCandidate)
				pMenu->AddOption(Localize("No online player is available to invite.", "Menu Team"), "DISPLAY", "-");
		}

		pMenu->AddHorizontalRule();
		pMenu->AddOption(Localize("Leave team", "Menu Team"), "TEAM LEAVE", "<");
		if(pTeam->m_Leader == Account)
			pMenu->AddOption(Localize("Disband team", "Menu Team"), "TEAM DISBAND", "x");
	}

	pMenu->AddHorizontalRule();
	pMenu->AddOption(Localize("Online teams (invitation only):", "Menu Team"), "DISPLAY", "-");
	bool AnyTeam = false;
	for(int i = 0; i < pTeams->NumTeams(); i++)
	{
		const SGameTeam *pOther = pTeams->TeamAt(i);
		if(!pOther || !pTeams->HasOnlineMember(pOther))
			continue;
		AnyTeam = true;
		pMenu->AddOption(LocalizeFormat("Menu Team", "{}  ({} online / {} members)", pOther->m_aName,
					 pTeams->OnlineMemberCount(pOther), (int) pOther->m_lMembers.size())
					 .c_str(),
			"DISPLAY", "-");
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
