/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/item.h>

#include <base/system.h>

#include <game/server/gamecontext.h>

// ---------------------------------------------------------------
// crafting
//
// recipes support any number of inputs and outputs. tools are required but not
// consumed.
// ---------------------------------------------------------------

void CItemSystem::ForEachCraft(FCraftCallback pfnFunc, void *pUser)
{
	m_Crafts.for_each((hash_table<unsigned, SCraftDef, 8>::foreach_function) pfnFunc, pUser);
}

const CItemSystem::SCraftDef *CItemSystem::GetCraft(const char *pCraftId) const
{
	return m_Crafts.get(str_quickhash(pCraftId));
}

bool CItemSystem::ReserveIngredients(int ClientID, const SCraftDef *pCraft, int *pTake) const
{
	if(!pCraft || ClientID < 0 || ClientID >= MAX_CLIENTS)
		return false;

	const CInventory &Inventory = m_aInventories[ClientID];

	// available (still unreserved) quantity per inventory slot
	int aAvail[CInventory::MAX_ITEMS];
	for(int i = 0; i < CInventory::MAX_ITEMS; i++)
		aAvail[i] = Inventory.IsEmpty(i) ? 0 : Inventory.m_aItems[i].m_Count;

	// satisfy each ingredient against the same shared availability, so a single
	// physical item can never be used by two different materials
	for(int i = 0; i < pCraft->m_NumNeeded; i++)
	{
		const SIngredient &Need = pCraft->m_aNeeded[i];

		if(Need.m_IsTool)
		{
			bool Found = false;
			for(int k = 0; k < CInventory::MAX_ITEMS && !Found; k++)
			{
				if(Inventory.IsEmpty(k) || aAvail[k] <= 0)
					continue;
				if(Need.m_MatchByType)
					Found = HasItemType(Inventory.m_aItems[k].m_Id, Need.m_aType);
				else
					Found = Inventory.m_aItems[k].m_Id == Need.m_Id;
			}
			if(!Found)
				return false;
			continue;
		}

		int Remaining = Need.m_Count;
		for(int k = 0; k < CInventory::MAX_ITEMS && Remaining > 0; k++)
		{
			if(Inventory.IsEmpty(k) || aAvail[k] <= 0)
				continue;
			if(Need.m_MatchByType)
			{
				if(!HasItemType(Inventory.m_aItems[k].m_Id, Need.m_aType))
					continue;
			}
			else
			{
				if(Inventory.m_aItems[k].m_Id != Need.m_Id)
					continue;
			}
			const int Take = minimum(Remaining, aAvail[k]);
			aAvail[k] -= Take;
			Remaining -= Take;
		}
		if(Remaining > 0)
			return false;
	}

	// report what to remove per slot
	for(int k = 0; k < CInventory::MAX_ITEMS; k++)
		pTake[k] = (Inventory.IsEmpty(k) ? 0 : Inventory.m_aItems[k].m_Count) - aAvail[k];
	return true;
}

bool CItemSystem::HasIngredients(int ClientID, const SCraftDef *pCraft) const
{
	int aTake[CInventory::MAX_ITEMS];
	return ReserveIngredients(ClientID, pCraft, aTake);
}

CItemSystem::ECraftResult CItemSystem::Craft(int ClientID, const char *pCraftId)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return CRAFT_NO_MATERIALS;

	SCraftDef *pCraft = m_Crafts.get(str_quickhash(pCraftId));
	if(!pCraft)
		return CRAFT_NO_MATERIALS;

	int aTake[CInventory::MAX_ITEMS] = {0};
	if(!ReserveIngredients(ClientID, pCraft, aTake))
		return CRAFT_NO_MATERIALS;

	CInventory &Inventory = m_aInventories[ClientID];

	// simulate the whole operation on a copy first: consume the reserved
	// non-tool ingredients, then add every result. This lets an output stack
	// onto a slot that an ingredient just freed, and reports CRAFT_NO_SPACE
	// without ever mutating the real inventory.
	CInventory Simulated = Inventory;
	for(int k = 0; k < CInventory::MAX_ITEMS; k++)
	{
		if(aTake[k] <= 0)
			continue;
		Simulated.m_aItems[k].m_Count -= aTake[k];
		if(Simulated.m_aItems[k].m_Count <= 0)
			Simulated.ClearSlot(k);
	}
	for(int g = 0; g < pCraft->m_NumGives; g++)
	{
		const SResult &Give = pCraft->m_aGives[g];
		if(Give.m_Count <= 0)
			continue;
		if(!IsKnownItem(Give.m_Id))
			continue;
		if(!AddToInventory(Simulated, Give.m_Id, Give.m_Count, m_Items.get(Give.m_Id.Value())))
			return CRAFT_NO_SPACE;
	}

	// commit: consume the ingredients, then hand over the results
	for(int k = 0; k < CInventory::MAX_ITEMS; k++)
	{
		if(aTake[k] > 0)
			RemoveItem(ClientID, Inventory.m_aItems[k].m_Id, aTake[k]);
	}
	for(int g = 0; g < pCraft->m_NumGives; g++)
	{
		const SResult &Give = pCraft->m_aGives[g];
		if(Give.m_Count <= 0 || !IsKnownItem(Give.m_Id))
			continue;
		AddToInventory(Inventory, Give.m_Id, Give.m_Count, m_Items.get(Give.m_Id.Value()));
	}
	return CRAFT_OK;
}
