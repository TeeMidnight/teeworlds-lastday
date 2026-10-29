/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>

#include <game/localization_format.h>

#include "gamecontext.h"
#include "player.h"
#include "team.h"

static void SetTeamError(char *pError, int ErrorSize, const char *pText)
{
	if(pError && ErrorSize > 0)
		str_copy(pError, pText, ErrorSize);
}

// Send a system message to one client in that client's language. The notice is
// best effort: a client that is only connecting has no usable chat channel yet.
static void SendNotice(CGameContext *pGame, int ClientID, const char *pText)
{
	if(!pGame || ClientID < 0 || ClientID >= MAX_CLIENTS || !pGame->m_apPlayers[ClientID])
		return;
	if(!pGame->Server()->ClientIngame(ClientID))
		return;
	CLanguageScope LanguageScope(pGame->LanguageOf(ClientID));
	pGame->SendChat(-1, CHAT_ALL, ClientID, Localize(pText, "Team"));
}

static void SendNotice(CGameContext *pGame, int ClientID, const char *pFormat, const char *pArg)
{
	if(!pGame || ClientID < 0 || ClientID >= MAX_CLIENTS || !pGame->m_apPlayers[ClientID])
		return;
	if(!pGame->Server()->ClientIngame(ClientID))
		return;
	CLanguageScope LanguageScope(pGame->LanguageOf(ClientID));
	pGame->SendChat(-1, CHAT_ALL, ClientID, LocalizeFormat("Team", pFormat, pArg).c_str());
}

static void SendNotice(CGameContext *pGame, int ClientID, const char *pFormat, const char *pArg1, const char *pArg2)
{
	if(!pGame || ClientID < 0 || ClientID >= MAX_CLIENTS || !pGame->m_apPlayers[ClientID])
		return;
	if(!pGame->Server()->ClientIngame(ClientID))
		return;
	CLanguageScope LanguageScope(pGame->LanguageOf(ClientID));
	pGame->SendChat(-1, CHAT_ALL, ClientID, LocalizeFormat("Team", pFormat, pArg1, pArg2).c_str());
}

// Tell the online members of a team about a change. Exclude is the account that
// caused it (usually the one that already got a direct confirmation).
static void SendTeamNotice(CGameContext *pGame, const SGameTeam *pTeam, const Uuid &Exclude, const char *pText)
{
	if(!pGame || !pTeam)
		return;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = pGame->m_apPlayers[i];
		if(!pPlayer || pPlayer->m_AccountUuid == UUID_ZEROED || pPlayer->m_AccountUuid == Exclude)
			continue;
		if(pGame->Teams()->TeamFor(pPlayer->m_AccountUuid) != pTeam)
			continue;
		SendNotice(pGame, i, pText);
	}
}

static void SendTeamNotice(CGameContext *pGame, const SGameTeam *pTeam, const Uuid &Exclude, const char *pFormat, const char *pArg)
{
	if(!pGame || !pTeam)
		return;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = pGame->m_apPlayers[i];
		if(!pPlayer || pPlayer->m_AccountUuid == UUID_ZEROED || pPlayer->m_AccountUuid == Exclude)
			continue;
		if(pGame->Teams()->TeamFor(pPlayer->m_AccountUuid) != pTeam)
			continue;
		SendNotice(pGame, i, pFormat, pArg);
	}
}

CTeamSystem::CTeamSystem() :
	m_pGameServer(nullptr), m_pDatabase(nullptr)
{
}

CTeamSystem::~CTeamSystem()
{
	for(int i = 0; i < m_lTeams.size(); i++)
		delete m_lTeams[i];
	m_lTeams.clear();
}

void CTeamSystem::Init(CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
	m_pDatabase = pGameServer ? pGameServer->Database() : nullptr;

	for(int i = 0; i < m_lTeams.size(); i++)
		delete m_lTeams[i];
	m_lTeams.clear();
	m_TeamsByName.clear();
	m_MemberTeam.clear();

	if(!m_pDatabase)
		return;

	array<CDatabase::STeamData> lTeams;
	if(!m_pDatabase->ListTeams(lTeams))
		return;
	for(int i = 0; i < lTeams.size(); i++)
		AddLoadedTeam(lTeams[i]);
	dbg_msg("team", "loaded %d team(s) from the database", m_lTeams.size());
}

void CTeamSystem::AddLoadedTeam(const CDatabase::STeamData &Data)
{
	SGameTeam *pTeam = new SGameTeam();
	pTeam->m_Id = Data.m_Id;
	str_copy(pTeam->m_aName, Data.m_aName, sizeof(pTeam->m_aName));
	pTeam->m_Leader = Data.m_Leader;
	pTeam->m_FriendlyFire = Data.m_FriendlyFire;

	if(m_pDatabase)
		m_pDatabase->LoadTeamMembers(pTeam->m_Id, pTeam->m_lMembers);
	// a row whose member table is empty is still a team; the leader is the
	// fallback member so the guild never becomes unreachable
	if(pTeam->m_lMembers.size() == 0 && pTeam->m_Leader != UUID_ZEROED)
		pTeam->m_lMembers.add(pTeam->m_Leader);

	m_lTeams.add(pTeam);
	m_TeamsByName.set(str_quickhash(pTeam->m_aName), pTeam);
	for(int i = 0; i < pTeam->m_lMembers.size(); i++)
		m_MemberTeam.set(pTeam->m_lMembers[i], pTeam);
}

const SGameTeam *CTeamSystem::TeamFor(const Uuid &Account) const
{
	if(Account == UUID_ZEROED)
		return nullptr;
	SGameTeam *const *ppTeam = m_MemberTeam.get(Account);
	return ppTeam ? *ppTeam : nullptr;
}

SGameTeam *CTeamSystem::MutableTeamFor(const Uuid &Account)
{
	if(Account == UUID_ZEROED)
		return nullptr;
	SGameTeam *const *ppTeam = m_MemberTeam.get(Account);
	return ppTeam ? *ppTeam : nullptr;
}

const SGameTeam *CTeamSystem::FindByName(const char *pName) const
{
	return const_cast<CTeamSystem *>(this)->MutableTeamByName(pName);
}

SGameTeam *CTeamSystem::MutableTeamByName(const char *pName)
{
	if(!pName || !pName[0])
		return nullptr;
	SGameTeam *const *ppTeam = m_TeamsByName.get(str_quickhash(pName));
	if(!ppTeam || !*ppTeam || str_comp((*ppTeam)->m_aName, pName) != 0)
		return nullptr;
	return *ppTeam;
}

const SGameTeam *CTeamSystem::FindById(int Id) const
{
	return const_cast<CTeamSystem *>(this)->MutableTeamById(Id);
}

SGameTeam *CTeamSystem::MutableTeamById(int Id)
{
	for(int i = 0; i < m_lTeams.size(); i++)
		if(m_lTeams[i]->m_Id == Id)
			return m_lTeams[i];
	return nullptr;
}

SGameTeam *CTeamSystem::MutableInviteTeamFor(const Uuid &Account)
{
	if(Account == UUID_ZEROED)
		return nullptr;
	for(int i = 0; i < m_lTeams.size(); i++)
		if(m_lTeams[i]->IsInvited(Account))
			return m_lTeams[i];
	return nullptr;
}

const SGameTeam *CTeamSystem::InviteFrom(const Uuid &Account) const
{
	return const_cast<CTeamSystem *>(this)->MutableInviteTeamFor(Account);
}

void CTeamSystem::ClearInvitesFor(const Uuid &Account)
{
	if(Account == UUID_ZEROED)
		return;
	for(int i = 0; i < m_lTeams.size(); i++)
	{
		SGameTeam *pTeam = m_lTeams[i];
		for(int j = pTeam->m_lInvites.size() - 1; j >= 0; j--)
			if(pTeam->m_lInvites[j] == Account)
				pTeam->m_lInvites.remove_index(j);
	}
}

const char *CTeamSystem::TeamNameFor(const Uuid &Account) const
{
	const SGameTeam *pTeam = TeamFor(Account);
	return pTeam ? pTeam->m_aName : "";
}

bool CTeamSystem::SameTeam(const Uuid &A, const Uuid &B) const
{
	if(A == UUID_ZEROED || B == UUID_ZEROED)
		return false;
	const SGameTeam *pA = TeamFor(A);
	return pA && pA == TeamFor(B);
}

bool CTeamSystem::IsLeader(const Uuid &Account) const
{
	const SGameTeam *pTeam = TeamFor(Account);
	return pTeam && pTeam->m_Leader == Account;
}

bool CTeamSystem::IsOnline(const Uuid &Account) const
{
	if(!m_pGameServer || Account == UUID_ZEROED)
		return false;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = m_pGameServer->m_apPlayers[i];
		if(pPlayer && pPlayer->m_AccountUuid == Account)
			return true;
	}
	return false;
}

int CTeamSystem::ClientIDFor(const Uuid &Account) const
{
	if(!m_pGameServer || Account == UUID_ZEROED)
		return -1;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = m_pGameServer->m_apPlayers[i];
		if(pPlayer && pPlayer->m_AccountUuid == Account && m_pGameServer->Server()->ClientIngame(i))
			return i;
	}
	return -1;
}

const char *CTeamSystem::MemberName(const Uuid &Account, char *pBuf, int Size) const
{
	if(!pBuf || Size <= 0)
		return "";
	pBuf[0] = '\0';
	if(Account == UUID_ZEROED)
	{
		str_copy(pBuf, "?", Size);
		return pBuf;
	}

	const int ClientID = ClientIDFor(Account);
	if(ClientID >= 0)
	{
		str_copy(pBuf, m_pGameServer->Server()->ClientName(ClientID), Size);
		return pBuf;
	}

	if(m_pDatabase)
	{
		// The nickname is captured with the rest of the player profile on every
		// save, so this is the name the account last played under.
		if(m_pDatabase->GetJson(Account, CJsonPath().Key("nickname"), pBuf, Size) && pBuf[0])
			return pBuf;

		// Profiles written before the nickname field existed still have the
		// account name; a guest name is stored as "@guest:<nickname>".
		CDatabase::SPlayerData Row;
		if(m_pDatabase->LoadByUuid(Account, Row) && Row.m_aUsername[0])
		{
			const char *pName = Row.m_aUsername;
			if(str_startswith(pName, "@guest:"))
				pName += str_length("@guest:");
			if(pName[0])
			{
				str_copy(pBuf, pName, Size);
				return pBuf;
			}
		}
	}

	format_uuid(Account, pBuf, Size);
	return pBuf;
}

int CTeamSystem::OnlineMemberCount(const SGameTeam *pTeam) const
{
	if(!pTeam)
		return 0;
	int Count = 0;
	for(int i = 0; i < pTeam->m_lMembers.size(); i++)
		if(IsOnline(pTeam->m_lMembers[i]))
			Count++;
	return Count;
}

bool CTeamSystem::CreateTeam(const Uuid &Leader, const char *pName, char *pError, int ErrorSize)
{
	SetTeamError(pError, ErrorSize, "");
	if(Leader == UUID_ZEROED)
	{
		SetTeamError(pError, ErrorSize, "You have no player identity yet.");
		return false;
	}
	if(MutableTeamFor(Leader))
	{
		SetTeamError(pError, ErrorSize, "You are already in a team. Leave it first.");
		return false;
	}

	const char *pTrimmed = str_skip_whitespaces_const(pName ? pName : "");
	if(!pTrimmed[0])
	{
		SetTeamError(pError, ErrorSize, "Enter a team name.");
		return false;
	}

	char aName[64];
	str_utf8_copy_num(aName, pTrimmed, sizeof(aName), MAX_NAME_CHARS);
	if(!aName[0])
	{
		SetTeamError(pError, ErrorSize, "That team name is not usable.");
		return false;
	}
	if(MutableTeamByName(aName))
	{
		SetTeamError(pError, ErrorSize, "That team name is already taken.");
		return false;
	}

	CDatabase::STeamData Data;
	mem_zero(&Data, sizeof(Data));
	if(m_pDatabase)
	{
		if(!m_pDatabase->CreateTeam(aName, Leader, false, Data))
		{
			SetTeamError(pError, ErrorSize, "Could not create the team.");
			return false;
		}
	}
	else
	{
		Data.m_Id = 1;
		for(int i = 0; i < m_lTeams.size(); i++)
			Data.m_Id = maximum(Data.m_Id, m_lTeams[i]->m_Id + 1);
		str_copy(Data.m_aName, aName, sizeof(Data.m_aName));
		Data.m_Leader = Leader;
		Data.m_FriendlyFire = false;
	}

	SGameTeam *pTeam = new SGameTeam();
	pTeam->m_Id = Data.m_Id;
	str_copy(pTeam->m_aName, Data.m_aName, sizeof(pTeam->m_aName));
	pTeam->m_Leader = Leader;
	pTeam->m_FriendlyFire = false;
	pTeam->m_lMembers.add(Leader);
	m_lTeams.add(pTeam);
	m_TeamsByName.set(str_quickhash(pTeam->m_aName), pTeam);
	m_MemberTeam.set(Leader, pTeam);
	ClearInvitesFor(Leader);
	return true;
}

bool CTeamSystem::InviteMember(const Uuid &Leader, const Uuid &Target, char *pError, int ErrorSize)
{
	SetTeamError(pError, ErrorSize, "");
	SGameTeam *pTeam = MutableTeamFor(Leader);
	if(!pTeam || pTeam->m_Leader != Leader)
	{
		SetTeamError(pError, ErrorSize, "Only the leader can invite players.");
		return false;
	}
	if(Target == Leader)
	{
		SetTeamError(pError, ErrorSize, "You are already on the team.");
		return false;
	}
	if(Target == UUID_ZEROED)
	{
		SetTeamError(pError, ErrorSize, "That player has no player identity yet.");
		return false;
	}
	if(MutableTeamFor(Target))
	{
		SetTeamError(pError, ErrorSize, "That player is already in a team.");
		return false;
	}
	if(pTeam->IsInvited(Target))
	{
		SetTeamError(pError, ErrorSize, "That player already has a pending invitation.");
		return false;
	}

	const int TargetClientID = ClientIDFor(Target);
	if(TargetClientID < 0)
	{
		SetTeamError(pError, ErrorSize, "That player is not online.");
		return false;
	}

	char aLeaderName[128];
	const char *pLeaderName = MemberName(Leader, aLeaderName, sizeof(aLeaderName));

	pTeam->m_lInvites.add(Target);
	SendNotice(m_pGameServer, TargetClientID, "{} invited you to join team {}. Open the Team menu to accept or decline.", pLeaderName, pTeam->m_aName);
	return true;
}

bool CTeamSystem::CancelInvite(const Uuid &Leader, const Uuid &Target, char *pError, int ErrorSize)
{
	SetTeamError(pError, ErrorSize, "");
	SGameTeam *pTeam = MutableTeamFor(Leader);
	if(!pTeam || pTeam->m_Leader != Leader)
	{
		SetTeamError(pError, ErrorSize, "Only the leader can cancel an invitation.");
		return false;
	}
	bool Removed = false;
	for(int i = pTeam->m_lInvites.size() - 1; i >= 0; i--)
		if(pTeam->m_lInvites[i] == Target)
		{
			pTeam->m_lInvites.remove_index(i);
			Removed = true;
		}
	if(!Removed)
	{
		SetTeamError(pError, ErrorSize, "That player has no pending invitation.");
		return false;
	}

	const int TargetClientID = ClientIDFor(Target);
	if(TargetClientID >= 0)
		SendNotice(m_pGameServer, TargetClientID, "The invitation to team {} was withdrawn.", pTeam->m_aName);
	return true;
}

bool CTeamSystem::AcceptInvite(const Uuid &Account, char *pError, int ErrorSize)
{
	SetTeamError(pError, ErrorSize, "");
	if(Account == UUID_ZEROED)
	{
		SetTeamError(pError, ErrorSize, "You have no player identity yet.");
		return false;
	}
	if(MutableTeamFor(Account))
	{
		SetTeamError(pError, ErrorSize, "You are already in a team. Leave it first.");
		return false;
	}
	SGameTeam *pTeam = MutableInviteTeamFor(Account);
	if(!pTeam)
	{
		SetTeamError(pError, ErrorSize, "You have no pending team invitation.");
		return false;
	}
	if(m_pDatabase && !m_pDatabase->AddTeamMember(pTeam->m_Id, Account))
	{
		SetTeamError(pError, ErrorSize, "Could not join the team.");
		return false;
	}

	pTeam->m_lMembers.add(Account);
	m_MemberTeam.set(Account, pTeam);
	ClearInvitesFor(Account);

	char aName[128];
	const char *pName = MemberName(Account, aName, sizeof(aName));
	SendTeamNotice(m_pGameServer, pTeam, Account, "{} joined the team.", pName);
	return true;
}

bool CTeamSystem::DeclineInvite(const Uuid &Account, char *pError, int ErrorSize)
{
	SetTeamError(pError, ErrorSize, "");
	if(Account == UUID_ZEROED)
	{
		SetTeamError(pError, ErrorSize, "You have no player identity yet.");
		return false;
	}
	SGameTeam *pTeam = MutableInviteTeamFor(Account);
	if(!pTeam)
	{
		SetTeamError(pError, ErrorSize, "You have no pending team invitation.");
		return false;
	}

	ClearInvitesFor(Account);
	char aName[128];
	const char *pName = MemberName(Account, aName, sizeof(aName));
	const int LeaderClientID = ClientIDFor(pTeam->m_Leader);
	if(LeaderClientID >= 0)
		SendNotice(m_pGameServer, LeaderClientID, "{} declined the invitation to team {}.", pName, pTeam->m_aName);
	return true;
}

bool CTeamSystem::LeaveTeam(const Uuid &Account, char *pError, int ErrorSize)
{
	SetTeamError(pError, ErrorSize, "");
	SGameTeam *pTeam = MutableTeamFor(Account);
	if(!pTeam)
	{
		SetTeamError(pError, ErrorSize, "You are not in a team.");
		return false;
	}
	ClearInvitesFor(Account);

	// A guild belongs to its leader. Losing the leader would otherwise leave a
	// team nobody can manage, so the whole team disbands instead of handing the
	// guild to a random member.
	if(pTeam->m_Leader == Account)
	{
		SendTeamNotice(m_pGameServer, pTeam, Account, "The team was disbanded because the leader left.");
		if(m_pDatabase)
			m_pDatabase->DeleteTeam(pTeam->m_Id);
		RemoveTeam(pTeam);
		return true;
	}

	if(m_pDatabase)
		m_pDatabase->RemoveTeamMember(pTeam->m_Id, Account);
	for(int i = pTeam->m_lMembers.size() - 1; i >= 0; i--)
		if(pTeam->m_lMembers[i] == Account)
			pTeam->m_lMembers.remove_index(i);
	m_MemberTeam.remove(Account);
	return true;
}

bool CTeamSystem::KickMember(const Uuid &Leader, const Uuid &Target, char *pError, int ErrorSize)
{
	SetTeamError(pError, ErrorSize, "");
	SGameTeam *pTeam = MutableTeamFor(Leader);
	if(!pTeam || pTeam->m_Leader != Leader)
	{
		SetTeamError(pError, ErrorSize, "Only the leader can kick members.");
		return false;
	}
	if(Target == Leader)
	{
		SetTeamError(pError, ErrorSize, "The leader cannot be kicked.");
		return false;
	}
	bool HasLeader = false, HasTarget = false;
	for(int i = 0; i < pTeam->m_lMembers.size(); i++)
	{
		HasLeader = HasLeader || pTeam->m_lMembers[i] == Leader;
		HasTarget = HasTarget || pTeam->m_lMembers[i] == Target;
	}
	if(!HasLeader || !HasTarget)
	{
		SetTeamError(pError, ErrorSize, "That player is not on your team.");
		return false;
	}
	if(m_pDatabase)
		m_pDatabase->RemoveTeamMember(pTeam->m_Id, Target);
	for(int i = pTeam->m_lMembers.size() - 1; i >= 0; i--)
		if(pTeam->m_lMembers[i] == Target)
			pTeam->m_lMembers.remove_index(i);
	m_MemberTeam.remove(Target);
	return true;
}

bool CTeamSystem::SetFriendlyFire(const Uuid &Leader, bool Enabled, char *pError, int ErrorSize)
{
	SetTeamError(pError, ErrorSize, "");
	SGameTeam *pTeam = MutableTeamFor(Leader);
	if(!pTeam || pTeam->m_Leader != Leader)
	{
		SetTeamError(pError, ErrorSize, "Only the leader can change friendly fire.");
		return false;
	}
	pTeam->m_FriendlyFire = Enabled;
	if(m_pDatabase)
		m_pDatabase->SetTeamFriendlyFire(pTeam->m_Id, Enabled);
	return true;
}

bool CTeamSystem::DisbandTeam(const Uuid &Leader, char *pError, int ErrorSize)
{
	SetTeamError(pError, ErrorSize, "");
	SGameTeam *pTeam = MutableTeamFor(Leader);
	if(!pTeam || pTeam->m_Leader != Leader)
	{
		SetTeamError(pError, ErrorSize, "Only the leader can disband the team.");
		return false;
	}
	SendTeamNotice(m_pGameServer, pTeam, Leader, "The leader disbanded the team.");
	if(m_pDatabase)
		m_pDatabase->DeleteTeam(pTeam->m_Id);
	RemoveTeam(pTeam);
	return true;
}

void CTeamSystem::RemoveTeam(SGameTeam *pTeam)
{
	if(!pTeam)
		return;
	m_TeamsByName.remove(str_quickhash(pTeam->m_aName));
	for(int i = 0; i < pTeam->m_lMembers.size(); i++)
		m_MemberTeam.remove(pTeam->m_lMembers[i]);
	for(int i = 0; i < m_lTeams.size(); i++)
		if(m_lTeams[i] == pTeam)
		{
			m_lTeams.remove_index(i);
			break;
		}
	delete pTeam;
}

void CTeamSystem::OnPlayerDisconnected(const Uuid &Account)
{
	if(Account == UUID_ZEROED)
		return;

	SGameTeam *pTeam = MutableTeamFor(Account);
	if(!pTeam)
	{
		// An invitation is only an offer; drop it when the invited player leaves
		// so a later client slot cannot inherit it.
		ClearInvitesFor(Account);
		return;
	}

	if(pTeam->m_Leader != Account)
		return;

	// The leader is gone: the guild disbands, even if other members are online.
	// This is what makes "leader leaves" unambiguous and prevents a dead team
	// from lingering with an offline owner.
	SendTeamNotice(m_pGameServer, pTeam, Account, "The team was disbanded because the leader left.");
	if(m_pDatabase)
		m_pDatabase->DeleteTeam(pTeam->m_Id);
	RemoveTeam(pTeam);
}
