/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_BUILDING_H
#define GAME_SERVER_BUILDING_H

#include <base/system.h>
#include <base/uuid.h>

// Who may use a placed building. The lock never changes the owner; only the
// owner may change the mode.
enum EBuildLock
{
	BUILD_LOCK_PUBLIC = 0,
	BUILD_LOCK_SELF,
	BUILD_LOCK_TEAM,
};

struct CBuildAccess
{
	Uuid m_Owner;
	int m_LockMode;

	CBuildAccess() : m_LockMode(BUILD_LOCK_SELF) { m_Owner = UUID_ZEROED; }
};

inline const char *BuildLockName(int Mode)
{
	switch(Mode)
	{
		case BUILD_LOCK_PUBLIC: return "public";
		case BUILD_LOCK_SELF: return "self";
		case BUILD_LOCK_TEAM: return "team";
	}
	return "self";
}

inline bool BuildLockFromName(const char *pName, int *pMode)
{
	if(!pName || !pMode)
		return false;
	if(str_comp_nocase(pName, "public") == 0 || str_comp_nocase(pName, "all") == 0)
		*pMode = BUILD_LOCK_PUBLIC;
	else if(str_comp_nocase(pName, "self") == 0 || str_comp_nocase(pName, "private") == 0)
		*pMode = BUILD_LOCK_SELF;
	else if(str_comp_nocase(pName, "team") == 0 || str_comp_nocase(pName, "guild") == 0)
		*pMode = BUILD_LOCK_TEAM;
	else
		return false;
	return true;
}

#endif // GAME_SERVER_BUILDING_H
