/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/item.h>

#include <base/system.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>
#include <game/server/scripting/script_runtime.h>

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
	if(!pItem)
		return false;
	if(pItem->m_Use.m_HasUse)
		return true;
	return pItem->m_Module >= 0 && CScriptRuntime::Get()->HasFunction(pItem->m_Module, "on_use");
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

	const SItemDef *pItem = m_Items.get(Id.Value());
	if(!pItem)
		return 0;

	CGameContext *pGameServer = m_pGameServer;
	CPlayer *pPlayer = pGameServer->m_apPlayers[ClientID];
	if(!pPlayer)
		return 0;

	// Scripted items own their use behaviour. C++ still owns the inventory
	// transaction: on_use returns True when one copy should be consumed. The
	// legacy ctx.consumed flag is accepted as a fallback for older scripts.
	if(pItem->m_Module >= 0 && CScriptRuntime::Get()->HasFunction(pItem->m_Module, "on_use"))
	{
		int Used = 0;
		CCharacter *pChr = pGameServer->GetPlayerChar(ClientID);
		while(Used < Count)
		{
			SScriptContext Ctx;
			Ctx.m_pGameServer = pGameServer;
			Ctx.m_ClientID = ClientID;
			Ctx.m_WorldID = pChr ? pChr->GameWorld()->WorldID() : 0;
			Ctx.m_ItemId = Id;
			Ctx.m_Now = pGameServer->Server()->Tick();
			Ctx.m_TickSpeed = pGameServer->Server()->TickSpeed();
			Ctx.m_Pos = pChr ? pChr->GetPos() : vec2(0.0f, 0.0f);
			str_copy(Ctx.m_aEvent, "item_use", sizeof(Ctx.m_aEvent));
			if(!CScriptRuntime::Get()->Call(pItem->m_Module, "on_use", Ctx))
			{
				dbg_msg("items", "script error in '%s': %s", pItem->m_aScript, CScriptRuntime::Get()->LastError());
				break;
			}
			if(!Ctx.m_ReturnValue && !Ctx.m_Consumed)
				break;
			if(!RemoveItem(ClientID, Id, 1))
				break;
			Used++;
		}
		if(Used > 0)
			pGameServer->SavePlayerData(pPlayer);
		return Used;
	}

	const SUse Use = GetUse(Id);
	if(!Use.m_HasUse)
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

bool CItemSystem::IsPlaceable(const CItemID &Id) const
{
	const SItemDef *pItem = m_Items.get(Id.Value());
	if(!pItem || pItem->m_Module < 0)
		return false;
	return CScriptRuntime::Get()->HasFunction(pItem->m_Module, "on_place");
}

bool CItemSystem::PlaceItem(int ClientID, const CItemID &Id)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return false;
	if(GetItemCount(ClientID, Id) <= 0)
		return false;

	const SItemDef *pItem = m_Items.get(Id.Value());
	if(!pItem || !IsPlaceable(Id))
		return false;

	CGameContext *pGameServer = m_pGameServer;
	CPlayer *pPlayer = pGameServer->m_apPlayers[ClientID];
	if(!pPlayer)
		return false;

	CCharacter *pChr = pGameServer->GetPlayerChar(ClientID);
	SScriptContext Ctx;
	Ctx.m_pGameServer = pGameServer;
	Ctx.m_ClientID = ClientID;
	Ctx.m_WorldID = pChr ? pChr->GameWorld()->WorldID() : 0;
	Ctx.m_ItemId = Id;
	Ctx.m_Now = pGameServer->Server()->Tick();
	Ctx.m_TickSpeed = pGameServer->Server()->TickSpeed();
	Ctx.m_Pos = pChr ? pChr->GetPos() : vec2(0.0f, 0.0f);
	str_copy(Ctx.m_aEvent, "item_place", sizeof(Ctx.m_aEvent));

	if(!CScriptRuntime::Get()->Call(pItem->m_Module, "on_place", Ctx))
	{
		dbg_msg("items", "script error in '%s': %s", pItem->m_aScript, CScriptRuntime::Get()->LastError());
		return false;
	}
	// The script only builds the object; C++ still owns the inventory
	// transaction, exactly like on_use.
	if(!Ctx.m_ReturnValue)
		return false;
	if(!RemoveItem(ClientID, Id, 1))
		return false;

	pGameServer->SavePlayerData(pPlayer);
	return true;
}
