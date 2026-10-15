/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <game/localization_format.h>

#include <game/server/gamecontext.h>
#include <game/server/gamemenu.h>
#include <game/server/item.h>
#include <game/server/npcdata.h>
#include <game/server/player.h>
#include <game/server/trade.h>

#include "item_menu.h"

// A shop provider's stable key for the "TRADEOPEN" command. Listings of one
// seller share a key; offline sellers fall into one "(offline)" group.
static void TradeProviderKey(int Seller, char *pBuf, int Size)
{
	if(Seller >= 0)
		str_format(pBuf, Size, "P%d", Seller);
	else
		str_copy(pBuf, "POFF", Size);
}

bool CGameMenu::MenuTrade(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	if(VoteStatus.m_aCmd[0])
	{
		if(str_comp(VoteStatus.m_aCmd, "PAGE TRADE_SELL") == 0)
		{
			pMenu->m_aTradeDraft[ClientID] = STradeDraft();
			pMenu->SetPlayerPage(ClientID, "TRADE_SELL");
			return false;
		}
		if(str_comp(VoteStatus.m_aCmd, "PAGE TRADE_MINE") == 0)
		{
			pMenu->SetPlayerPage(ClientID, "TRADE_MINE");
			return false;
		}
		// clicking a provider folds its goods open or shut (same page)
		if(str_startswith(VoteStatus.m_aCmd, "TRADEOPEN "))
		{
			const char *pKey = VoteStatus.m_aCmd + str_length("TRADEOPEN ");
			if(str_comp(pMenu->m_aTradeOpen[ClientID], pKey) == 0)
				pMenu->m_aTradeOpen[ClientID][0] = '\0';
			else
				str_copy(pMenu->m_aTradeOpen[ClientID], pKey, sizeof(pMenu->m_aTradeOpen[ClientID]));
			// fall through and redraw the page with the new expansion
		}
		// clicking a listing buys it. The command is "BUY <id>".
		else if(str_startswith(VoteStatus.m_aCmd, "BUY "))
		{
			const int ListingId = str_toint(VoteStatus.m_aCmd + 4);
			pMenu->GameServer()->Trade()->Buy(ClientID, ListingId);
			pMenu->SetPlayerPage(ClientID, "TRADE");
			return false;
		}
		// clicking an NPC shelf entry buys it: "BUYNPC <def> <sell>".
		else if(str_startswith(VoteStatus.m_aCmd, "BUYNPC "))
		{
			int DefIndex = 0;
			int SellIndex = 0;
			if(sscanf(VoteStatus.m_aCmd + 7, "%d %d", &DefIndex, &SellIndex) == 2)
				pMenu->GameServer()->BuyFromNpc(ClientID, DefIndex, SellIndex);
			pMenu->SetPlayerPage(ClientID, "TRADE");
			return false;
		}
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();
	pMenu->AddOption(Localize("Sell Goods", "Menu Trade"), "PAGE TRADE_SELL", "★");
	pMenu->AddOption(Localize("My Listings", "Menu Trade"), "PAGE TRADE_MINE", "★");
	pMenu->AddHorizontalRule();
	pMenu->AddOption(Localize("Click a merchant to see their goods.", "Menu Trade"), "DISPLAY", "-");

	const char *pOpen = pMenu->m_aTradeOpen[ClientID];
	const bool OpenRequested = pOpen[0] != '\0';
	bool Any = false;
	bool OpenFound = false;
	int ProviderIndex = 0;

	// player listings, grouped by seller: one row per provider, its goods only
	// when that provider is expanded
	CTradeSystem *pTrade = pMenu->GameServer()->Trade();
	if(pTrade)
	{
		for(int i = 0; i < pTrade->NumListings(); i++)
		{
			const CTradeSystem::SListing *pListing = pTrade->ListingAt(i);
			if(!pListing)
				continue;
			// skip when an earlier listing already introduced this seller
			bool Seen = false;
			for(int k = 0; k < i && !Seen; k++)
			{
				const CTradeSystem::SListing *pPrev = pTrade->ListingAt(k);
				Seen = pPrev && pPrev->m_Seller == pListing->m_Seller;
			}
			if(Seen)
				continue;

			char aKey[16];
			TradeProviderKey(pListing->m_Seller, aKey, sizeof(aKey));
			const bool Open = str_comp(pOpen, aKey) == 0;

			// count the offers so the header can show the merchant's size
			int Offered = 0;
			for(int k = 0; k < pTrade->NumListings(); k++)
			{
				const CTradeSystem::SListing *pOther = pTrade->ListingAt(k);
				if(pOther && pOther->m_Seller == pListing->m_Seller)
					Offered++;
			}

			const char *pSeller = pListing->m_Seller >= 0 ? pMenu->Server()->ClientName(pListing->m_Seller) : Localize("(offline)", "Menu Trade");
			// the ordinal keeps every header unique: the client looks an option
			// up by its displayed text, so duplicate names must not collide
			const std::string Head = LocalizeFormat("Menu Trade", "{}. {} ({})", ++ProviderIndex, pSeller, Offered);
			OpenFound = OpenFound || Open;
			char aCmd[32];
			str_format(aCmd, sizeof(aCmd), "TRADEOPEN %s", aKey);
			pMenu->AddOption(Head.c_str(), aCmd, Open ? "▾" : "▸");
			Any = true;

			if(!Open)
				continue;
			for(int k = 0; k < pTrade->NumListings(); k++)
			{
				const CTradeSystem::SListing *pOther = pTrade->ListingAt(k);
				if(!pOther || pOther->m_Seller != pListing->m_Seller)
					continue;
				// both sides from the merchant's point of view: 收购 ("Buy")
				// is what the merchant takes from the buyer, 给予 ("Give") is
				// what the buyer receives
				const std::string Takes = LocalizeFormat("Menu Trade", "   Buy: {} x{}",
					pOther->m_PriceItem, pOther->m_PriceCount);
				const std::string Gives = LocalizeFormat("Menu Trade", "   #{} Give: {} x{}",
					pOther->m_Id, pOther->m_OfferItem, pOther->m_OfferCount);
				char aBuy[32];
				str_format(aBuy, sizeof(aBuy), "BUY %d", pOther->m_Id);
				pMenu->AddOption(Takes.c_str(), "DISPLAY", "");
				pMenu->AddOption(Gives.c_str(), aBuy, "$");
			}
		}
	}

	// a configured NPC's shelf is part of the same shop; its stock and the next
	// restock ("refresh") time only show inside the expanded submenu
	CNpcSystem *pNpcs = pMenu->GameServer()->Npcs();
	if(pNpcs)
	{
		for(int d = 0; d < pNpcs->NumDefs(); d++)
		{
			const SNpcDef *pDef = pNpcs->DefAt(d);
			// only a nearby NPC's shelf can be traded with
			if(!pDef || !pMenu->GameServer()->NpcInRange(ClientID, d) || pDef->m_lSells.size() == 0)
				continue;

			char aKey[16];
			str_format(aKey, sizeof(aKey), "N%d", d);
			const bool Open = str_comp(pOpen, aKey) == 0;
			const std::string Head = LocalizeFormat("Menu Trade", "{}. {} ({})", ++ProviderIndex, NpcIDDisplayName(pDef->m_Id), pDef->m_lSells.size());
			OpenFound = OpenFound || Open;
			char aCmd[32];
			str_format(aCmd, sizeof(aCmd), "TRADEOPEN %s", aKey);
			pMenu->AddOption(Head.c_str(), aCmd, Open ? "▾" : "▸");
			Any = true;

			if(!Open)
				continue;
			for(int s = 0; s < pDef->m_lSells.size(); s++)
			{
				const SNpcSell &Sell = pDef->m_lSells[s];
				// both sides from the merchant's point of view: 收购 ("Buy")
				// is what the merchant takes from the buyer, 给予 ("Give") is
				// what the buyer receives
				const std::string Takes = LocalizeFormat("Menu Trade", "   Buy: {} x{}",
					Sell.m_PriceItem, Sell.m_PriceCount);
				const std::string Gives = LocalizeFormat("Menu Trade", "   Give: {} x{}",
					Sell.m_Item, Sell.m_Count);
				char aBuy[32];
				str_format(aBuy, sizeof(aBuy), "BUYNPC %d %d", d, s);
				pMenu->AddOption(Takes.c_str(), "DISPLAY", "");
				pMenu->AddOption(Gives.c_str(), aBuy, "$");
				// the shelf's stock and next restock live on their own row so the
				// long item ids cannot push the countdown out of the option text
				const int RestockIn = Sell.m_RestockSeconds > 0 ? maximum(0, Sell.m_RestockAt - time_timestamp()) : 0;
				pMenu->AddOption(LocalizeFormat("Menu Trade", "      stock {}, restock {}s", Sell.m_Stock, RestockIn).c_str(), "DISPLAY", "-");
			}
		}
	}

	// drop an expansion whose provider is gone (sold out / walked away)
	if(OpenRequested && !OpenFound)
		pMenu->m_aTradeOpen[ClientID][0] = '\0';

	if(!Any)
		pMenu->AddOption(Localize("No goods are for sale.", "Menu Trade"), "DISPLAY", "-");

	return true;
}

// step 1: pick the item to sell from the inventory
bool CGameMenu::MenuTradeSell(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	CItemSystem *pItem = pMenu->GameServer()->Item();
	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "DISPLAY") != 0 && str_comp(VoteStatus.m_aCmd, "NONE") != 0)
	{
		CItemID Id;
		if(CItemID::Parse(VoteStatus.m_aCmd, &Id) && !Id.IsZero() && pItem->GetItemCount(ClientID, Id) > 0)
		{
			pMenu->m_aTradeDraft[ClientID] = STradeDraft();
			pMenu->m_aTradeDraft[ClientID].m_OfferItem = Id;
			pMenu->SetPlayerPage(ClientID, "TRADE_SELL_COUNT");
			return false;
		}
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();
	pMenu->AddOption(Localize("Click the item you want to sell.", "Menu Trade"), "DISPLAY", "-");
	pMenu->AddHorizontalRule();

	const CItemSystem::CInventory &Inventory = pItem->GetInventory(ClientID);
	for(int i = 0; i < CItemSystem::CInventory::MAX_ITEMS; i++)
	{
		if(Inventory.IsEmpty(i))
			continue;
		const CItemID &Id = Inventory.m_aItems[i].m_Id;
		char aCmd[UUID_MAXSTRSIZE];
		ItemCommand(Id, aCmd, sizeof(aCmd));
		const std::string Desc = LocalizeFormat("Menu Trade", "{} x{}", Id, Inventory.m_aItems[i].m_Count);
		pMenu->AddOption(Desc.c_str(), aCmd, "-");
	}
	return true;
}

// step 2: type the amount into the vote reason box, then confirm
bool CGameMenu::MenuTradeSellCount(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	CItemSystem *pItem = pMenu->GameServer()->Item();
	STradeDraft &Draft = pMenu->m_aTradeDraft[ClientID];
	if(Draft.m_OfferItem.IsZero() || pItem->GetItemCount(ClientID, Draft.m_OfferItem) <= 0)
	{
		pMenu->SetPlayerPage(ClientID, "TRADE_SELL");
		return false;
	}

	if(str_comp(VoteStatus.m_aCmd, "SELLCOUNT") == 0)
	{
		int Count = VoteStatus.m_aReason[0] ? str_toint(VoteStatus.m_aReason) : 1;
		const int Owned = pItem->GetItemCount(ClientID, Draft.m_OfferItem);
		if(Count < 1)
			Count = 1;
		if(Count > Owned)
			Count = Owned;
		Draft.m_OfferCount = Count;
		pMenu->SetPlayerPage(ClientID, "TRADE_SELL_PRICE");
		return false;
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();
	pMenu->AddOption(Localize("How many do you want to sell?", "Menu Trade"), "DISPLAY", "-");
	{
		const std::string Desc = LocalizeFormat("Menu Trade", "Selling: {} (you have {})",
			Draft.m_OfferItem, pItem->GetItemCount(ClientID, Draft.m_OfferItem));
		pMenu->AddOption(Desc.c_str(), "DISPLAY", "-");
	}
	pMenu->AddOption(Localize("Type the amount in the vote reason box, then click Confirm.", "Menu Trade"), "DISPLAY", "-");
	pMenu->AddOption(Localize("Confirm", "Menu Trade"), "SELLCOUNT", "★");
	return true;
}

// step 3: pick the item the buyer has to pay with
bool CGameMenu::MenuTradeSellPrice(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	CItemSystem *pItem = pMenu->GameServer()->Item();
	STradeDraft &Draft = pMenu->m_aTradeDraft[ClientID];
	if(Draft.m_OfferItem.IsZero() || Draft.m_OfferCount <= 0 || pItem->GetItemCount(ClientID, Draft.m_OfferItem) < Draft.m_OfferCount)
	{
		pMenu->SetPlayerPage(ClientID, "TRADE_SELL");
		return false;
	}

	if(VoteStatus.m_aCmd[0] && str_comp(VoteStatus.m_aCmd, "DISPLAY") != 0 && str_comp(VoteStatus.m_aCmd, "NONE") != 0)
	{
		CItemID Id;
		if(CItemID::Parse(VoteStatus.m_aCmd, &Id) && !Id.IsZero() && pItem->GetItemCount(ClientID, Id) > 0)
		{
			Draft.m_PriceItem = Id;
			pMenu->SetPlayerPage(ClientID, "TRADE_SELL_PRICE_COUNT");
			return false;
		}
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();
	{
		const std::string Desc = LocalizeFormat("Menu Trade", "You are selling {} x{}. Click the item you want as payment.",
			Draft.m_OfferItem, Draft.m_OfferCount);
		pMenu->AddOption(Desc.c_str(), "DISPLAY", "-");
	}
	pMenu->AddHorizontalRule();

	const CItemSystem::CInventory &Inventory = pItem->GetInventory(ClientID);
	for(int i = 0; i < CItemSystem::CInventory::MAX_ITEMS; i++)
	{
		if(Inventory.IsEmpty(i))
			continue;
		const CItemID &Id = Inventory.m_aItems[i].m_Id;
		char aCmd[UUID_MAXSTRSIZE];
		ItemCommand(Id, aCmd, sizeof(aCmd));
		const std::string Desc = LocalizeFormat("Menu Trade", "{} x{}", Id, Inventory.m_aItems[i].m_Count);
		pMenu->AddOption(Desc.c_str(), aCmd, "-");
	}
	return true;
}

// step 4: type the price amount and create the listing
bool CGameMenu::MenuTradeSellPriceCount(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	STradeDraft &Draft = pMenu->m_aTradeDraft[ClientID];
	if(Draft.m_OfferItem.IsZero() || Draft.m_OfferCount <= 0 || Draft.m_PriceItem.IsZero())
	{
		pMenu->SetPlayerPage(ClientID, "TRADE_SELL");
		return false;
	}

	if(str_comp(VoteStatus.m_aCmd, "SELLPRICE") == 0)
	{
		int Count = VoteStatus.m_aReason[0] ? str_toint(VoteStatus.m_aReason) : 1;
		if(Count < 1)
			Count = 1;
		const int ListingId = pMenu->GameServer()->Trade()->AddListing(
			ClientID, Draft.m_OfferItem, Draft.m_OfferCount, Draft.m_PriceItem, Count);
		if(ListingId >= 0)
		{
			pMenu->m_aTradeDraft[ClientID] = STradeDraft();
			pMenu->SetPlayerPage(ClientID, "TRADE_MINE");
		}
		else
			pMenu->SetPlayerPage(ClientID, "TRADE_SELL_PRICE_COUNT");
		return false;
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();
	{
		const std::string Desc = LocalizeFormat("Menu Trade", "Payment: {}. Type the amount in the vote reason box, then click Confirm.",
			Draft.m_PriceItem);
		pMenu->AddOption(Desc.c_str(), "DISPLAY", "-");
	}
	pMenu->AddOption(Localize("Confirm", "Menu Trade"), "SELLPRICE", "★");
	return true;
}

// the player's own listings: click one to take it down again
bool CGameMenu::MenuTradeMine(int ClientID, CCallVoteStatus &VoteStatus, class CGameMenu *pMenu, void *pUserData)
{
	(void) pUserData;
	CPlayer *pPlayer = pMenu->GameServer()->m_apPlayers[ClientID];
	if(!pPlayer)
		return true;

	if(VoteStatus.m_aCmd[0] && str_startswith(VoteStatus.m_aCmd, "UNLIST "))
	{
		const int ListingId = str_toint(VoteStatus.m_aCmd + 7);
		pMenu->GameServer()->Trade()->RemoveListing(ListingId, ClientID, false);
		pMenu->SetPlayerPage(ClientID, "TRADE_MINE");
		return false;
	}

	pMenu->ClearOptions(ClientID);
	pMenu->AddPageTitle();
	pMenu->AddOption(Localize("Click one of your listings to take it down.", "Menu Trade"), "DISPLAY", "-");
	pMenu->AddHorizontalRule();

	CTradeSystem *pTrade = pMenu->GameServer()->Trade();
	int Shown = 0;
	for(int i = 0; pTrade && i < pTrade->NumListings(); i++)
	{
		const CTradeSystem::SListing *pListing = pTrade->ListingAt(i);
		if(!pListing || pListing->m_Seller != ClientID)
			continue;
		const std::string Desc = LocalizeFormat("Menu Trade", "#{}: {} x{} for {} x{}",
			pListing->m_Id, pListing->m_OfferItem, pListing->m_OfferCount, pListing->m_PriceItem, pListing->m_PriceCount);
		char aCmd[24];
		str_format(aCmd, sizeof(aCmd), "UNLIST %d", pListing->m_Id);
		pMenu->AddOption(Desc.c_str(), aCmd, "-");
		Shown++;
	}
	if(Shown == 0)
		pMenu->AddOption(Localize("You have no listings.", "Menu Trade"), "DISPLAY", "-");
	return true;
}
