/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/math.h>
#include <base/system.h>
#include <game/collision.h>
#include <game/server/container.h>
#include <game/server/entities/character.h>
#include <game/server/entities/container.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/item.h>
#include <game/server/player.h>
#include <game/server/scripting/script_runtime.h>

#include <cmath>

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
	if(!pItem)
		return false;
	// container items place a native prop; the rest build through on_place
	if(pItem->m_aContainerKind[0])
		return true;
	if(pItem->m_Module < 0)
		return false;
	return CScriptRuntime::Get()->HasFunction(pItem->m_Module, "on_place");
}

// Find a floor tile in front of the player and return the container centre that
// rests on it. The box is the same one CContainerEntity draws; a solid tile or
// another container rejects the spot.
static bool FindContainerPlacement(CGameWorld *pWorld, CCharacter *pChr, int Capacity, vec2 *pOut)
{
	const vec2 Pos = pChr->GetPos();
	const vec2 Aim = pChr->AimDirection();
	const int Face = Aim.x >= 0.0f ? 1 : -1;
	const int TileX = (int) floorf(Pos.x / 32.0f) + Face;
	const int StartTileY = (int) floorf(Pos.y / 32.0f) - 1;
	const float Height = 18.0f + 6.0f * (float) Capacity;

	for(int ty = StartTileY; ty <= StartTileY + 6; ty++)
	{
		if(!pWorld->Collision()->CheckPoint(vec2((float) TileX * 32.0f + 16.0f, (float) ty * 32.0f + 16.0f)))
			continue;

		const float Bottom = (float) ty * 32.0f;
		const vec2 Center((float) TileX * 32.0f + 16.0f, Bottom - 16.0f);
		// the drawn box spans [Center.y - Height + 16, Center.y + 16]
		if(pWorld->Collision()->TestBox(vec2(Center.x, Bottom - Height / 2.0f), vec2(30.0f, Height - 2.0f)))
			continue;
		if(pWorld->ClosestEntity(Center, 32.0f, CGameWorld::ENTTYPE_CONTAINER, nullptr, true))
			continue;
		*pOut = Center;
		return true;
	}
	return false;
}

bool CItemSystem::PlaceContainer(int ClientID, const CItemID &Id)
{
	CGameContext *pGameServer = m_pGameServer;
	CPlayer *pPlayer = pGameServer->m_apPlayers[ClientID];
	CCharacter *pChr = pGameServer->GetPlayerChar(ClientID);
	if(!pPlayer || !pChr || !pChr->IsAlive())
	{
		pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("You have to be alive to place a container.", "Container"));
		return false;
	}
	if(!pChr->IsGrounded())
	{
		pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("Stand on the ground to place a container.", "Container"));
		return false;
	}

	const char *pKind = GetContainerKind(Id);
	CContainerSystem *pContainers = pGameServer->Containers();
	const SContainerDef *pDef = pContainers ? pContainers->Find(pKind) : nullptr;
	if(!pDef)
	{
		pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("Containers are unavailable.", "Container"));
		return false;
	}

	vec2 Pos;
	if(!FindContainerPlacement(pChr->GameWorld(), pChr, pDef->m_Capacity, &Pos))
	{
		pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("There is no room for a container here.", "Container"));
		return false;
	}

	if(!RemoveItem(ClientID, Id, 1))
		return false;

	CGameWorld *pWorld = pChr->GameWorld();
	CContainerEntity *pContainer = new CContainerEntity(pWorld, Pos, pDef->m_aKind, pDef->m_aName,
		"", pDef->m_Capacity, pDef->m_Pickup, pWorld->NextPlacedContainerStorageID());
	// a placed container starts owned-empty, never with rolled loot
	pContainer->SetInitialized(true);

	pGameServer->SavePlayerData(pPlayer);
	pGameServer->SendChat(-1, CHAT_WHISPER, ClientID, Localize("Container placed.", "Container"));
	return true;
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

	if(pItem->m_aContainerKind[0])
		return PlaceContainer(ClientID, Id);

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
