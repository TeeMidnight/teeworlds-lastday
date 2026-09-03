#include <game/server/item.h>

#include <base/system.h>
#include <engine/shared/jsonparser.h>
#include <engine/storage.h>

#include <game/server/gamecontext.h>

// ---------------------------------------------------------------
// crafting
//
// recipes are loaded from datasrc/craft/*.json (the loader is shared with
// the item loader in the main file) and executed here.
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

	// try to satisfy each ingredient against the same shared availability, so a
	// single physical item can never be used by two different materials
	for(int i = 0; i < pCraft->m_NumNeeded; i++)
	{
		const SIngredient &Need = pCraft->m_aNeeded[i];

		if(Need.m_IsTool)
		{
			// a tool must simply be owned; it is never consumed
			bool Found = false;
			for(int k = 0; k < CInventory::MAX_ITEMS && !Found; k++)
			{
				if(Inventory.IsEmpty(k) || aAvail[k] <= 0)
					continue;
				if(Need.m_MatchByType)
					Found = HasItemType(Inventory.m_aItems[k].m_aResId, Need.m_aType);
				else
					Found = str_comp(Inventory.m_aItems[k].m_aResId, Need.m_aItemId) == 0;
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
				if(!HasItemType(Inventory.m_aItems[k].m_aResId, Need.m_aType))
					continue;
			}
			else
			{
				if(str_comp(Inventory.m_aItems[k].m_aResId, Need.m_aItemId) != 0)
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

	// separate the failure reasons: missing materials vs. no room for the result
	if(!HasIngredients(ClientID, pCraft))
		return CRAFT_NO_MATERIALS;

	CInventory &Inventory = m_aInventories[ClientID];
	// separate "no free slot for the result" (find by id first: stacking the
	// result onto an existing stack needs no free slot)
	if(Inventory.Find(pCraft->m_aResultItemId) < 0 &&
		Inventory.FindEmpty() < 0)
		return CRAFT_NO_SPACE;

	// consume the reserved non-tool ingredients; tools are kept. RemoveItem
	// only clears the emptied slot and never moves the others, so removing
	// by index stays valid across the loop
	int aTake[CInventory::MAX_ITEMS] = {0};
	if(!ReserveIngredients(ClientID, pCraft, aTake))
		return CRAFT_NO_MATERIALS;
	for(int k = 0; k < CInventory::MAX_ITEMS; k++)
		if(aTake[k] > 0)
			RemoveItem(ClientID, Inventory.m_aItems[k].m_aResId, aTake[k]);

	// the slot was already checked above, so a silent add cannot fail again
	AddItem(ClientID, pCraft->m_aResultItemId, pCraft->m_ResultCount, true);
	return CRAFT_OK;
}
