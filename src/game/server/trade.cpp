/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/server/trade.h>

#include <game/localization_format.h>
#include <game/server/gamecontext.h>
#include <game/server/item.h>
#include <game/server/player.h>

CTradeSystem::CTradeSystem() :
	m_pGameServer(nullptr),
	m_NextId(1)
{
}

void CTradeSystem::Init(CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
	m_lListings.clear();
}

int CTradeSystem::FindIndex(int ListingId) const
{
	for(int i = 0; i < m_lListings.size(); i++)
		if(m_lListings[i].m_Id == ListingId)
			return i;
	return -1;
}

const CTradeSystem::SListing *CTradeSystem::Find(int ListingId) const
{
	const int Index = FindIndex(ListingId);
	return Index >= 0 ? &m_lListings[Index] : nullptr;
}

int CTradeSystem::AddListing(int Seller, const CItemID &OfferItem, int OfferCount, const CItemID &PriceItem, int PriceCount)
{
	if(!m_pGameServer || Seller < 0 || Seller >= MAX_CLIENTS)
		return -1;
	if(OfferItem.IsZero() || PriceItem.IsZero() || OfferCount <= 0 || PriceCount <= 0)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Seller, Localize("Usage: /sell <item> <count> <price item> <price count>", "Trade"));
		return -1;
	}

	CItemSystem *pItems = m_pGameServer->Item();
	if(!pItems->IsKnownItem(OfferItem) || !pItems->IsKnownItem(PriceItem))
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Seller, Localize("Unknown item.", "Trade"));
		return -1;
	}
	if(pItems->GetItemCount(Seller, OfferItem) < OfferCount)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Seller, Localize("You do not have that many.", "Trade"));
		return -1;
	}
	// escrow the offered stack; this can only fail if the count changed under us
	if(!pItems->RemoveItem(Seller, OfferItem, OfferCount))
		return -1;

	SListing Listing;
	Listing.m_Id = m_NextId++;
	Listing.m_Seller = Seller;
	Listing.m_OfferItem = OfferItem;
	Listing.m_OfferCount = OfferCount;
	Listing.m_PriceItem = PriceItem;
	Listing.m_PriceCount = PriceCount;
	m_lListings.add(Listing);

	const std::string Msg = LocalizeFormat("Trade", "Listed #{}: {} x{} for {} x{}",
		Listing.m_Id, OfferItem, OfferCount, PriceItem, PriceCount);
	m_pGameServer->SendChat(-1, CHAT_WHISPER, Seller, Msg.c_str());
	return Listing.m_Id;
}

bool CTradeSystem::RemoveListing(int ListingId, int ByClient, bool Force)
{
	const int Index = FindIndex(ListingId);
	if(Index < 0)
		return false;
	const SListing Listing = m_lListings[Index];
	if(!Force && Listing.m_Seller != ByClient)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, ByClient, Localize("That is not your listing.", "Trade"));
		return false;
	}

	CItemSystem *pItems = m_pGameServer->Item();
	// return the escrowed goods first; if the seller has no space the listing
	// stays valid instead of silently losing the items
	if(!pItems->AddItem(Listing.m_Seller, Listing.m_OfferItem, Listing.m_OfferCount))
		return false;

	m_lListings.remove_index(Index);
	if(!Force)
		m_pGameServer->SendChat(-1, CHAT_WHISPER, ByClient, Localize("Listing removed.", "Trade"));
	return true;
}

void CTradeSystem::RemoveListingsOf(int ClientID)
{
	// remove from the end so indices stay valid; a full inventory is ignored
	// because the client is leaving anyway
	for(int i = m_lListings.size() - 1; i >= 0; i--)
	{
		if(m_lListings[i].m_Seller != ClientID)
			continue;
		CItemSystem *pItems = m_pGameServer->Item();
		pItems->AddItem(ClientID, m_lListings[i].m_OfferItem, m_lListings[i].m_OfferCount, true);
		m_lListings.remove_index(i);
	}
}

bool CTradeSystem::Buy(int Buyer, int ListingId)
{
	const int Index = FindIndex(ListingId);
	if(Index < 0)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Buyer, Localize("No such listing.", "Trade"));
		return false;
	}
	const SListing Listing = m_lListings[Index];
	if(Listing.m_Seller == Buyer)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Buyer, Localize("You cannot buy your own listing.", "Trade"));
		return false;
	}

	CItemSystem *pItems = m_pGameServer->Item();
	if(pItems->GetItemCount(Buyer, Listing.m_PriceItem) < Listing.m_PriceCount)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Buyer, Localize("You cannot afford this.", "Trade"));
		return false;
	}

	// hand over the goods first: if the buyer has no room nothing has changed
	if(!pItems->AddItem(Buyer, Listing.m_OfferItem, Listing.m_OfferCount))
		return false;
	// now take the payment (the count was checked above)
	if(!pItems->RemoveItem(Buyer, Listing.m_PriceItem, Listing.m_PriceCount))
	{
		pItems->RemoveItem(Buyer, Listing.m_OfferItem, Listing.m_OfferCount);
		return false;
	}
	// pay the seller; if their inventory is full the items are queued silently
	pItems->AddItem(Listing.m_Seller, Listing.m_PriceItem, Listing.m_PriceCount, true);

	m_lListings.remove_index(Index);

	const std::string ToBuyer = LocalizeFormat("Trade", "Bought #{}: {} x{}", ListingId, Listing.m_OfferItem, Listing.m_OfferCount);
	m_pGameServer->SendChat(-1, CHAT_WHISPER, Buyer, ToBuyer.c_str());
	const std::string ToSeller = LocalizeFormat("Trade", "Sold #{}: {} x{} for {} x{}",
		ListingId, Listing.m_OfferItem, Listing.m_OfferCount, Listing.m_PriceItem, Listing.m_PriceCount);
	m_pGameServer->SendChat(-1, CHAT_WHISPER, Listing.m_Seller, ToSeller.c_str());
	return true;
}
