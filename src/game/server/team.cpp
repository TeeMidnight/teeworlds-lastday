/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>

#include "gamecontext.h"
#include "player.h"
#include "team.h"

namespace
{
void SetTeamError(char *pError, int ErrorSize, const char *pText)
{
	if(pError && ErrorSize > 0)
		str_copy(pError, pText, ErrorSize);
}
} // namespace

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
	return true;
}

bool CTeamSystem::JoinTeam(const Uuid &Account, int TeamId, char *pError, int ErrorSize)
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
	SGameTeam *pTeam = MutableTeamById(TeamId);
	if(!pTeam)
	{
		SetTeamError(pError, ErrorSize, "That team no longer exists.");
		return false;
	}
	if(m_pDatabase && !m_pDatabase->AddTeamMember(pTeam->m_Id, Account))
	{
		SetTeamError(pError, ErrorSize, "Could not join the team.");
		return false;
	}
	pTeam->m_lMembers.add(Account);
	m_MemberTeam.set(Account, pTeam);
	return true;
}

void CTeamSystem::ChooseNewLeader(SGameTeam *pTeam, const Uuid &Exclude)
{
	if(!pTeam || pTeam->m_lMembers.size() == 0)
		return;

	Uuid NewLeader = UUID_ZEROED;
	for(int i = 0; i < pTeam->m_lMembers.size(); i++)
	{
		const Uuid &Member = pTeam->m_lMembers[i];
		if(Member != Exclude && IsOnline(Member))
		{
			NewLeader = Member;
			break;
		}
	}
	if(NewLeader == UUID_ZEROED)
	{
		for(int i = 0; i < pTeam->m_lMembers.size(); i++)
			if(pTeam->m_lMembers[i] != Exclude)
			{
				NewLeader = pTeam->m_lMembers[i];
				break;
			}
	}
	if(NewLeader == UUID_ZEROED)
		return;

	pTeam->m_Leader = NewLeader;
	if(m_pDatabase)
		m_pDatabase->SetTeamLeader(pTeam->m_Id, NewLeader);
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
	if(m_pDatabase)
		m_pDatabase->RemoveTeamMember(pTeam->m_Id, Account);

	const bool WasLeader = pTeam->m_Leader == Account;
	for(int i = pTeam->m_lMembers.size() - 1; i >= 0; i--)
		if(pTeam->m_lMembers[i] == Account)
			pTeam->m_lMembers.remove_index(i);
	m_MemberTeam.remove(Account);

	if(pTeam->m_lMembers.size() == 0)
	{
		if(m_pDatabase)
			m_pDatabase->DeleteTeam(pTeam->m_Id);
		RemoveTeam(pTeam);
		return true;
	}
	if(WasLeader)
		ChooseNewLeader(pTeam, Account);
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
	SGameTeam *pTeam = MutableTeamFor(Account);
	if(!pTeam || pTeam->m_Leader != Account)
		return;
	// Hand over only when another member is online. Otherwise the offline
	// leader keeps the guild intact until they return.
	for(int i = 0; i < pTeam->m_lMembers.size(); i++)
	{
		const Uuid &Member = pTeam->m_lMembers[i];
		if(Member != Account && IsOnline(Member))
		{
			ChooseNewLeader(pTeam, Account);
			return;
		}
	}
}
