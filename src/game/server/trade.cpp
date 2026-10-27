/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/localization_format.h>
#include <game/server/database/database.h>
#include <game/server/gamecontext.h>
#include <game/server/item.h>
#include <game/server/player.h>
#include <game/server/trade.h>

CTradeSystem::CTradeSystem() :
	m_pGameServer(nullptr),
	m_NextId(1)
{
}

void CTradeSystem::Init(CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
	m_lListings.clear();
	LoadFromDatabase();
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
	CLanguageScope LanguageScope(m_pGameServer->LanguageOf(Seller));
	if(OfferItem.IsZero() || PriceItem.IsZero() || OfferCount <= 0 || PriceCount <= 0)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Seller, Localize("Invalid listing.", "Trade"));
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
	CPlayer *pSeller = m_pGameServer->m_apPlayers[Seller];
	Listing.m_SellerAccount = pSeller ? pSeller->m_AccountUuid : UUID_ZEROED;
	Listing.m_OfferItem = OfferItem;
	Listing.m_OfferCount = OfferCount;
	Listing.m_PriceItem = PriceItem;
	Listing.m_PriceCount = PriceCount;
	m_lListings.add(Listing);
	Persist(Listing);

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
	CLanguageScope LanguageScope(m_pGameServer->LanguageOf(ByClient));
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
	Unpersist(Listing.m_Id);
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
	CLanguageScope BuyerLanguageScope(m_pGameServer->LanguageOf(Buyer));
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
	if(Listing.m_Seller < 0)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Buyer, Localize("The seller is offline.", "Trade"));
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
	// pay the seller. If their inventory is full the purchase is rolled back
	// rather than losing the payment, and the listing stays up.
	if(!pItems->AddItem(Listing.m_Seller, Listing.m_PriceItem, Listing.m_PriceCount, true))
	{
		pItems->RemoveItem(Buyer, Listing.m_OfferItem, Listing.m_OfferCount);
		pItems->AddItem(Buyer, Listing.m_PriceItem, Listing.m_PriceCount, true);
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Buyer, Localize("The seller has no inventory space.", "Trade"));
		return false;
	}

	m_lListings.remove_index(Index);
	Unpersist(ListingId);

	const std::string ToBuyer = LocalizeFormat("Trade", "Bought #{}: {} x{}", ListingId, Listing.m_OfferItem, Listing.m_OfferCount);
	m_pGameServer->SendChat(-1, CHAT_WHISPER, Buyer, ToBuyer.c_str());
	{
		CLanguageScope SellerLanguageScope(m_pGameServer->LanguageOf(Listing.m_Seller));
		const std::string ToSeller = LocalizeFormat("Trade", "Sold #{}: {} x{} for {} x{}",
			ListingId, Listing.m_OfferItem, Listing.m_OfferCount, Listing.m_PriceItem, Listing.m_PriceCount);
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Listing.m_Seller, ToSeller.c_str());
	}
	return true;
}

void CTradeSystem::LoadFromDatabase()
{
	CDatabase *pDB = m_pGameServer ? m_pGameServer->Database() : nullptr;
	if(!pDB)
		return;

	array<CDatabase::SShopListing> lRows;
	if(!pDB->LoadShopListings(lRows))
		return;

	for(int i = 0; i < lRows.size(); i++)
	{
		const CDatabase::SShopListing &Row = lRows[i];
		SListing Listing;
		Listing.m_Id = Row.m_Id;
		// the seller is not connected yet; the listing is re-bound when the
		// account logs in again
		Listing.m_Seller = -1;
		Listing.m_SellerAccount = Row.m_Seller;
		CItemID::Parse(Row.m_aOfferItem, &Listing.m_OfferItem);
		Listing.m_OfferCount = Row.m_OfferCount;
		CItemID::Parse(Row.m_aPriceItem, &Listing.m_PriceItem);
		Listing.m_PriceCount = Row.m_PriceCount;
		m_lListings.add(Listing);
		if(Listing.m_Id >= m_NextId)
			m_NextId = Listing.m_Id + 1;
	}

	if(lRows.size())
		dbg_msg("trade", "restored %d shop listing(s)", lRows.size());
}

void CTradeSystem::Persist(const SListing &Listing)
{
	CDatabase *pDB = m_pGameServer ? m_pGameServer->Database() : nullptr;
	if(!pDB || Listing.m_SellerAccount == UUID_ZEROED)
		return;

	CDatabase::SShopListing Row;
	mem_zero(&Row, sizeof(Row));
	Row.m_Id = Listing.m_Id;
	Row.m_Seller = Listing.m_SellerAccount;
	str_copy(Row.m_aOfferItem, Listing.m_OfferItem.SourceName(), sizeof(Row.m_aOfferItem));
	Row.m_OfferCount = Listing.m_OfferCount;
	str_copy(Row.m_aPriceItem, Listing.m_PriceItem.SourceName(), sizeof(Row.m_aPriceItem));
	Row.m_PriceCount = Listing.m_PriceCount;
	pDB->SaveShopListing(Row);
}

void CTradeSystem::Unpersist(int ListingId)
{
	CDatabase *pDB = m_pGameServer ? m_pGameServer->Database() : nullptr;
	if(pDB)
		pDB->DeleteShopListing(ListingId);
}

void CTradeSystem::RebindSeller(int ClientID, const Uuid &Account)
{
	if(Account == UUID_ZEROED)
		return;
	for(int i = 0; i < m_lListings.size(); i++)
		if(m_lListings[i].m_SellerAccount == Account)
			m_lListings[i].m_Seller = ClientID;
}

void CTradeSystem::DetachSeller(int ClientID)
{
	for(int i = 0; i < m_lListings.size(); i++)
		if(m_lListings[i].m_Seller == ClientID)
			m_lListings[i].m_Seller = -1;
}

