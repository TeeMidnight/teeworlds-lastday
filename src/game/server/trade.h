/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_TRADE_H
#define GAME_SERVER_TRADE_H

#include <base/tl/array.h>
#include <base/uuid.h>
#include <game/item_id.h>

class CGameContext;

// Player owned shops. A player puts their own goods up for sale at a price in
// currency ("G"): the price is never another item, and the money is moved on
// the account ledger in the database, not through the inventory. The offered
// stack is moved into escrow when the listing is created, so a listing can
// never promise items the seller no longer has.
class CTradeSystem
{
public:
	struct SListing
	{
		int m_Id;
		int m_Seller; // ClientID, or -1 while the seller is offline
		Uuid m_SellerAccount; // the account the listing belongs to, used to persist it
		CItemID m_OfferItem;
		int m_OfferCount;
		int64 m_Price; // price in G
	};

private:
	CGameContext *m_pGameServer;
	array<SListing> m_lListings;
	int m_NextId;

	int FindIndex(int ListingId) const;

	// restore the persisted listings and keep the table in step with the list
	void LoadFromDatabase();
	void Persist(const SListing &Listing);
	void Unpersist(int ListingId);

public:
	CTradeSystem();

	void Init(CGameContext *pGameServer);

	// Put OfferCount of OfferItem into escrow and list it for Price (G).
	// Returns the listing id, or -1 on failure (the player already got a chat
	// message explaining why).
	int AddListing(int Seller, const CItemID &OfferItem, int OfferCount, int64 Price);
	// Return the escrowed goods to the seller (or, with Force, drop them when
	// the owner is gone) and delete the listing.
	bool RemoveListing(int ListingId, int ByClient, bool Force);
	// Called on disconnect: every listing of that player is returned to them.
	void RemoveListingsOf(int ClientID);
	// Re-bind the persisted listings of an account to the client slot that just
	// logged in, so an offline listing becomes buyable again.
	void RebindSeller(int ClientID, const Uuid &Account);
	// Forget the client slot of a disconnecting seller: the listing and its
	// escrowed goods stay (they persist), but the slot must not stay attached.
	void DetachSeller(int ClientID);
	// Transfer the escrowed goods to the buyer and the payment to the seller.
	bool Buy(int Buyer, int ListingId);

	int NumListings() const { return m_lListings.size(); }
	const SListing *ListingAt(int Index) const { return Index >= 0 && Index < m_lListings.size() ? &m_lListings[Index] : nullptr; }
	const SListing *Find(int ListingId) const;
};

#endif // GAME_SERVER_TRADE_H
