/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <engine/shared/config.h>
#include <engine/shared/memheap.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/gamemenu.h>
#include <game/server/player.h>

// -- server vote ---------------------------------------------------------
//
// The menu and the server vote share one protocol: the vote option list packet
// carries both the menu pages and the server's vote options. The vote state
// lives on CGameMenu (declared in gamemenu.h) so the two cannot drift apart;
// this file holds its implementation.

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
