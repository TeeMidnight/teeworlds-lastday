/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
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
//	"use": { "health": 5, "forgetting": -5 }
//
// using the item consumes one copy and applies the declared effects to the
// player. "forgetting" is a delta on the mercury chapter meter.
// ---------------------------------------------------------------

bool CItemSystem::IsUsable(const CItemID &Id) const
{
	const SItemDef *pItem = m_Items.get(Id.Value());
	return pItem && pItem->m_Use.m_HasUse;
}

CItemSystem::SUse CItemSystem::GetUse(const CItemID &Id) const
{
	const SItemDef *pItem = m_Items.get(Id.Value());
	if(!pItem)
		return SUse();
	return pItem->m_Use;
}

int CItemSystem::UseItem(int ClientID, const CItemID &Id, int Count)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || Count <= 0)
		return 0;

	const int Owned = GetItemCount(ClientID, Id);
	if(Owned <= 0)
		return 0;
	if(Count > Owned)
		Count = Owned;

	const SUse Use = GetUse(Id);
	if(!Use.m_HasUse)
		return 0;

	CGameContext *pGameServer = m_pGameServer;
	CPlayer *pPlayer = pGameServer->m_apPlayers[ClientID];
	if(!pPlayer)
		return 0;

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
		if(Use.m_Forgetting != 0)
		{
			const int NewForgetting = clamp(pPlayer->m_Status.m_Forgetting + Use.m_Forgetting, 0, 100);
			if(NewForgetting != pPlayer->m_Status.m_Forgetting)
			{
				pPlayer->m_Status.m_Forgetting = NewForgetting;
				Applied = true;
			}
		}

		if(!Applied)
			break;

		if(!RemoveItem(ClientID, Id, 1))
			break;
		Used++;
	}

	if(Used == 0)
		return 0;

	pGameServer->SavePlayerData(pPlayer);
	return Used;
}
