#include <game/server/item.h>

#include <base/system.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>

// ---------------------------------------------------------------
// item "use" effects
//
// an item can declare an optional "use" object in its json definition:
//
//	"use": { "health": 5, "sanity": 10 }
//
// using the item consumes one copy and applies the declared effects to the
// player. the use effects are parsed in CItemSystem::LoadItem (main file),
// the runtime behaviour lives here.
// ---------------------------------------------------------------

bool CItemSystem::IsUsable(const char *pResId) const
{
	const SItemDef *pItem = m_Items.get(str_quickhash(pResId));
	return pItem && pItem->m_Use.m_HasUse;
}

CItemSystem::SUse CItemSystem::GetUse(const char *pResId) const
{
	const SItemDef *pItem = m_Items.get(str_quickhash(pResId));
	if(!pItem)
		return SUse();
	return pItem->m_Use;
}

int CItemSystem::UseItem(int ClientID, const char *pResId, int Count)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || Count <= 0)
		return 0;

	if(GetItemCount(ClientID, pResId) <= 0)
		return 0;

	const SUse Use = GetUse(pResId);
	if(!Use.m_HasUse)
		return 0;

	CGameContext *pGameServer = m_pGameServer;
	CPlayer *pPlayer = pGameServer->m_apPlayers[ClientID];
	if(!pPlayer)
		return 0;

	// apply the effect repeatedly until the requested amount is used up or
	// nothing is left to restore (full health/sanity)
	int Used = 0;
	while(Used < Count)
	{
		bool Applied = false;
		if(Use.m_Health != 0)
		{
			CCharacter *pChr = pGameServer->GetPlayerChar(ClientID);
			if(pChr && pChr->IncreaseHealth(Use.m_Health))
				Applied = true;
		}
		if(Use.m_Sanity != 0)
		{
			const int NewSanity = clamp(pPlayer->m_Status.m_Sanity + Use.m_Sanity, 0, 100);
			if(NewSanity != pPlayer->m_Status.m_Sanity)
			{
				pPlayer->m_Status.m_Sanity = NewSanity;
				Applied = true;
			}
		}

		if(!Applied)
			break; // nothing to restore, keep the remaining items

		Used++;
	}

	if(Used == 0)
		return 0;

	// consume what was actually used; RemoveItem drops the stack completely
	// once it reaches zero
	RemoveItem(ClientID, pResId, Used);

	// the used-up status should be persisted on the next save
	pGameServer->SavePlayerData(pPlayer);
	return Used;
}
