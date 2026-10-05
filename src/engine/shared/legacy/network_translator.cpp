/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include "network_translator.h"

#include <base/system.h>

#include <engine/message.h>
#include <engine/shared/compression.h>
#include <engine/shared/packer.h>
#include <engine/shared/protocol.h>

#include <generated/protocol.h>
#include <generated/protocol7.h>

#include "network7.h"

namespace legacy
{
	// 0.7 int-packed string encoding, copied from StrToInts in gamecore.h
	// (game-shared is not linkable from here). Reads at most Num * 4 bytes and
	// clears the final byte so the decoded string is NUL terminated, matching a
	// real 0.7 server byte for byte.
	static void StrToInts(const char *pStr, int *pInts, int Num)
	{
		int Index = 0;
		while(Num)
		{
			char aBuf[4] = {0, 0, 0, 0};
			for(int c = 0; c < 4 && pStr[Index]; c++, Index++)
				aBuf[c] = pStr[Index];
			*pInts = ((aBuf[0] + 128) << 24) | ((aBuf[1] + 128) << 16) | ((aBuf[2] + 128) << 8) | (aBuf[3] + 128);
			pInts++;
			Num--;
		}

		// null terminate
		pInts[-1] &= 0xffffff00;
	}

	// Copies a fixed-width 0.8 NetChar field, which carries no terminator and may
	// fill its whole width, into NUL-terminated storage. str_copy_fixed cannot be
	// used here because its str_length would read past the end of such a field.
	static void CopyFixedWidth(char *pDst, int DstSize, const char *pSrc, int Width)
	{
		int Len = 0;
		while(Len < Width && pSrc[Len])
			Len++;
		if(Len > DstSize - 1)
			Len = DstSize - 1;
		mem_copy(pDst, pSrc, Len);
		pDst[Len] = 0;
	}

	static int FinishMsg(CPacker *pPacker, void *pOut, int OutSize)
	{
		if(pPacker->Error())
			return -2;
		if(pPacker->Size() > OutSize)
			return -2;
		mem_copy(pOut, pPacker->Data(), pPacker->Size());
		return pPacker->Size();
	}

	static int CopyRawMsg(int MsgId8, CUnpacker *pUnpacker, void *pOut, int OutSize)
	{
		const int Remaining = pUnpacker->RemainingSize();
		const unsigned char *pRaw = pUnpacker->GetRaw(Remaining);
		if(!pRaw)
			return -2;
		CMsgPacker Packer(MsgId8);
		Packer.AddRaw(pRaw, Remaining);
		return FinishMsg(&Packer, pOut, OutSize);
	}

	// 0.8 and 0.7 Votes enums differ: 0.8 dropped UNKNOWN and appended RUN_*.
	static int MapVoteType7To8(int Type7)
	{
		switch(Type7)
		{
			case protocol7::VOTE_START_OP: return VOTE_START_OP;
			case protocol7::VOTE_START_KICK: return VOTE_START_KICK;
			case protocol7::VOTE_START_SPEC: return VOTE_START_SPEC;
			case protocol7::VOTE_END_ABORT: return VOTE_END_ABORT;
			case protocol7::VOTE_END_PASS: return VOTE_END_PASS;
			case protocol7::VOTE_END_FAIL: return VOTE_END_FAIL;
		}
		return -1;
	}

	// 0.8 object type -> 0.7 object type. -1 means the type has no 0.7
	// counterpart; TeeInfo, Tuning and m_PredictionFlags are synthesized instead.
	static int MapObjectType8To7(int Type8)
	{
		switch(Type8)
		{
			case NETOBJTYPE_PLAYERINPUT: return protocol7::NETOBJTYPE_PLAYERINPUT;
			case NETOBJTYPE_PROJECTILE: return protocol7::NETOBJTYPE_PROJECTILE;
			case NETOBJTYPE_LASER: return protocol7::NETOBJTYPE_LASER;
			case NETOBJTYPE_PICKUP: return protocol7::NETOBJTYPE_PICKUP;
			case NETOBJTYPE_FLAG: return protocol7::NETOBJTYPE_FLAG;
			case NETOBJTYPE_GAMEDATA: return protocol7::NETOBJTYPE_GAMEDATA;
			case NETOBJTYPE_GAMEDATATEAM: return protocol7::NETOBJTYPE_GAMEDATATEAM;
			case NETOBJTYPE_GAMEDATAFLAG: return protocol7::NETOBJTYPE_GAMEDATAFLAG;
			case NETOBJTYPE_CHARACTERCORE: return protocol7::NETOBJTYPE_CHARACTERCORE;
			case NETOBJTYPE_CHARACTER: return protocol7::NETOBJTYPE_CHARACTER;
			case NETOBJTYPE_SPECTATORINFO: return protocol7::NETOBJTYPE_SPECTATORINFO;
			case NETOBJTYPE_GAMEDATARACE: return protocol7::NETOBJTYPE_GAMEDATARACE;
			// events
			case NETEVENTTYPE_COMMON: return protocol7::NETEVENTTYPE_COMMON;
			case NETEVENTTYPE_EXPLOSION: return protocol7::NETEVENTTYPE_EXPLOSION;
			case NETEVENTTYPE_SPAWN: return protocol7::NETEVENTTYPE_SPAWN;
			case NETEVENTTYPE_HAMMERHIT: return protocol7::NETEVENTTYPE_HAMMERHIT;
			case NETEVENTTYPE_DEATH: return protocol7::NETEVENTTYPE_DEATH;
			case NETEVENTTYPE_SOUNDWORLD: return protocol7::NETEVENTTYPE_SOUNDWORLD;
			// NETEVENTTYPE_SOUNDGLOBAL is 0.8-only and has no 0.7 counterpart, so
			// it is dropped. 0.8 inserts it before NETEVENTTYPE_DAMAGE, shifting
			// every later enumerator, so it must never be copied positionally.
			case NETEVENTTYPE_DAMAGE: return protocol7::NETEVENTTYPE_DAMAGE;
		}
		return -1;
	}

	// The inverse of MapVoteType7To8. 0.8's RUN_OP/RUN_KICK/RUN_SPEC are its
	// replacements for 0.7's VOTE_START_OP/KICK/SPEC, so they map back onto those.
	static int MapVoteType8To7(int Type8)
	{
		switch(Type8)
		{
			case VOTE_START_OP: return protocol7::VOTE_START_OP;
			case VOTE_START_KICK: return protocol7::VOTE_START_KICK;
			case VOTE_START_SPEC: return protocol7::VOTE_START_SPEC;
			case VOTE_END_ABORT: return protocol7::VOTE_END_ABORT;
			case VOTE_END_PASS: return protocol7::VOTE_END_PASS;
			case VOTE_END_FAIL: return protocol7::VOTE_END_FAIL;
			case VOTE_RUN_OP: return protocol7::VOTE_START_OP;
			case VOTE_RUN_KICK: return protocol7::VOTE_START_KICK;
			case VOTE_RUN_SPEC: return protocol7::VOTE_START_SPEC;
		}
		return -1;
	}

	void CNetworkTranslator::CClientState7::Reset()
	{
		m_Active = false;
		m_Local = 0;
		m_Team = 0;
		m_Country = 0;
		mem_zero(m_aName, sizeof(m_aName));
		mem_zero(m_aClan, sizeof(m_aClan));
		mem_zero(m_aaSkinPartNames, sizeof(m_aaSkinPartNames));
		mem_zero(m_aUseCustomColors, sizeof(m_aUseCustomColors));
		mem_zero(m_aSkinPartColors, sizeof(m_aSkinPartColors));
	}

	CNetworkTranslator::CNetworkTranslator()
	{
		// The static item sizes must match what each protocol's own server uses,
		// otherwise the delta size fields desync. The 0.8 delta is needed to
		// decode the deltas our own server emits before rebuilding them as 0.7.
		Net7ConfigureSnapshotDelta(&m_Delta7);
		CNetObjHandler Handler8;
		for(int i = 0; i < NUM_NETOBJTYPES; i++)
			m_Delta8.SetStaticsize(i, Handler8.GetObjSize(i));

		// the storage pointers must be valid before Reset() purges them
		m_Snapshots8.Init();
		m_pNetVersion = LEGACY_NET7_NETVERSION;
		Reset();
	}

	CNetworkTranslator::~CNetworkTranslator()
	{
	}

	void CNetworkTranslator::Reset()
	{
		for(int i = 0; i < MAX_CLIENTS7; i++)
			m_aClients7[i].Reset();
		m_TuningValid = false;
		mem_zero(m_aTuneParams7, sizeof(m_aTuneParams7));

		// outbound state
		mem_zero(m_aTees8, sizeof(m_aTees8));
		mem_zero(m_aTeeActive8, sizeof(m_aTeeActive8));
		m_TuningValid8 = false;
		mem_zero(m_aTuneParams8, sizeof(m_aTuneParams8));
		m_Snapshots8.PurgeAll();
		m_Snapshots8.Init();
		m_EmptySnap8.Clear();
		m_CurrentSentTick8 = 0;
		ResetBase7();
	}

	void CNetworkTranslator::ResetBase7()
	{
		m_Base7Data.set_size(0);
		m_pBase7 = &m_EmptySnap8;
		m_BaseTick7 = -1;
	}

	// Clears the outbound part-reassembly state. Only 0.8 snapshots are
	// reassembled, because a 0.7 client cannot send one.
	void CNetworkTranslator::ResetSnapshotParts()
	{
		mem_zero(m_aParts8, sizeof(m_aParts8));
		m_NumParts8 = 0;
		m_LastPartSize8 = 0;
	}

	void CNetworkTranslator::SetBase7(int GameTick, int Snap7Size)
	{
		m_Base7Data.set_size(Snap7Size);
		mem_copy(m_Base7Data.base_ptr(), m_Snap7Out.base_ptr(), Snap7Size);
		m_pBase7 = (CSnapshot *) m_Base7Data.base_ptr();
		m_BaseTick7 = GameTick;
	}

	const CNetworkTranslator::CTeeState8 *CNetworkTranslator::TeeState8(int TeeID) const
	{
		if(TeeID < 0 || TeeID >= MAX_TEES8)
			return 0;
		return &m_aTees8[TeeID];
	}

	const CNetworkTranslator::CClientState7 *CNetworkTranslator::ClientState(int ClientID) const
	{
		if(ClientID < 0 || ClientID >= MAX_CLIENTS7)
			return 0;
		return &m_aClients7[ClientID];
	}

	void CNetworkTranslator::AbsorbClientInfo(int ClientID, int Local, int Team, int Country,
		const char *pName, const char *pClan, const char *const *papSkinPartNames,
		const int *pUseCustomColors, const int *pSkinPartColors)
	{
		if(ClientID < 0 || ClientID >= MAX_CLIENTS7)
			return;
		CClientState7 *pState = &m_aClients7[ClientID];
		pState->m_Active = true;
		pState->m_Local = Local;
		pState->m_Team = Team;
		pState->m_Country = Country == -1 ? 0xFFFF : Country;
		str_copy_fixed(pState->m_aName, pName, sizeof(pState->m_aName));
		str_copy_fixed(pState->m_aClan, pClan, sizeof(pState->m_aClan));
		for(int p = 0; p < NUM_SKINPARTS; p++)
		{
			str_copy_fixed(pState->m_aaSkinPartNames[p], papSkinPartNames[p], sizeof(pState->m_aaSkinPartNames[p]));
			pState->m_aUseCustomColors[p] = pUseCustomColors[p];
			pState->m_aSkinPartColors[p] = pSkinPartColors[p];
		}
	}

	// Captures the identity carried by one 0.8 TeeInfo so a later 0.7 snapshot can
	// re-emit it as PlayerInfo + PlayerInfoRace + De_ClientInfo. Called for every
	// TeeInfo item seen, including the one embedded in a full snapshot.
	void CNetworkTranslator::AbsorbTeeInfo8(int ID, const CSnapshotItem *pItem, int ItemSize)
	{
		if(ID < 0 || ID >= MAX_TEES8)
			return;
		if(ItemSize < (int) sizeof(CNetObj_TeeInfo))
			return;

		const CNetObj_TeeInfo *pIn = (const CNetObj_TeeInfo *) pItem->Data();
		CTeeState8 *pState = &m_aTees8[ID];

		pState->m_Active = true;
		pState->m_Flag = pIn->m_Flag;
		pState->m_Score = pIn->m_Score;
		pState->m_Team = pIn->m_Team;
		pState->m_Latency = (pIn->m_LatencyAndCountry >> 16) & 0xffff;
		pState->m_Country = pIn->m_LatencyAndCountry & 0xffff;
		pState->m_RaceStartTick = pIn->m_RaceStartTick;
		// The 0.8 fields are raw content without a terminator and may fill their
		// whole width, so they are copied by width rather than by length.
		CopyFixedWidth(pState->m_aName, sizeof(pState->m_aName), pIn->m_aName, sizeof(pIn->m_aName));
		CopyFixedWidth(pState->m_aClan, sizeof(pState->m_aClan), pIn->m_aClan, sizeof(pIn->m_aClan));
		for(int p = 0; p < NUM_SKINPARTS; p++)
		{
			CopyFixedWidth(pState->m_aaSkinPartNames[p], sizeof(pState->m_aaSkinPartNames[p]), pIn->m_aaSkinPartNames[p], sizeof(pIn->m_aaSkinPartNames[p]));
			pState->m_aUseCustomColors[p] = pIn->m_aUseCustomColors[p];
			pState->m_aSkinPartColors[p] = pIn->m_aSkinPartColors[p];
		}
		m_aTeeActive8[ID] = true;
	}

	// Rebuilds a 0.7 snapshot from one of our 0.8 snapshots: TeeInfo splits back
	// into the three 0.7 identity objects, Tuning becomes De_TuneParams, and
	// GameData::m_PredictionFlags becomes a GameDataPrediction object.
	int CNetworkTranslator::BuildSnapshot7(const CSnapshot *pSnap8)
	{
		const int NumItems = pSnap8->NumItems();

		// Pass one: learn every identity so the split objects are consistent even
		// if a TeeInfo appears after the item that references it.
		for(int i = 0; i < NumItems; i++)
		{
			const CSnapshotItem *pItem = pSnap8->GetItem(i);
			if(pItem->Type() == NETOBJTYPE_TEEINFO)
				AbsorbTeeInfo8(pItem->ID(), pItem, pSnap8->GetItemSize(i));
			else if(pItem->Type() == NETOBJTYPE_TUNING && pSnap8->GetItemSize(i) >= (int) sizeof(CNetObj_Tuning))
			{
				mem_copy(m_aTuneParams8, pItem->Data(), sizeof(m_aTuneParams8));
				m_TuningValid8 = true;
			}
		}

		CSnapshotBuilder Builder;
		Builder.Init();

		// The 0.7 client needs a PlayerInfo for every tee it can see, and the
		// identity objects alongside it. Emit them from the cached state so a
		// client that missed the Sv_ClientEnter still gets a complete tee list.
		for(int TeeID = 0; TeeID < MAX_TEES8; TeeID++)
		{
			if(!m_aTeeActive8[TeeID])
				continue;
			const CTeeState8 *pState = &m_aTees8[TeeID];
			if(!pState->m_Active)
				continue;
			// The 0.7 identity objects are keyed by client id, so a tee outside
			// the 0.7 client range cannot be represented and is skipped.
			if(TeeID >= MAX_CLIENTS7)
				continue;

			protocol7::CNetObj_PlayerInfo *pPlayerInfo = (protocol7::CNetObj_PlayerInfo *) Builder.NewItem(protocol7::NETOBJTYPE_PLAYERINFO, TeeID, sizeof(protocol7::CNetObj_PlayerInfo));
			if(!pPlayerInfo)
				return -1;
			// ADMIN..BOT occupy the same bits in both protocols
			pPlayerInfo->m_PlayerFlags = pState->m_Flag & 0x7f;
			pPlayerInfo->m_Score = pState->m_Score;
			pPlayerInfo->m_Latency = pState->m_Latency;

			protocol7::CNetObj_PlayerInfoRace *pRace = (protocol7::CNetObj_PlayerInfoRace *) Builder.NewItem(protocol7::NETOBJTYPE_PLAYERINFORACE, TeeID, sizeof(protocol7::CNetObj_PlayerInfoRace));
			if(!pRace)
				return -1;
			pRace->m_RaceStartTick = pState->m_RaceStartTick;

			protocol7::CNetObj_De_ClientInfo *pClientInfo = (protocol7::CNetObj_De_ClientInfo *) Builder.NewItem(protocol7::NETOBJTYPE_DE_CLIENTINFO, TeeID, sizeof(protocol7::CNetObj_De_ClientInfo));
			if(!pClientInfo)
				return -1;
			// 0.8 carries "local" as a TeeInfo flag; 0.7 puts it on this object
			pClientInfo->m_Local = (pState->m_Flag & TEEFLAG_LOCAL) ? 1 : 0;
			pClientInfo->m_Team = pState->m_Team;
			StrToInts(pState->m_aName, pClientInfo->m_aName, 4);
			StrToInts(pState->m_aClan, pClientInfo->m_aClan, 3);
			pClientInfo->m_Country = pState->m_Country;
			for(int p = 0; p < NUM_SKINPARTS; p++)
			{
				StrToInts(pState->m_aaSkinPartNames[p], pClientInfo->m_aaSkinPartNames[p], 6);
				pClientInfo->m_aUseCustomColors[p] = pState->m_aUseCustomColors[p];
				pClientInfo->m_aSkinPartColors[p] = pState->m_aSkinPartColors[p];
			}

			// 0.7 keeps the "hidden in the board" bit on a separate object
			if(pState->m_Flag & TEEFLAG_HIDDEN_IN_BOARD)
			{
				protocol7::CNetObj_PlayerInfoExtra *pExtra = (protocol7::CNetObj_PlayerInfoExtra *) Builder.NewItem(protocol7::NETOBJTYPE_PLAYERINFOEXTRA, TeeID, sizeof(protocol7::CNetObj_PlayerInfoExtra));
				if(!pExtra)
					return -1;
				pExtra->m_RealClientID = TeeID;
				pExtra->m_PlayerFlagsExtra = 1; // PLAYERFLAGEXTRA_HIDDEN_IN_BOARD
			}
		}

		if(m_TuningValid8)
		{
			protocol7::CNetObj_De_TuneParams *pTune = (protocol7::CNetObj_De_TuneParams *) Builder.NewItem(protocol7::NETOBJTYPE_DE_TUNEPARAMS, 0, sizeof(protocol7::CNetObj_De_TuneParams));
			if(!pTune)
				return -1;
			mem_copy(pTune->m_aTuneParams, m_aTuneParams8, sizeof(pTune->m_aTuneParams));
		}

		for(int i = 0; i < NumItems; i++)
		{
			const CSnapshotItem *pItem = pSnap8->GetItem(i);
			const int Type = pItem->Type();
			const int ID = pItem->ID();
			const int Size = pSnap8->GetItemSize(i);

			// absorbed into the objects emitted above
			if(Type == NETOBJTYPE_TEEINFO || Type == NETOBJTYPE_TUNING)
				continue;

			if(Type == NETOBJTYPE_GAMEDATA)
			{
				if(Size < (int) sizeof(CNetObj_GameData))
					continue;
				const CNetObj_GameData *pIn = (const CNetObj_GameData *) pItem->Data();

				// 0.7's GameData has no m_PredictionFlags; it lives on its own object
				CNetObj_GameData *pOut = (CNetObj_GameData *) Builder.NewItem(protocol7::NETOBJTYPE_GAMEDATA, ID, sizeof(protocol7::CNetObj_GameData));
				if(!pOut)
					return -1;
				pOut->m_GameStartTick = pIn->m_GameStartTick;
				pOut->m_GameStateFlags = pIn->m_GameStateFlags;
				pOut->m_GameStateEndTick = pIn->m_GameStateEndTick;

				protocol7::CNetObj_GameDataPrediction *pPrediction = (protocol7::CNetObj_GameDataPrediction *) Builder.NewItem(protocol7::NETOBJTYPE_GAMEDATAPREDICTION, 0, sizeof(protocol7::CNetObj_GameDataPrediction));
				if(!pPrediction)
					return -1;
				pPrediction->m_PredictionFlags = pIn->m_PredictionFlags;
				continue;
			}

			if(Type == NETOBJTYPE_CHARACTER)
			{
				if(Size < (int) sizeof(CNetObj_Character))
					continue;
				const CNetObj_Character *pIn = (const CNetObj_Character *) pItem->Data();
				protocol7::CNetObj_Character *pOut = (protocol7::CNetObj_Character *) Builder.NewItem(protocol7::NETOBJTYPE_CHARACTER, ID, sizeof(protocol7::CNetObj_Character));
				if(!pOut)
					return -1;
				// 0.8 inserted m_MaxHealth/m_MaxArmor *between* m_Armor and
				// m_AmmoCount, so a prefix copy would shift every later field by
				// two and the 0.7 client would read ammo as health, weapon as
				// armor and so on. Copy the shared core, then the tail field by
				// field.
				mem_copy((CNetObj_CharacterCore *) pOut, (const CNetObj_CharacterCore *) pIn, sizeof(protocol7::CNetObj_CharacterCore));
				pOut->m_Health = pIn->m_Health;
				pOut->m_Armor = pIn->m_Armor;
				pOut->m_AmmoCount = pIn->m_AmmoCount;
				pOut->m_Weapon = pIn->m_Weapon;
				pOut->m_Emote = pIn->m_Emote;
				pOut->m_AttackTick = pIn->m_AttackTick;
				pOut->m_TriggeredEvents = pIn->m_TriggeredEvents;
				continue;
			}

			const int Type7 = MapObjectType8To7(Type);
			if(Type7 < 0)
				continue;

			// GAMEDATARACE has a different type number in each protocol, so the
			// item is created under the 0.7 type rather than reusing its header.
			if(Type == NETOBJTYPE_GAMEDATARACE)
			{
				if(Size < (int) sizeof(CNetObj_GameDataRace))
					continue;
				CNetObj_GameDataRace *pOut = (CNetObj_GameDataRace *) Builder.NewItem(Type7, ID, sizeof(protocol7::CNetObj_GameDataRace));
				if(!pOut)
					return -1;
				mem_copy(pOut, pItem->Data(), sizeof(protocol7::CNetObj_GameDataRace));
				continue;
			}

			void *pOut = Builder.NewItem(Type7, ID, Size);
			if(!pOut)
				return -1;
			mem_copy(pOut, pItem->Data(), Size);
		}

		m_Snap7Out.set_size(Builder.RequiredSize());
		return Builder.Finish(m_Snap7Out.base_ptr());
	}

	void CNetworkTranslator::StoreSnapshot8Copy(int GameTick, int DeltaTick)
	{
		// An empty 0.8 delta means the snapshot at GameTick equals the one at
		// DeltaTick. Mirror that tick into the 0.8 history so a later delta can be
		// rebased on it, and bail out if the source tick is unknown rather than
		// inventing a baseline.
		if(DeltaTick >= 0)
		{
			CSnapshot *pStored = 0;
			const int StoredSize = m_Snapshots8.Get(DeltaTick, 0, &pStored, 0);
			if(StoredSize < 0 || !pStored)
				return;
			m_Snapshots8.Add(GameTick, time_get(), StoredSize, pStored, 0);
		}
		else
		{
			m_EmptySnap8.Clear();
			m_Snapshots8.Add(GameTick, time_get(), sizeof(CSnapshot), &m_EmptySnap8, 0);
		}
		m_Snapshots8.PurgeUntil(GameTick - SERVER_TICK_SPEED * 3);
	}

	int CNetworkTranslator::TranslateSnapshotDelta8To7(int GameTick, int DeltaTick, const void *pDelta8, int DeltaSize8, void *pOut)
	{
		const CSnapshot *pBase8 = &m_EmptySnap8;
		int BaseSize8 = (int) sizeof(CSnapshot);
		if(DeltaTick >= 0)
		{
			CSnapshot *pStored = 0;
			BaseSize8 = m_Snapshots8.Get(DeltaTick, 0, &pStored, 0);
			if(BaseSize8 < 0 || !pStored)
				return -1; // unknown baseline, the next full snapshot resyncs us
			pBase8 = pStored;
		}

		const int Bound = BaseSize8 + DeltaSize8 * 2 + 256;
		m_Snap8CopyData.set_size(Bound);
		CSnapshot *pTo8 = (CSnapshot *) m_Snap8CopyData.base_ptr();
		const int Snap8Size = m_Delta8.UnpackDelta(pBase8, pTo8, pDelta8, DeltaSize8);
		if(Snap8Size < 0)
			return Snap8Size;

		m_Snapshots8.Add(GameTick, time_get(), Snap8Size, pTo8, 0);
		m_Snapshots8.PurgeUntil(GameTick - SERVER_TICK_SPEED * 3);

		const int Snap7Size = BuildSnapshot7(pTo8);
		if(Snap7Size < 0)
			return -1;

		const int DeltaSize7 = m_Delta7.CreateDelta(m_pBase7, (CSnapshot *) m_Snap7Out.base_ptr(), pOut);
		if(DeltaSize7 < 0)
			return DeltaSize7;

		SetBase7(GameTick, Snap7Size);
		return DeltaSize7;
	}

	int CNetworkTranslator::EmitSnapshot7(int GameTick, int DeltaField, int DeltaSize7, const void *pDelta7, CNetChunk *pOutChunks, int MaxOutChunks)
	{
		if(MaxOutChunks < 1)
			return 0;

		if(DeltaSize7 <= 0)
		{
			CMsgPacker Packer(NETMSG_SNAPEMPTY, true);
			Packer.AddInt(GameTick);
			Packer.AddInt(DeltaField);
			if(Packer.Error())
				return 0;
			m_aClientOut[0].set_size(Packer.Size());
			mem_copy(m_aClientOut[0].base_ptr(), Packer.Data(), Packer.Size());
			pOutChunks[0].m_ClientID = 0;
			pOutChunks[0].m_Flags = NETSENDFLAG_FLUSH;
			pOutChunks[0].m_DataSize = Packer.Size();
			pOutChunks[0].m_pData = m_aClientOut[0].base_ptr();
			m_BaseTick7 = GameTick;
			return 1;
		}

		array<unsigned char> aCompressed;
		aCompressed.set_size(DeltaSize7 + DeltaSize7 / 2 + 4096);
		const int CompSize = (int) CVariableInt::Compress(pDelta7, DeltaSize7, aCompressed.base_ptr(), aCompressed.size());
		if(CompSize < 0)
			return 0;

		const int Crc = m_pBase7->Crc();
		const int MaxSize = MAX_SNAPSHOT_PACKSIZE;
		const int NumPackets = (CompSize + MaxSize - 1) / MaxSize;
		if(NumPackets < 1 || NumPackets > MaxOutChunks)
			return 0;

		int n = 0;
		for(int Left = CompSize; Left > 0; n++)
		{
			const int Chunk = Left < MaxSize ? Left : MaxSize;
			Left -= Chunk;

			CMsgPacker Packer(NumPackets == 1 ? NETMSG_SNAPSINGLE : NETMSG_SNAP, true);
			Packer.AddInt(GameTick);
			Packer.AddInt(DeltaField);
			if(NumPackets > 1)
			{
				Packer.AddInt(NumPackets);
				Packer.AddInt(n);
			}
			Packer.AddInt(Crc);
			Packer.AddInt(Chunk);
			Packer.AddRaw(aCompressed.base_ptr() + n * MaxSize, Chunk);
			if(Packer.Error())
				return 0;

			m_aClientOut[n].set_size(Packer.Size());
			mem_copy(m_aClientOut[n].base_ptr(), Packer.Data(), Packer.Size());
			pOutChunks[n].m_ClientID = 0;
			pOutChunks[n].m_Flags = NETSENDFLAG_FLUSH;
			pOutChunks[n].m_DataSize = Packer.Size();
			pOutChunks[n].m_pData = m_aClientOut[n].base_ptr();
		}
		return n;
	}

	int CNetworkTranslator::TranslateCompleteSnapshot8(int GameTick, int DeltaTick, int CompleteSize, CNetChunk *pOutChunks, int MaxOutChunks)
	{
		ResetSnapshotParts();
		m_CurrentSentTick8 = GameTick;

		if(CompleteSize <= 0)
		{
			StoreSnapshot8Copy(GameTick, DeltaTick);
			return EmitSnapshot7(GameTick, GameTick - m_BaseTick7, 0, 0, pOutChunks, MaxOutChunks);
		}

		array<unsigned char> aDecompressed;
		aDecompressed.set_size(CompleteSize * 4 + 1024);
		const int IntSize = (int) CVariableInt::Decompress(m_Incoming8Data.base_ptr(), CompleteSize, aDecompressed.base_ptr(), aDecompressed.size());
		if(IntSize < 0)
			return 0;

		array<unsigned char> aDelta7;
		aDelta7.set_size(IntSize * 4 + 1024 * 1024);
		// A full 0.8 snapshot means our server believes the client has no usable
		// baseline, so the 0.7 delta has to be made self-contained as well.
		if(DeltaTick < 0)
			ResetBase7();
		const int BaseTickBefore = m_BaseTick7;
		const int DeltaSize7 = TranslateSnapshotDelta8To7(GameTick, DeltaTick, aDecompressed.base_ptr(), IntSize, aDelta7.base_ptr());
		if(DeltaSize7 < 0)
			return 0;

		return EmitSnapshot7(GameTick, GameTick - BaseTickBefore, DeltaSize7, aDelta7.base_ptr(), pOutChunks, MaxOutChunks);
	}

	int CNetworkTranslator::HandleSnapshot8(CUnpacker *pUnpacker, int MsgId, CNetChunk *pOutChunks, int MaxOutChunks)
	{
		const int GameTick = pUnpacker->GetInt();
		const int DeltaTick = GameTick - pUnpacker->GetInt();
		if(pUnpacker->Error())
			return 0;

		if(GameTick < m_CurrentSentTick8)
			return 0;

		if(MsgId == NETMSG_SNAPEMPTY)
			return TranslateCompleteSnapshot8(GameTick, DeltaTick, 0, pOutChunks, MaxOutChunks);

		int NumParts = 1;
		int Part = 0;
		if(MsgId == NETMSG_SNAP)
		{
			NumParts = pUnpacker->GetInt();
			Part = pUnpacker->GetInt();
			if(NumParts < 1 || NumParts > CSnapshot::MAX_PARTS || Part < 0 || Part >= NumParts)
				return 0;
		}

		const int Crc = pUnpacker->GetInt();
		(void) Crc; // the 0.7 CRC is recomputed from the rebuilt snapshot
		const int PartSize = pUnpacker->GetInt();
		if(PartSize < 0 || PartSize > MAX_SNAPSHOT_PACKSIZE)
			return 0;
		const char *pData = (const char *) pUnpacker->GetRaw(PartSize);
		if(pUnpacker->Error())
			return 0;

		if(MsgId == NETMSG_SNAPSINGLE)
		{
			if(m_Incoming8Data.size() < MAX_SNAPSHOT_PACKSIZE)
				m_Incoming8Data.set_size(MAX_SNAPSHOT_PACKSIZE);
			if(pData)
				mem_copy(m_Incoming8Data.base_ptr(), pData, PartSize);
			return TranslateCompleteSnapshot8(GameTick, DeltaTick, PartSize, pOutChunks, MaxOutChunks);
		}

		if(GameTick != m_CurrentSentTick8)
		{
			ResetSnapshotParts();
			m_CurrentSentTick8 = GameTick;
		}

		const int BufSize = CSnapshot::MAX_PARTS * MAX_SNAPSHOT_PACKSIZE;
		if(m_Incoming8Data.size() < BufSize)
			m_Incoming8Data.set_size(BufSize);
		if(pData)
			mem_copy(m_Incoming8Data.base_ptr() + Part * MAX_SNAPSHOT_PACKSIZE, pData, PartSize);

		const int PartByte = Part / 8;
		const unsigned char PartMask = (unsigned char) (1 << (Part % 8));
		if(!(m_aParts8[PartByte] & PartMask))
		{
			m_aParts8[PartByte] |= PartMask;
			m_NumParts8++;
		}
		if(Part == NumParts - 1)
			m_LastPartSize8 = PartSize;

		if(m_NumParts8 != NumParts)
			return 0;

		const int CompleteSize = (NumParts - 1) * MAX_SNAPSHOT_PACKSIZE + m_LastPartSize8;
		return TranslateCompleteSnapshot8(GameTick, DeltaTick, CompleteSize, pOutChunks, MaxOutChunks);
	}

	int CNetworkTranslator::TranslateServerMsg(const void *pMsg7, int Size7, void *pOut, int OutSize)
	{
		CMsgUnpacker Unpacker(pMsg7, Size7);
		if(Unpacker.Error())
			return -2;
		const int MsgId7 = Unpacker.Type();

		// Sv_TuneParams has no declared fields; the 32 raw CTuningParams ints
		// follow the empty message.
		if(MsgId7 == protocol7::NETMSGTYPE_SV_TUNEPARAMS)
		{
			for(int i = 0; i < NUM_TUNES; i++)
				m_aTuneParams7[i] = Unpacker.GetInt();
			if(Unpacker.Error())
				return -2;
			m_TuningValid = true;
			return -1; // consumed, synthesized into the Tuning snapshot object
		}

		// Messages whose payload is not described by the table: copy it verbatim.
		if(MsgId7 == protocol7::NETMSGTYPE_SV_VOTEOPTIONLISTADD)
			return CopyRawMsg(NETMSGTYPE_SV_VOTEOPTIONLISTADD, &Unpacker, pOut, OutSize);
		if(MsgId7 == protocol7::NETMSGTYPE_SV_GAMEMSG)
			return CopyRawMsg(NETMSGTYPE_SV_GAMEMSG, &Unpacker, pOut, OutSize);

		static protocol7::CNetObjHandler Handler7;
		void *pRaw = Handler7.SecureUnpackMsg(MsgId7, &Unpacker);
		if(!pRaw)
			return -2;

		switch(MsgId7)
		{
			case protocol7::NETMSGTYPE_SV_MOTD:
			{
				protocol7::CNetMsg_Sv_Motd *pIn = (protocol7::CNetMsg_Sv_Motd *) pRaw;
				CNetMsg_Sv_Motd Out;
				Out.m_pMessage = pIn->m_pMessage;
				CMsgPacker Packer(NETMSGTYPE_SV_MOTD);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_BROADCAST:
			{
				protocol7::CNetMsg_Sv_Broadcast *pIn = (protocol7::CNetMsg_Sv_Broadcast *) pRaw;
				CNetMsg_Sv_Broadcast Out;
				Out.m_pMessage = pIn->m_pMessage;
				CMsgPacker Packer(NETMSGTYPE_SV_BROADCAST);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_CHAT:
			{
				protocol7::CNetMsg_Sv_Chat *pIn = (protocol7::CNetMsg_Sv_Chat *) pRaw;
				CNetMsg_Sv_Chat Out;
				Out.m_Mode = pIn->m_Mode;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_TargetID = pIn->m_TargetID;
				Out.m_pMessage = pIn->m_pMessage;
				CMsgPacker Packer(NETMSGTYPE_SV_CHAT);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_TEAM:
			{
				protocol7::CNetMsg_Sv_Team *pIn = (protocol7::CNetMsg_Sv_Team *) pRaw;
				// 0.8 derives team membership from the TeeInfo snapshot object,
				// so team changes must update the identity table too
				if(pIn->m_ClientID >= 0 && pIn->m_ClientID < MAX_CLIENTS7)
					m_aClients7[pIn->m_ClientID].m_Team = pIn->m_Team;
				CNetMsg_Sv_Team Out;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_Team = pIn->m_Team;
				Out.m_Silent = pIn->m_Silent;
				Out.m_CooldownTick = pIn->m_CooldownTick;
				CMsgPacker Packer(NETMSGTYPE_SV_TEAM);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_KILLMSG:
			{
				protocol7::CNetMsg_Sv_KillMsg *pIn = (protocol7::CNetMsg_Sv_KillMsg *) pRaw;
				CNetMsg_Sv_KillMsg Out;
				Out.m_Killer = pIn->m_Killer;
				Out.m_Victim = pIn->m_Victim;
				Out.m_Weapon = pIn->m_Weapon;
				Out.m_ModeSpecial = pIn->m_ModeSpecial;
				Out.m_Assist = pIn->m_Assist;
				CMsgPacker Packer(NETMSGTYPE_SV_KILLMSG);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_EXTRAPROJECTILE:
				return -1; // removed in 0.8
			case protocol7::NETMSGTYPE_SV_READYTOENTER:
			{
				CNetMsg_Sv_ReadyToEnter Out;
				CMsgPacker Packer(NETMSGTYPE_SV_READYTOENTER);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_WEAPONPICKUP:
			{
				protocol7::CNetMsg_Sv_WeaponPickup *pIn = (protocol7::CNetMsg_Sv_WeaponPickup *) pRaw;
				CNetMsg_Sv_WeaponPickup Out;
				Out.m_Weapon = pIn->m_Weapon;
				CMsgPacker Packer(NETMSGTYPE_SV_WEAPONPICKUP);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_EMOTICON:
			{
				protocol7::CNetMsg_Sv_Emoticon *pIn = (protocol7::CNetMsg_Sv_Emoticon *) pRaw;
				CNetMsg_Sv_Emoticon Out;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_Emoticon = pIn->m_Emoticon;
				CMsgPacker Packer(NETMSGTYPE_SV_EMOTICON);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_VOTECLEAROPTIONS:
			{
				CNetMsg_Sv_VoteClearOptions Out;
				CMsgPacker Packer(NETMSGTYPE_SV_VOTECLEAROPTIONS);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_VOTEOPTIONADD:
			{
				protocol7::CNetMsg_Sv_VoteOptionAdd *pIn = (protocol7::CNetMsg_Sv_VoteOptionAdd *) pRaw;
				CNetMsg_Sv_VoteOptionAdd Out;
				Out.m_pDescription = pIn->m_pDescription;
				CMsgPacker Packer(NETMSGTYPE_SV_VOTEOPTIONADD);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_VOTEOPTIONREMOVE:
			{
				protocol7::CNetMsg_Sv_VoteOptionRemove *pIn = (protocol7::CNetMsg_Sv_VoteOptionRemove *) pRaw;
				CNetMsg_Sv_VoteOptionRemove Out;
				Out.m_pDescription = pIn->m_pDescription;
				CMsgPacker Packer(NETMSGTYPE_SV_VOTEOPTIONREMOVE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_VOTESET:
			{
				protocol7::CNetMsg_Sv_VoteSet *pIn = (protocol7::CNetMsg_Sv_VoteSet *) pRaw;
				const int Type8 = MapVoteType7To8(pIn->m_Type);
				if(Type8 < 0)
					return -1;
				CNetMsg_Sv_VoteSet Out;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_Type = Type8;
				Out.m_Timeout = pIn->m_Timeout;
				Out.m_pDescription = pIn->m_pDescription;
				Out.m_pReason = pIn->m_pReason;
				CMsgPacker Packer(NETMSGTYPE_SV_VOTESET);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_VOTESTATUS:
			{
				protocol7::CNetMsg_Sv_VoteStatus *pIn = (protocol7::CNetMsg_Sv_VoteStatus *) pRaw;
				CNetMsg_Sv_VoteStatus Out;
				Out.m_Yes = pIn->m_Yes;
				Out.m_No = pIn->m_No;
				Out.m_Pass = pIn->m_Pass;
				Out.m_Total = pIn->m_Total;
				CMsgPacker Packer(NETMSGTYPE_SV_VOTESTATUS);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_SERVERSETTINGS:
			{
				protocol7::CNetMsg_Sv_ServerSettings *pIn = (protocol7::CNetMsg_Sv_ServerSettings *) pRaw;
				CNetMsg_Sv_ServerSettings Out;
				Out.m_KickVote = pIn->m_KickVote;
				Out.m_KickMin = pIn->m_KickMin;
				Out.m_SpecVote = pIn->m_SpecVote;
				Out.m_TeamLock = pIn->m_TeamLock;
				Out.m_TeamBalance = pIn->m_TeamBalance;
				Out.m_PlayerSlots = pIn->m_PlayerSlots;
				Out.m_AllowSpecVoting = pIn->m_AllowSpecVoting;
				CMsgPacker Packer(NETMSGTYPE_SV_SERVERSETTINGS);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_CLIENTINFO:
			{
				protocol7::CNetMsg_Sv_ClientInfo *pIn = (protocol7::CNetMsg_Sv_ClientInfo *) pRaw;
				AbsorbClientInfo(pIn->m_ClientID, pIn->m_Local, pIn->m_Team, pIn->m_Country,
					pIn->m_pName, pIn->m_pClan, pIn->m_apSkinPartNames, pIn->m_aUseCustomColors, pIn->m_aSkinPartColors);
				if(pIn->m_Silent)
					return -1;
				CNetMsg_Sv_ClientEnter Out;
				Out.m_ClientID = pIn->m_ClientID;
				CMsgPacker Packer(NETMSGTYPE_SV_CLIENTENTER);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_GAMEINFO:
			{
				protocol7::CNetMsg_Sv_GameInfo *pIn = (protocol7::CNetMsg_Sv_GameInfo *) pRaw;
				CNetMsg_Sv_GameInfo Out;
				Out.m_GameFlags = pIn->m_GameFlags;
				Out.m_ScoreLimit = pIn->m_ScoreLimit;
				Out.m_TimeLimit = pIn->m_TimeLimit;
				Out.m_MatchNum = pIn->m_MatchNum;
				Out.m_MatchCurrent = pIn->m_MatchCurrent;
				CMsgPacker Packer(NETMSGTYPE_SV_GAMEINFO);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_CLIENTDROP:
			{
				protocol7::CNetMsg_Sv_ClientDrop *pIn = (protocol7::CNetMsg_Sv_ClientDrop *) pRaw;
				if(pIn->m_ClientID >= 0 && pIn->m_ClientID < MAX_CLIENTS7)
					m_aClients7[pIn->m_ClientID].Reset();
				if(pIn->m_Silent)
					return -1;
				CNetMsg_Sv_ClientDrop Out;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_pReason = pIn->m_pReason;
				CMsgPacker Packer(NETMSGTYPE_SV_CLIENTDROP);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_DE_CLIENTENTER:
			{
				protocol7::CNetMsg_De_ClientEnter *pIn = (protocol7::CNetMsg_De_ClientEnter *) pRaw;
				const int ClientID = pIn->m_ClientID;
				if(ClientID >= 0 && ClientID < MAX_CLIENTS7)
				{
					CClientState7 *pState = &m_aClients7[ClientID];
					pState->m_Active = true;
					pState->m_Team = pIn->m_Team;
					str_copy_fixed(pState->m_aName, pIn->m_pName, sizeof(pState->m_aName));
				}
				CNetMsg_Sv_ClientEnter Out;
				Out.m_ClientID = ClientID;
				CMsgPacker Packer(NETMSGTYPE_SV_CLIENTENTER);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_DE_CLIENTLEAVE:
			{
				protocol7::CNetMsg_De_ClientLeave *pIn = (protocol7::CNetMsg_De_ClientLeave *) pRaw;
				if(pIn->m_ClientID >= 0 && pIn->m_ClientID < MAX_CLIENTS7)
					m_aClients7[pIn->m_ClientID].Reset();
				CNetMsg_Sv_ClientDrop Out;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_pReason = pIn->m_pReason;
				CMsgPacker Packer(NETMSGTYPE_SV_CLIENTDROP);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_SKINCHANGE:
			{
				protocol7::CNetMsg_Sv_SkinChange *pIn = (protocol7::CNetMsg_Sv_SkinChange *) pRaw;
				const int ClientID = pIn->m_ClientID;
				if(ClientID >= 0 && ClientID < MAX_CLIENTS7)
				{
					CClientState7 *pState = &m_aClients7[ClientID];
					for(int p = 0; p < NUM_SKINPARTS; p++)
					{
						str_copy_fixed(pState->m_aaSkinPartNames[p], pIn->m_apSkinPartNames[p], sizeof(pState->m_aaSkinPartNames[p]));
						pState->m_aUseCustomColors[p] = pIn->m_aUseCustomColors[p];
						pState->m_aSkinPartColors[p] = pIn->m_aSkinPartColors[p];
					}
				}
				return -1; // absorbed into the TeeInfo snapshot object
			}
			case protocol7::NETMSGTYPE_SV_RACEFINISH:
			{
				protocol7::CNetMsg_Sv_RaceFinish *pIn = (protocol7::CNetMsg_Sv_RaceFinish *) pRaw;
				CNetMsg_Sv_RaceFinish Out;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_Time = pIn->m_Time;
				Out.m_Diff = pIn->m_Diff;
				Out.m_RecordPersonal = pIn->m_RecordPersonal;
				Out.m_RecordServer = pIn->m_RecordServer;
				CMsgPacker Packer(NETMSGTYPE_SV_RACEFINISH);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_CHECKPOINT:
			{
				protocol7::CNetMsg_Sv_Checkpoint *pIn = (protocol7::CNetMsg_Sv_Checkpoint *) pRaw;
				CNetMsg_Sv_Checkpoint Out;
				Out.m_Diff = pIn->m_Diff;
				CMsgPacker Packer(NETMSGTYPE_SV_CHECKPOINT);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_COMMANDINFO:
			{
				protocol7::CNetMsg_Sv_CommandInfo *pIn = (protocol7::CNetMsg_Sv_CommandInfo *) pRaw;
				CNetMsg_Sv_CommandInfo Out;
				Out.m_Name = pIn->m_Name;
				Out.m_ArgsFormat = pIn->m_ArgsFormat;
				Out.m_HelpText = pIn->m_HelpText;
				CMsgPacker Packer(NETMSGTYPE_SV_COMMANDINFO);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_SV_COMMANDINFOREMOVE:
			{
				protocol7::CNetMsg_Sv_CommandInfoRemove *pIn = (protocol7::CNetMsg_Sv_CommandInfoRemove *) pRaw;
				CNetMsg_Sv_CommandInfoRemove Out;
				Out.m_Name = pIn->m_Name;
				CMsgPacker Packer(NETMSGTYPE_SV_COMMANDINFOREMOVE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}

			// 0.7 CL_* ids differ from 0.8 (CL_STARTINFO is 27 vs 33), so each
			// case is keyed on the protocol7 id it arrives with.
			case protocol7::NETMSGTYPE_CL_SAY:
			{
				protocol7::CNetMsg_Cl_Say *pIn = (protocol7::CNetMsg_Cl_Say *) pRaw;
				CNetMsg_Cl_Say Out;
				Out.m_Mode = pIn->m_Mode;
				Out.m_Target = pIn->m_Target;
				Out.m_pMessage = pIn->m_pMessage;
				CMsgPacker Packer(NETMSGTYPE_CL_SAY);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_CL_KILL:
			{
				protocol7::CNetMsg_Cl_Kill *pIn = (protocol7::CNetMsg_Cl_Kill *) pRaw;
				(void) pIn;
				CNetMsg_Cl_Kill Out;
				CMsgPacker Packer(NETMSGTYPE_CL_KILL);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_CL_SETTEAM:
			{
				protocol7::CNetMsg_Cl_SetTeam *pIn = (protocol7::CNetMsg_Cl_SetTeam *) pRaw;
				CNetMsg_Cl_SetTeam Out;
				Out.m_Team = pIn->m_Team;
				CMsgPacker Packer(NETMSGTYPE_CL_SETTEAM);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_CL_SETSPECTATORMODE:
			{
				protocol7::CNetMsg_Cl_SetSpectatorMode *pIn = (protocol7::CNetMsg_Cl_SetSpectatorMode *) pRaw;
				CNetMsg_Cl_SetSpectatorMode Out;
				Out.m_SpecMode = pIn->m_SpecMode;
				Out.m_SpectatorID = pIn->m_SpectatorID;
				CMsgPacker Packer(NETMSGTYPE_CL_SETSPECTATORMODE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_CL_VOTE:
			{
				protocol7::CNetMsg_Cl_Vote *pIn = (protocol7::CNetMsg_Cl_Vote *) pRaw;
				CNetMsg_Cl_Vote Out;
				Out.m_Vote = pIn->m_Vote;
				CMsgPacker Packer(NETMSGTYPE_CL_VOTE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_CL_CALLVOTE:
			{
				protocol7::CNetMsg_Cl_CallVote *pIn = (protocol7::CNetMsg_Cl_CallVote *) pRaw;
				CNetMsg_Cl_CallVote Out;
				Out.m_Type = pIn->m_Type;
				Out.m_Value = pIn->m_Value;
				Out.m_Reason = pIn->m_Reason;
				Out.m_Force = pIn->m_Force;
				CMsgPacker Packer(NETMSGTYPE_CL_CALLVOTE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_CL_EMOTICON:
			{
				protocol7::CNetMsg_Cl_Emoticon *pIn = (protocol7::CNetMsg_Cl_Emoticon *) pRaw;
				CNetMsg_Cl_Emoticon Out;
				Out.m_Emoticon = pIn->m_Emoticon;
				CMsgPacker Packer(NETMSGTYPE_CL_EMOTICON);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_CL_SKINCHANGE:
			{
				protocol7::CNetMsg_Cl_SkinChange *pIn = (protocol7::CNetMsg_Cl_SkinChange *) pRaw;
				CNetMsg_Cl_SkinChange Out;
				for(int p = 0; p < NUM_SKINPARTS; p++)
				{
					Out.m_apSkinPartNames[p] = pIn->m_apSkinPartNames[p];
					Out.m_aUseCustomColors[p] = pIn->m_aUseCustomColors[p];
					Out.m_aSkinPartColors[p] = pIn->m_aSkinPartColors[p];
				}
				CMsgPacker Packer(NETMSGTYPE_CL_SKINCHANGE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_CL_READYCHANGE:
			{
				protocol7::CNetMsg_Cl_ReadyChange *pIn = (protocol7::CNetMsg_Cl_ReadyChange *) pRaw;
				(void) pIn;
				CNetMsg_Cl_ReadyChange Out;
				CMsgPacker Packer(NETMSGTYPE_CL_READYCHANGE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_CL_STARTINFO:
			{
				protocol7::CNetMsg_Cl_StartInfo *pIn = (protocol7::CNetMsg_Cl_StartInfo *) pRaw;
				CNetMsg_Cl_StartInfo Out;
				Out.m_pName = pIn->m_pName;
				Out.m_pClan = pIn->m_pClan;
				Out.m_Country = pIn->m_Country;
				for(int p = 0; p < NUM_SKINPARTS; p++)
				{
					Out.m_apSkinPartNames[p] = pIn->m_apSkinPartNames[p];
					Out.m_aUseCustomColors[p] = pIn->m_aUseCustomColors[p];
					Out.m_aSkinPartColors[p] = pIn->m_aSkinPartColors[p];
				}
				CMsgPacker Packer(NETMSGTYPE_CL_STARTINFO);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case protocol7::NETMSGTYPE_CL_COMMAND:
			{
				protocol7::CNetMsg_Cl_Command *pIn = (protocol7::CNetMsg_Cl_Command *) pRaw;
				CNetMsg_Cl_Command Out;
				Out.m_Name = pIn->m_Name;
				Out.m_Arguments = pIn->m_Arguments;
				CMsgPacker Packer(NETMSGTYPE_CL_COMMAND);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
		}
		return -1;
	}

	int CNetworkTranslator::TranslateClientMsg(const void *pMsg8, int Size8, void *pOut, int OutSize)
	{
		CMsgUnpacker Unpacker(pMsg8, Size8);
		if(Unpacker.Error())
			return -2;
		const int MsgId8 = Unpacker.Type();

		static CNetObjHandler Handler8;
		void *pRaw = Handler8.SecureUnpackMsg(MsgId8, &Unpacker);
		if(!pRaw)
			return -2;

		switch(MsgId8)
		{
			case NETMSGTYPE_CL_SAY:
			{
				CNetMsg_Cl_Say *pIn = (CNetMsg_Cl_Say *) pRaw;
				protocol7::CNetMsg_Cl_Say Out;
				Out.m_Mode = pIn->m_Mode;
				Out.m_Target = pIn->m_Target;
				Out.m_pMessage = pIn->m_pMessage;
				CMsgPacker Packer(protocol7::NETMSGTYPE_CL_SAY);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_CL_KILL:
			{
				protocol7::CNetMsg_Cl_Kill Out;
				CMsgPacker Packer(protocol7::NETMSGTYPE_CL_KILL);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_CL_SETTEAM:
			{
				CNetMsg_Cl_SetTeam *pIn = (CNetMsg_Cl_SetTeam *) pRaw;
				protocol7::CNetMsg_Cl_SetTeam Out;
				Out.m_Team = pIn->m_Team;
				CMsgPacker Packer(protocol7::NETMSGTYPE_CL_SETTEAM);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_CL_SETSPECTATORMODE:
			{
				CNetMsg_Cl_SetSpectatorMode *pIn = (CNetMsg_Cl_SetSpectatorMode *) pRaw;
				protocol7::CNetMsg_Cl_SetSpectatorMode Out;
				Out.m_SpecMode = pIn->m_SpecMode;
				Out.m_SpectatorID = pIn->m_SpectatorID;
				CMsgPacker Packer(protocol7::NETMSGTYPE_CL_SETSPECTATORMODE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_CL_VOTE:
			{
				CNetMsg_Cl_Vote *pIn = (CNetMsg_Cl_Vote *) pRaw;
				protocol7::CNetMsg_Cl_Vote Out;
				Out.m_Vote = pIn->m_Vote;
				CMsgPacker Packer(protocol7::NETMSGTYPE_CL_VOTE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_CL_CALLVOTE:
			{
				CNetMsg_Cl_CallVote *pIn = (CNetMsg_Cl_CallVote *) pRaw;
				protocol7::CNetMsg_Cl_CallVote Out;
				Out.m_Type = pIn->m_Type;
				Out.m_Value = pIn->m_Value;
				Out.m_Reason = pIn->m_Reason;
				Out.m_Force = pIn->m_Force;
				CMsgPacker Packer(protocol7::NETMSGTYPE_CL_CALLVOTE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_CL_EMOTICON:
			{
				CNetMsg_Cl_Emoticon *pIn = (CNetMsg_Cl_Emoticon *) pRaw;
				protocol7::CNetMsg_Cl_Emoticon Out;
				Out.m_Emoticon = pIn->m_Emoticon;
				CMsgPacker Packer(protocol7::NETMSGTYPE_CL_EMOTICON);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_CL_SKINCHANGE:
			{
				CNetMsg_Cl_SkinChange *pIn = (CNetMsg_Cl_SkinChange *) pRaw;
				protocol7::CNetMsg_Cl_SkinChange Out;
				for(int p = 0; p < NUM_SKINPARTS; p++)
				{
					Out.m_apSkinPartNames[p] = pIn->m_apSkinPartNames[p];
					Out.m_aUseCustomColors[p] = pIn->m_aUseCustomColors[p];
					Out.m_aSkinPartColors[p] = pIn->m_aSkinPartColors[p];
				}
				CMsgPacker Packer(protocol7::NETMSGTYPE_CL_SKINCHANGE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_CL_READYCHANGE:
			{
				protocol7::CNetMsg_Cl_ReadyChange Out;
				CMsgPacker Packer(protocol7::NETMSGTYPE_CL_READYCHANGE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_CL_STARTINFO:
			{
				CNetMsg_Cl_StartInfo *pIn = (CNetMsg_Cl_StartInfo *) pRaw;
				protocol7::CNetMsg_Cl_StartInfo Out;
				Out.m_pName = pIn->m_pName;
				Out.m_pClan = pIn->m_pClan;
				Out.m_Country = pIn->m_Country;
				for(int p = 0; p < NUM_SKINPARTS; p++)
				{
					Out.m_apSkinPartNames[p] = pIn->m_apSkinPartNames[p];
					Out.m_aUseCustomColors[p] = pIn->m_aUseCustomColors[p];
					Out.m_aSkinPartColors[p] = pIn->m_aSkinPartColors[p];
				}
				CMsgPacker Packer(protocol7::NETMSGTYPE_CL_STARTINFO);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_CL_COMMAND:
			{
				CNetMsg_Cl_Command *pIn = (CNetMsg_Cl_Command *) pRaw;
				protocol7::CNetMsg_Cl_Command Out;
				Out.m_Name = pIn->m_Name;
				Out.m_Arguments = pIn->m_Arguments;
				CMsgPacker Packer(protocol7::NETMSGTYPE_CL_COMMAND);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
		}
		return -1;
	}

	int CNetworkTranslator::TranslateServerChunk(const void *pInData, int InSize, CNetChunk *pOutChunks, int MaxOutChunks)
	{
		CMsgUnpacker Unpacker(pInData, InSize);
		if(Unpacker.Error())
			return 0;

		if(Unpacker.System())
		{
			const int MsgId = Unpacker.Type();
			// A 0.7 client cannot send a snapshot, so this is never legitimate
			// traffic and is dropped.
			if(MsgId == NETMSG_SNAP || MsgId == NETMSG_SNAPSINGLE || MsgId == NETMSG_SNAPEMPTY)
				return 0;

			// A map change restarts the tick domain, so stale baselines and
			// history would alias the new ticks.
			if(MsgId == NETMSG_MAP_CHANGE)
				Reset();

			// A 0.7 client reports the 0.7 netversion, which the server rejects
			// as a mismatch, so the first field is rewritten to ours. The rest
			// (password, client version, server-info version) passes through.
			if(MsgId == NETMSG_INFO)
			{
				const char *pVersion = Unpacker.GetString(CUnpacker::SANITIZE_CC);
				const char *pRest = Unpacker.GetString(CUnpacker::SANITIZE_CC);
				const int ClientVersion = Unpacker.GetInt();
				const int ServerInfoVersion = Unpacker.GetIntOrDefault(SERVERINFO_VERSION_LEGACY);
				if(Unpacker.Error())
					return 0;

				CMsgPacker Packer(NETMSG_INFO, true);
				Packer.AddString(m_pNetVersion, 0);
				Packer.AddString(pRest, 0);
				Packer.AddInt(ClientVersion);
				Packer.AddInt(ServerInfoVersion);
				if(Packer.Error())
					return 0;
				if(MaxOutChunks < 1)
					return 0;
				m_aServerOut[0].set_size(Packer.Size());
				mem_copy(m_aServerOut[0].base_ptr(), Packer.Data(), Packer.Size());
				pOutChunks[0].m_ClientID = 0;
				pOutChunks[0].m_Flags = NETSENDFLAG_VITAL;
				pOutChunks[0].m_DataSize = Packer.Size();
				pOutChunks[0].m_pData = m_aServerOut[0].base_ptr();
				(void) pVersion;
				return 1;
			}

			// other system messages are frozen and pass through unchanged
			if(MaxOutChunks < 1)
				return 0;
			m_aServerOut[0].set_size(InSize);
			mem_copy(m_aServerOut[0].base_ptr(), pInData, InSize);
			pOutChunks[0].m_ClientID = 0;
			pOutChunks[0].m_Flags = NETSENDFLAG_VITAL;
			pOutChunks[0].m_DataSize = InSize;
			pOutChunks[0].m_pData = m_aServerOut[0].base_ptr();
			return 1;
		}

		if(MaxOutChunks < 1)
			return 0;
		m_aServerOut[0].set_size(InSize + 4096);
		const int Written = TranslateServerMsg(pInData, InSize, m_aServerOut[0].base_ptr(), m_aServerOut[0].size());
		if(Written < 0)
			return 0;
		pOutChunks[0].m_ClientID = 0;
		pOutChunks[0].m_Flags = NETSENDFLAG_VITAL;
		pOutChunks[0].m_DataSize = Written;
		pOutChunks[0].m_pData = m_aServerOut[0].base_ptr();
		return 1;
	}

	// Lowers one of our own 0.8 server->client messages into its 0.7 form. This is
	// the mirror of TranslateServerMsg and covers the whole SV_* surface 0.8 can
	// send, including the messages that only exist on one side.
	int CNetworkTranslator::TranslateServerToClientMsg(const void *pMsg8, int Size8, void *pOut, int OutSize)
	{
		CMsgUnpacker Unpacker(pMsg8, Size8);
		if(Unpacker.Error())
			return -2;
		const int MsgId8 = Unpacker.Type();

		// Messages whose payload is not described by the table: copy it verbatim.
		if(MsgId8 == NETMSGTYPE_SV_VOTEOPTIONLISTADD)
			return CopyRawMsg(protocol7::NETMSGTYPE_SV_VOTEOPTIONLISTADD, &Unpacker, pOut, OutSize);
		if(MsgId8 == NETMSGTYPE_SV_GAMEMSG)
			return CopyRawMsg(protocol7::NETMSGTYPE_SV_GAMEMSG, &Unpacker, pOut, OutSize);

		static CNetObjHandler Handler8;
		void *pRaw = Handler8.SecureUnpackMsg(MsgId8, &Unpacker);
		if(!pRaw)
			return -2;

		switch(MsgId8)
		{
			case NETMSGTYPE_SV_MOTD:
			{
				CNetMsg_Sv_Motd *pIn = (CNetMsg_Sv_Motd *) pRaw;
				protocol7::CNetMsg_Sv_Motd Out;
				Out.m_pMessage = pIn->m_pMessage;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_MOTD);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_BROADCAST:
			{
				CNetMsg_Sv_Broadcast *pIn = (CNetMsg_Sv_Broadcast *) pRaw;
				protocol7::CNetMsg_Sv_Broadcast Out;
				Out.m_pMessage = pIn->m_pMessage;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_BROADCAST);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_CHAT:
			{
				CNetMsg_Sv_Chat *pIn = (CNetMsg_Sv_Chat *) pRaw;
				protocol7::CNetMsg_Sv_Chat Out;
				Out.m_Mode = pIn->m_Mode;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_TargetID = pIn->m_TargetID;
				Out.m_pMessage = pIn->m_pMessage;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_CHAT);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_TEAM:
			{
				CNetMsg_Sv_Team *pIn = (CNetMsg_Sv_Team *) pRaw;
				// keep the cached identity in step so the TeeInfo->De_ClientInfo
				// split reports the new team
				if(pIn->m_ClientID >= 0 && pIn->m_ClientID < MAX_TEES8)
					m_aTees8[pIn->m_ClientID].m_Team = pIn->m_Team;
				protocol7::CNetMsg_Sv_Team Out;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_Team = pIn->m_Team;
				Out.m_Silent = pIn->m_Silent;
				Out.m_CooldownTick = pIn->m_CooldownTick;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_TEAM);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_KILLMSG:
			{
				CNetMsg_Sv_KillMsg *pIn = (CNetMsg_Sv_KillMsg *) pRaw;
				protocol7::CNetMsg_Sv_KillMsg Out;
				Out.m_Killer = pIn->m_Killer;
				Out.m_Victim = pIn->m_Victim;
				Out.m_Weapon = pIn->m_Weapon;
				Out.m_ModeSpecial = pIn->m_ModeSpecial;
				Out.m_Assist = pIn->m_Assist;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_KILLMSG);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_READYTOENTER:
			{
				protocol7::CNetMsg_Sv_ReadyToEnter Out;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_READYTOENTER);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_WEAPONPICKUP:
			{
				CNetMsg_Sv_WeaponPickup *pIn = (CNetMsg_Sv_WeaponPickup *) pRaw;
				protocol7::CNetMsg_Sv_WeaponPickup Out;
				Out.m_Weapon = pIn->m_Weapon;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_WEAPONPICKUP);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_EMOTICON:
			{
				CNetMsg_Sv_Emoticon *pIn = (CNetMsg_Sv_Emoticon *) pRaw;
				protocol7::CNetMsg_Sv_Emoticon Out;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_Emoticon = pIn->m_Emoticon;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_EMOTICON);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_VOTECLEAROPTIONS:
			{
				protocol7::CNetMsg_Sv_VoteClearOptions Out;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_VOTECLEAROPTIONS);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_VOTEOPTIONADD:
			{
				CNetMsg_Sv_VoteOptionAdd *pIn = (CNetMsg_Sv_VoteOptionAdd *) pRaw;
				protocol7::CNetMsg_Sv_VoteOptionAdd Out;
				Out.m_pDescription = pIn->m_pDescription;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_VOTEOPTIONADD);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_VOTEOPTIONREMOVE:
			{
				CNetMsg_Sv_VoteOptionRemove *pIn = (CNetMsg_Sv_VoteOptionRemove *) pRaw;
				protocol7::CNetMsg_Sv_VoteOptionRemove Out;
				Out.m_pDescription = pIn->m_pDescription;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_VOTEOPTIONREMOVE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_VOTESET:
			{
				CNetMsg_Sv_VoteSet *pIn = (CNetMsg_Sv_VoteSet *) pRaw;
				const int Type7 = MapVoteType8To7(pIn->m_Type);
				if(Type7 < 0)
					return -1;
				protocol7::CNetMsg_Sv_VoteSet Out;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_Type = Type7;
				Out.m_Timeout = pIn->m_Timeout;
				Out.m_pDescription = pIn->m_pDescription;
				Out.m_pReason = pIn->m_pReason;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_VOTESET);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_VOTESTATUS:
			{
				CNetMsg_Sv_VoteStatus *pIn = (CNetMsg_Sv_VoteStatus *) pRaw;
				protocol7::CNetMsg_Sv_VoteStatus Out;
				Out.m_Yes = pIn->m_Yes;
				Out.m_No = pIn->m_No;
				Out.m_Pass = pIn->m_Pass;
				Out.m_Total = pIn->m_Total;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_VOTESTATUS);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_SERVERSETTINGS:
			{
				CNetMsg_Sv_ServerSettings *pIn = (CNetMsg_Sv_ServerSettings *) pRaw;
				protocol7::CNetMsg_Sv_ServerSettings Out;
				Out.m_KickVote = pIn->m_KickVote;
				Out.m_KickMin = pIn->m_KickMin;
				Out.m_SpecVote = pIn->m_SpecVote;
				Out.m_TeamLock = pIn->m_TeamLock;
				Out.m_TeamBalance = pIn->m_TeamBalance;
				Out.m_PlayerSlots = pIn->m_PlayerSlots;
				Out.m_AllowSpecVoting = pIn->m_AllowSpecVoting;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_SERVERSETTINGS);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_CLIENTENTER:
			{
				// 0.8 only announces that a tee entered; the identity travels in
				// the TeeInfo snapshot object. 0.7 has no TeeInfo, so the cached
				// identity is expanded back into a full Sv_ClientInfo here.
				CNetMsg_Sv_ClientEnter *pIn = (CNetMsg_Sv_ClientEnter *) pRaw;
				const int TeeID = pIn->m_ClientID;
				if(TeeID < 0 || TeeID >= MAX_CLIENTS7)
					return -1;
				const CTeeState8 *pState = &m_aTees8[TeeID];
				if(!pState->m_Active)
					return -1; // identity not known yet; the snapshot will carry it
				protocol7::CNetMsg_Sv_ClientInfo Out;
				Out.m_ClientID = TeeID;
				Out.m_Local = (pState->m_Flag & TEEFLAG_LOCAL) ? 1 : 0;
				Out.m_Team = pState->m_Team;
				Out.m_pName = pState->m_aName;
				Out.m_pClan = pState->m_aClan;
				Out.m_Country = pState->m_Country;
				for(int p = 0; p < NUM_SKINPARTS; p++)
				{
					Out.m_apSkinPartNames[p] = pState->m_aaSkinPartNames[p];
					Out.m_aUseCustomColors[p] = pState->m_aUseCustomColors[p];
					Out.m_aSkinPartColors[p] = pState->m_aSkinPartColors[p];
				}
				Out.m_Silent = 0;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_CLIENTINFO);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_CLIENTDROP:
			{
				CNetMsg_Sv_ClientDrop *pIn = (CNetMsg_Sv_ClientDrop *) pRaw;
				if(pIn->m_ClientID >= 0 && pIn->m_ClientID < MAX_TEES8)
				{
					m_aTees8[pIn->m_ClientID].m_Active = false;
					m_aTeeActive8[pIn->m_ClientID] = false;
				}
				protocol7::CNetMsg_Sv_ClientDrop Out;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_pReason = pIn->m_pReason;
				Out.m_Silent = 0; // 0.7 has the field, 0.8 no longer sends it
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_CLIENTDROP);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_GAMEINFO:
			{
				CNetMsg_Sv_GameInfo *pIn = (CNetMsg_Sv_GameInfo *) pRaw;
				protocol7::CNetMsg_Sv_GameInfo Out;
				Out.m_GameFlags = pIn->m_GameFlags;
				Out.m_ScoreLimit = pIn->m_ScoreLimit;
				Out.m_TimeLimit = pIn->m_TimeLimit;
				Out.m_MatchNum = pIn->m_MatchNum;
				Out.m_MatchCurrent = pIn->m_MatchCurrent;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_GAMEINFO);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_RACEFINISH:
			{
				CNetMsg_Sv_RaceFinish *pIn = (CNetMsg_Sv_RaceFinish *) pRaw;
				protocol7::CNetMsg_Sv_RaceFinish Out;
				Out.m_ClientID = pIn->m_ClientID;
				Out.m_Time = pIn->m_Time;
				Out.m_Diff = pIn->m_Diff;
				Out.m_RecordPersonal = pIn->m_RecordPersonal;
				Out.m_RecordServer = pIn->m_RecordServer;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_RACEFINISH);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_CHECKPOINT:
			{
				CNetMsg_Sv_Checkpoint *pIn = (CNetMsg_Sv_Checkpoint *) pRaw;
				protocol7::CNetMsg_Sv_Checkpoint Out;
				Out.m_Diff = pIn->m_Diff;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_CHECKPOINT);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_COMMANDINFO:
			{
				CNetMsg_Sv_CommandInfo *pIn = (CNetMsg_Sv_CommandInfo *) pRaw;
				protocol7::CNetMsg_Sv_CommandInfo Out;
				Out.m_Name = pIn->m_Name;
				Out.m_ArgsFormat = pIn->m_ArgsFormat;
				Out.m_HelpText = pIn->m_HelpText;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_COMMANDINFO);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
			case NETMSGTYPE_SV_COMMANDINFOREMOVE:
			{
				CNetMsg_Sv_CommandInfoRemove *pIn = (CNetMsg_Sv_CommandInfoRemove *) pRaw;
				protocol7::CNetMsg_Sv_CommandInfoRemove Out;
				Out.m_Name = pIn->m_Name;
				CMsgPacker Packer(protocol7::NETMSGTYPE_SV_COMMANDINFOREMOVE);
				Out.Pack(&Packer);
				return FinishMsg(&Packer, pOut, OutSize);
			}
		}
		return -1;
	}

	int CNetworkTranslator::TranslateServerToClientChunk(const void *pInData, int InSize, int Flags, CNetChunk *pOutChunks, int MaxOutChunks)
	{
		if(MaxOutChunks < 1)
			return 0;

		// system message ids are frozen between 0.7 and 0.8, so NETMSG_READY /
		// NETMSG_ENTERGAME pass through unchanged
		CMsgUnpacker Unpacker(pInData, InSize);
		if(Unpacker.Error())
			return 0;

		if(Unpacker.System())
		{
			const int MsgId8 = Unpacker.Type();
			// Snapshot payloads are system messages too and need real work; only
			// the non-snapshot system messages pass through untouched.
			if(MsgId8 == NETMSG_SNAP || MsgId8 == NETMSG_SNAPSINGLE || MsgId8 == NETMSG_SNAPEMPTY)
				return HandleSnapshot8(&Unpacker, MsgId8, pOutChunks, MaxOutChunks);

			m_aClientOut[0].set_size(InSize);
			mem_copy(m_aClientOut[0].base_ptr(), pInData, InSize);
			pOutChunks[0].m_ClientID = 0;
			pOutChunks[0].m_Flags = Flags;
			pOutChunks[0].m_DataSize = InSize;
			pOutChunks[0].m_pData = m_aClientOut[0].base_ptr();
			return 1;
		}

		// Sv_TuneParams does not exist in 0.8 (it is 0.7-only), so our server can
		// never emit it and there is nothing to synthesize here: tuning travels to
		// the 0.7 client as the De_TuneParams snapshot object built by
		// BuildSnapshot7.

		m_aClientOut[0].set_size(InSize + 4096);
		const int Written = TranslateServerToClientMsg(pInData, InSize, m_aClientOut[0].base_ptr(), m_aClientOut[0].size());
		if(Written < 0)
			return 0;
		pOutChunks[0].m_ClientID = 0;
		pOutChunks[0].m_Flags = Flags;
		pOutChunks[0].m_DataSize = Written;
		pOutChunks[0].m_pData = m_aClientOut[0].base_ptr();
		return 1;
	}

	int CNetworkTranslator::TranslateClientChunk(const void *pInData, int InSize, int Flags, CNetChunk *pOutChunks, int MaxOutChunks)
	{
		if(MaxOutChunks < 1)
			return 0;

		// system message ids are frozen between 0.7 and 0.8, so NETMSG_READY /
		// NETMSG_ENTERGAME pass through unchanged
		CMsgUnpacker Unpacker(pInData, InSize);
		if(Unpacker.Error())
			return 0;
		if(Unpacker.System())
		{
			m_aClientOut[0].set_size(InSize);
			mem_copy(m_aClientOut[0].base_ptr(), pInData, InSize);
			pOutChunks[0].m_ClientID = 0;
			pOutChunks[0].m_Flags = Flags;
			pOutChunks[0].m_DataSize = InSize;
			pOutChunks[0].m_pData = m_aClientOut[0].base_ptr();
			return 1;
		}

		m_aClientOut[0].set_size(InSize + 4096);
		const int Written = TranslateClientMsg(pInData, InSize, m_aClientOut[0].base_ptr(), m_aClientOut[0].size());
		if(Written < 0)
			return 0;
		pOutChunks[0].m_ClientID = 0;
		pOutChunks[0].m_Flags = Flags;
		pOutChunks[0].m_DataSize = Written;
		pOutChunks[0].m_pData = m_aClientOut[0].base_ptr();
		return 1;
	}
} // namespace legacy
