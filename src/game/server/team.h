/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_TEAM_H
#define GAME_SERVER_TEAM_H

#include <base/tl/array.h>
#include <base/tl/hashtable.h>
#include <base/uuid.h>
#include <game/server/database/database.h>

class CGameContext;

// A guild-style team. Membership is unlimited and keyed by account UUID; a guest
// has a nickname-derived UUID and therefore persists the same way.
struct SGameTeam
{
	int m_Id;
	char m_aName[64];
	Uuid m_Leader;
	bool m_FriendlyFire;
	array<Uuid> m_lMembers;
	// Invitations are session state: a leader offers membership to an online
	// player, and the player accepts or declines from the Team page. They are
	// deliberately not stored in SQL, so an offer cannot outlive the team it
	// belongs to.
	array<Uuid> m_lInvites;

	SGameTeam() : m_Id(0), m_FriendlyFire(false)
	{
		m_aName[0] = '\0';
		m_Leader = UUID_ZEROED;
	}

	bool IsInvited(const Uuid &Account) const
	{
		for(int i = 0; i < m_lInvites.size(); i++)
			if(m_lInvites[i] == Account)
				return true;
		return false;
	}
};

class CTeamSystem
{
public:
	CTeamSystem();
	~CTeamSystem();

	void Init(CGameContext *pGameServer);

	int NumTeams() const { return m_lTeams.size(); }
	const SGameTeam *TeamAt(int Index) const { return Index >= 0 && Index < m_lTeams.size() ? m_lTeams[Index] : nullptr; }
	const SGameTeam *TeamFor(const Uuid &Account) const;
	const SGameTeam *FindByName(const char *pName) const;
	const SGameTeam *FindById(int Id) const;

	const char *TeamNameFor(const Uuid &Account) const;
	bool SameTeam(const Uuid &A, const Uuid &B) const;
	bool IsMember(const Uuid &Account) const { return TeamFor(Account) != nullptr; }
	bool IsLeader(const Uuid &Account) const;
	int OnlineMemberCount(const SGameTeam *pTeam) const;
	bool HasOnlineMember(const SGameTeam *pTeam) const { return OnlineMemberCount(pTeam) > 0; }
	bool IsOnline(const Uuid &Account) const;

	// The client slot currently bound to the account, or -1. A connecting client
	// has no usable name/menu yet, so it is not returned.
	int ClientIDFor(const Uuid &Account) const;
	// Name to show for an account: the live server name while connected, the
	// nickname stored in the player's JSON data when offline, and the UUID only
	// as a last resort for rows that predate the nickname field.
	const char *MemberName(const Uuid &Account, char *pBuf, int Size) const;

	// The team that has a pending invitation for this account, or nullptr.
	const SGameTeam *InviteFrom(const Uuid &Account) const;

	// Player actions. Each returns false and fills pError on failure.
	bool CreateTeam(const Uuid &Leader, const char *pName, char *pError, int ErrorSize);
	bool InviteMember(const Uuid &Leader, const Uuid &Target, char *pError, int ErrorSize);
	bool CancelInvite(const Uuid &Leader, const Uuid &Target, char *pError, int ErrorSize);
	bool AcceptInvite(const Uuid &Account, char *pError, int ErrorSize);
	bool DeclineInvite(const Uuid &Account, char *pError, int ErrorSize);
	bool LeaveTeam(const Uuid &Account, char *pError, int ErrorSize);
	bool KickMember(const Uuid &Leader, const Uuid &Target, char *pError, int ErrorSize);
	bool SetFriendlyFire(const Uuid &Leader, bool Enabled, char *pError, int ErrorSize);
	bool DisbandTeam(const Uuid &Leader, char *pError, int ErrorSize);

	// Called when a player disconnects. A leader leaving disbands the guild;
	// pending offers addressed to the account are dropped.
	void OnPlayerDisconnected(const Uuid &Account);

	// maximum display characters in a team name (TeeInfo clan limit)
	static constexpr int MAX_NAME_CHARS = 12;

private:
	CGameContext *m_pGameServer;
	CDatabase *m_pDatabase;
	array<SGameTeam *> m_lTeams;
	hash_table<unsigned, SGameTeam *, 8> m_TeamsByName;
	hash_table<Uuid, SGameTeam *, 8, uuid_table_function> m_MemberTeam;

	SGameTeam *MutableTeamFor(const Uuid &Account);
	SGameTeam *MutableTeamByName(const char *pName);
	SGameTeam *MutableTeamById(int Id);
	SGameTeam *MutableInviteTeamFor(const Uuid &Account);
	void AddLoadedTeam(const CDatabase::STeamData &Data);
	void RemoveTeam(SGameTeam *pTeam);
	void ClearInvitesFor(const Uuid &Account);
};

#endif // GAME_SERVER_TEAM_H
