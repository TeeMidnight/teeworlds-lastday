/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/localization_format.h>
#include <game/server/currency.h>
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

int CTradeSystem::AddListing(int Seller, const CItemID &OfferItem, int OfferCount, int64 Price)
{
	if(!m_pGameServer || Seller < 0 || Seller >= MAX_CLIENTS)
		return -1;
	CLanguageScope LanguageScope(m_pGameServer->LanguageOf(Seller));
	if(OfferItem.IsZero() || OfferCount <= 0 || Price <= 0 || Price > CURRENCY_MAX_AMOUNT)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Seller, Localize("Invalid listing.", "Trade"));
		return -1;
	}

	CItemSystem *pItems = m_pGameServer->Item();
	CPlayer *pSeller = m_pGameServer->m_apPlayers[Seller];
	// A listing is paid out on the account ledger, so it needs an account to
	// be persisted against and to receive the money.
	if(!pSeller || pSeller->m_AccountUuid == UUID_ZEROED)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Seller, Localize("The bank is unavailable.", "Trade"));
		return -1;
	}
	if(!pItems->IsKnownItem(OfferItem))
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
	Listing.m_SellerAccount = pSeller->m_AccountUuid;
	Listing.m_OfferItem = OfferItem;
	Listing.m_OfferCount = OfferCount;
	Listing.m_Price = Price;
	m_lListings.add(Listing);
	Persist(Listing);

	const std::string Msg = LocalizeFormat("Trade", "Listed #{}: {} x{} for {} G",
		Listing.m_Id, OfferItem, OfferCount, Price);
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
	CPlayer *pSeller = Listing.m_Seller >= 0 ? m_pGameServer->m_apPlayers[Listing.m_Seller] : nullptr;
	if(!pSeller)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Buyer, Localize("The seller is offline.", "Trade"));
		return false;
	}

	CItemSystem *pItems = m_pGameServer->Item();
	CDatabase *pDB = m_pGameServer->Database();
	CPlayer *pBuyer = m_pGameServer->m_apPlayers[Buyer];
	if(!pBuyer || !pDB || pBuyer->m_AccountUuid == UUID_ZEROED || Listing.m_SellerAccount == UUID_ZEROED)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Buyer, Localize("The bank is unavailable.", "Trade"));
		return false;
	}

	// the balance is SUM(delta) over the ledger; nothing is cached in memory
	int64 Balance = 0;
	if(!pDB->GetCurrencyBalance(pBuyer->m_AccountUuid, &Balance) || Balance < Listing.m_Price)
	{
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Buyer, Localize("You cannot afford this.", "Trade"));
		return false;
	}

	// hand over the goods first: if the buyer has no room nothing has changed
	if(!pItems->AddItem(Buyer, Listing.m_OfferItem, Listing.m_OfferCount))
		return false;

	// Move the money on the ledger. The debit and the credit are one statement,
	// so a failed transfer leaves the buyer's balance untouched.
	char aItem[UUID_MAXSTRSIZE];
	const char *pItem = Listing.m_OfferItem.SourceName();
	if(!pItem[0])
	{
		Listing.m_OfferItem.FormatUuid(aItem, sizeof(aItem));
		pItem = aItem;
	}
	char aBuyerReason[CURRENCY_REASON_SIZE];
	char aSellerReason[CURRENCY_REASON_SIZE];
	str_format(aBuyerReason, sizeof(aBuyerReason), "buy listing #%d: %s x%d from %s",
		Listing.m_Id, pItem, Listing.m_OfferCount, m_pGameServer->Server()->ClientName(Listing.m_Seller));
	str_format(aSellerReason, sizeof(aSellerReason), "sell listing #%d: %s x%d to %s",
		Listing.m_Id, pItem, Listing.m_OfferCount, m_pGameServer->Server()->ClientName(Buyer));

	if(!pDB->TransferCurrency(pBuyer->m_AccountUuid, Listing.m_SellerAccount, Listing.m_Price, aBuyerReason, aSellerReason))
	{
		pItems->RemoveItem(Buyer, Listing.m_OfferItem, Listing.m_OfferCount);
		m_pGameServer->SendChat(-1, CHAT_WHISPER, Buyer, Localize("Payment failed.", "Trade"));
		return false;
	}

	m_lListings.remove_index(Index);
	Unpersist(ListingId);

	const std::string ToBuyer = LocalizeFormat("Trade", "Bought #{}: {} x{} for {} G",
		ListingId, Listing.m_OfferItem, Listing.m_OfferCount, Listing.m_Price);
	m_pGameServer->SendChat(-1, CHAT_WHISPER, Buyer, ToBuyer.c_str());
	{
		CLanguageScope SellerLanguageScope(m_pGameServer->LanguageOf(Listing.m_Seller));
		const std::string ToSeller = LocalizeFormat("Trade", "Sold #{}: {} x{} for {} G",
			ListingId, Listing.m_OfferItem, Listing.m_OfferCount, Listing.m_Price);
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
		// keep the id counter above every persisted row, even an ignored one,
		// so a new listing cannot reuse an id that is still in the table
		if(Row.m_Id >= m_NextId)
			m_NextId = Row.m_Id + 1;
		// a row without an account or a positive G price can never be bought
		if(Row.m_Seller == UUID_ZEROED || Row.m_Price <= 0)
			continue;
		SListing Listing;
		Listing.m_Id = Row.m_Id;
		// the seller is not connected yet; the listing is re-bound when the
		// account logs in again
		Listing.m_Seller = -1;
		Listing.m_SellerAccount = Row.m_Seller;
		CItemID::Parse(Row.m_aOfferItem, &Listing.m_OfferItem);
		Listing.m_OfferCount = Row.m_OfferCount;
		Listing.m_Price = Row.m_Price;
		m_lListings.add(Listing);
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
	Row.m_Price = Listing.m_Price;
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
