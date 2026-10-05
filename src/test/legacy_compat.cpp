/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include "test.h"
#include <gtest/gtest.h>

#include <base/system.h>

#include <engine/shared/legacy/network7.h>
#include <engine/shared/legacy/network_translator.h>
#include <engine/shared/compression.h>
#include <engine/shared/snapshot.h>

#include <generated/protocol.h>
#include <generated/protocol7.h>

// Tests for the 0.7 handshake, the frozen 0.7 delta codec and snapshot
// translation in both directions.

TEST(LegacyCompat, HandshakeCapabilitiesAreFrozen)
{
	// A 0.7 peer that does not know the extension leaves the byte zero, which
	// must still select plain Huffman.
	EXPECT_EQ(legacy::Net7SelectCompression(0), legacy::NET7_COMPRESSION_HUFFMAN);
	// The only defined capability bit selects zstd.
	EXPECT_EQ(legacy::Net7SelectCompression(legacy::NET7_CTRLFLAG_ZSTD_DICT), legacy::NET7_COMPRESSION_ZSTD);
	// Unknown bits must be ignored rather than mistaken for a codec.
	EXPECT_EQ(legacy::Net7SelectCompression(0x80), legacy::NET7_COMPRESSION_HUFFMAN);

	EXPECT_EQ(legacy::Net7BuildClientCapabilities(false), 0);
	EXPECT_EQ(legacy::Net7BuildClientCapabilities(true), legacy::NET7_CTRLFLAG_ZSTD_DICT);
	EXPECT_EQ(legacy::Net7BuildAcceptCapabilities(true), legacy::NET7_CTRLFLAG_ZSTD_DICT);

	// A CONNECT without the extension byte reads as Huffman, not as garbage.
	unsigned char aBare[legacy::NET7_CTRL_CONNECT_CAPABILITY_OFFSET] = {0};
	EXPECT_EQ(legacy::Net7ReadConnectCapabilities(aBare, sizeof(aBare)), 0);

	// An ACCEPT carrying exactly the capability byte is read back.
	unsigned char aAccept[legacy::NET7_CTRL_ACCEPT_CAPABILITY_OFFSET + 1] = {2, legacy::NET7_CTRLFLAG_ZSTD_DICT};
	EXPECT_EQ(legacy::Net7ReadAcceptCapabilities(aAccept, sizeof(aAccept)), legacy::NET7_CTRLFLAG_ZSTD_DICT);
}

TEST(LegacyCompat, GenerationMarkerSeparatesTheTwoStacks)
{
	// The marker is what tells a 0.7 peer from a 0.8 one at CONNECT time, so the
	// offsets must line up with the frozen 0.7 layout shifted by its size.
	EXPECT_EQ(NET_CTRL_CONNECT_CAPABILITY_OFFSET_8,
		legacy::NET7_CTRL_CONNECT_CAPABILITY_OFFSET + NET_GENERATION_MARKER_SIZE);
	EXPECT_EQ(NET_CTRL_ACCEPT_CAPABILITY_OFFSET_8,
		legacy::NET7_CTRL_ACCEPT_CAPABILITY_OFFSET + NET_GENERATION_MARKER_SIZE);

	unsigned char aBuf[NET_CTRL_ACCEPT_CAPABILITY_OFFSET_8 + 1] = {0};

	// A buffer that is all zeroes carries no marker: that is exactly what a 0.7
	// peer sends, and it must be recognised as legacy.
	EXPECT_FALSE(Net8HasGenerationMarker(aBuf, sizeof(aBuf), NET_CTRL_ACCEPT_CAPABILITY_OFFSET));

	// Writing the marker makes the same buffer read as 0.8.
	EXPECT_TRUE(Net8WriteGenerationMarker(aBuf, sizeof(aBuf), NET_CTRL_ACCEPT_CAPABILITY_OFFSET));
	EXPECT_TRUE(Net8HasGenerationMarker(aBuf, sizeof(aBuf), NET_CTRL_ACCEPT_CAPABILITY_OFFSET));
	EXPECT_EQ(aBuf[NET_CTRL_ACCEPT_CAPABILITY_OFFSET + 0], NET_GENERATION_MARKER_0);
	EXPECT_EQ(aBuf[NET_CTRL_ACCEPT_CAPABILITY_OFFSET + 1], NET_GENERATION_MARKER_1);
	EXPECT_EQ(aBuf[NET_CTRL_ACCEPT_CAPABILITY_OFFSET + 2], NET_GENERATION_MARKER_2);

	// A buffer too small for the marker must not be written past its end, and
	// must never be mistaken for a 0.8 peer.
	unsigned char aTiny[NET_GENERATION_MARKER_SIZE] = {0};
	// offset 1 leaves only 2 of the 3 bytes the marker needs
	EXPECT_FALSE(Net8WriteGenerationMarker(aTiny, sizeof(aTiny), 1));
	EXPECT_FALSE(Net8HasGenerationMarker(aTiny, sizeof(aTiny), 1));
	// and it really did not write anything
	EXPECT_EQ(aTiny[1], 0);
	EXPECT_EQ(aTiny[2], 0);
}

TEST(LegacyCompat, SnapshotDeltaUsesFrozen07ObjectSizes)
{
	// The 0.7 static item sizes must match what a 0.7 server used, otherwise the
	// delta size fields desync and no snapshot survives decoding.
	protocol7::CNetObjHandler Handler7;
	CSnapshotDelta Delta7;
	legacy::Net7ConfigureSnapshotDelta(&Delta7);

	for(int i = 0; i < 23; i++)
		EXPECT_EQ(Delta7.GetDataRate(i) >= 0, true);

	// The 0.7 PlayerInput object is 10 ints on the wire.
	EXPECT_EQ(Handler7.GetObjSize(protocol7::NETOBJTYPE_PLAYERINPUT), (int) sizeof(protocol7::CNetObj_PlayerInput));

	// And the live (0.8) table must be configured with the 0.8 sizes, so the two
	// codecs are genuinely different where the protocols differ.
	CNetObjHandler Handler8;
	CSnapshotDelta Delta8;
	for(int i = 0; i < NUM_NETOBJTYPES; i++)
		Delta8.SetStaticsize(i, Handler8.GetObjSize(i));

	EXPECT_EQ(Handler8.GetObjSize(NETOBJTYPE_TUNING), (int) sizeof(CNetObj_Tuning));
	// 0.7 has no Tuning object at all; the 0.8 one carries NUM_TUNES params.
	EXPECT_EQ(sizeof(CNetObj_Tuning) / sizeof(int), 32);
}

TEST(LegacyCompat, TranslationDropsNothingOnEmptyInput)
{
	// A translator must be usable immediately and must not read uninitialised
	// state; an empty chunk translates to no output.
	legacy::CNetworkTranslator Translator;
	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
	EXPECT_EQ(Translator.TranslateServerChunk("", 0, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0);
	EXPECT_EQ(Translator.ClientState(0) != nullptr, true);
	EXPECT_EQ(Translator.ClientState(0)->m_Active, false);
	EXPECT_EQ(Translator.TuningValid(), false);

	// Out-of-range client ids are rejected rather than read out of bounds.
	EXPECT_EQ(Translator.ClientState(-1), nullptr);
	EXPECT_EQ(Translator.ClientState(legacy::CNetworkTranslator::MAX_CLIENTS7), nullptr);
}

TEST(LegacyCompat, ResetClearsNegotiatedState)
{
	legacy::CNetworkTranslator Translator;
	Translator.Reset();
	EXPECT_EQ(Translator.TuningValid(), false);
	EXPECT_EQ(Translator.TuningValid8(), false);
	// every client slot is wiped, so no stale 0.7 identity survives a reset
	for(int i = 0; i < legacy::CNetworkTranslator::MAX_CLIENTS7; i++)
		EXPECT_EQ(Translator.ClientState(i)->m_Active, false);
}

// The entry point names follow the reference's topology (0.8 client, 0.7
// server), which is inverted for us: TranslateServerChunk lifts 0.7 -> 0.8 and
// TranslateClientChunk lowers 0.8 -> 0.7. Wiring them the other way round
// silently drops nearly every packet, so the direction is pinned here.
TEST(LegacyCompat, TranslatorEntryPointsAreDirectionPinned)
{
	// A genuine 0.7 message: only the 0.7 -> 0.8 entry point may recognise it.
	{
		protocol7::CNetMsg_Sv_GameInfo GameInfo7;
		GameInfo7.m_GameFlags = 1;
		GameInfo7.m_ScoreLimit = 2;
		GameInfo7.m_TimeLimit = 3;
		GameInfo7.m_MatchNum = 4;
		GameInfo7.m_MatchCurrent = 5;
		CMsgPacker Packer7(protocol7::NETMSGTYPE_SV_GAMEINFO);
		GameInfo7.Pack(&Packer7);

		CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];

		legacy::CNetworkTranslator Translator;
		EXPECT_GT(Translator.TranslateServerChunk(Packer7.Data(), Packer7.Size(), aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0)
			<< "0.7 traffic must be lifted by TranslateServerChunk";

		Translator.Reset();
		EXPECT_EQ(Translator.TranslateClientChunk(Packer7.Data(), Packer7.Size(), 0, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0)
			<< "TranslateClientChunk must not accept 0.7 input";
	}

	// The 0.8 CL_* ids differ from the 0.7 ones (CL_STARTINFO is 33 vs 27), so
	// each direction keys its switch on the input protocol's numbering. This
	// checks the outbound direction, which lowers 0.8 -> 0.7.
	{
		CNetMsg_Cl_Say Say8;
		Say8.m_Mode = 0;
		Say8.m_Target = -1;
		Say8.m_pMessage = "hi";
		CMsgPacker Packer8(NETMSGTYPE_CL_SAY);
		Say8.Pack(&Packer8);

		CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];

		legacy::CNetworkTranslator Translator;
		EXPECT_GT(Translator.TranslateClientChunk(Packer8.Data(), Packer8.Size(), 0, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0)
			<< "0.8 client messages must be lowered by TranslateClientChunk";
	}
}

// TranslateClientChunk only maps CL_* input, so a server-to-client Sv_* message
// must produce no output rather than garbage.
TEST(LegacyCompat, OutboundServerMessagesAreNotTranslatedByTheReference)
{
	CNetMsg_Sv_GameInfo GameInfo8;
	GameInfo8.m_GameFlags = 1;
	GameInfo8.m_ScoreLimit = 2;
	GameInfo8.m_TimeLimit = 3;
	GameInfo8.m_MatchNum = 4;
	GameInfo8.m_MatchCurrent = 5;
	CMsgPacker Packer8(NETMSGTYPE_SV_GAMEINFO);
	GameInfo8.Pack(&Packer8);

	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];

	legacy::CNetworkTranslator Translator;
	EXPECT_EQ(Translator.TranslateClientChunk(Packer8.Data(), Packer8.Size(), 0, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0)
		<< "Sv_* has no 0.8 -> 0.7 mapping in the reference translator";
}


// --- outbound (0.8 server -> 0.7 client) ------------------------------------

// Wraps a finished 0.8 snapshot into a NETMSG_SNAPSINGLE chunk exactly the way
// CServer::DoSnapshot does, and runs it through the outbound translator. The wire
// carries a *delta* produced by CSnapshotDelta::CreateDelta and compressed with
// CVariableInt, not a raw snapshot, so the same transformation is applied here.
static int SendSnapshot8AsServer(legacy::CNetworkTranslator &Translator, CSnapshotBuilder &Builder, CNetChunk *pOut, int MaxOut, int GameTick = 1)
{
	array<unsigned char> aSnap;
	aSnap.set_size(Builder.RequiredSize());
	const int SnapSize = Builder.Finish(aSnap.base_ptr());
	if(SnapSize < 0)
		return 0;

	// Build the delta against an empty baseline, as a first-ever snapshot would be.
	CSnapshotDelta Delta;
	CNetObjHandler Handler;
	for(int i = 0; i < NUM_NETOBJTYPES; i++)
		Delta.SetStaticsize(i, Handler.GetObjSize(i));
	CSnapshot Empty;
	Empty.Clear();
	array<unsigned char> aDelta;
	aDelta.set_size(SnapSize * 4 + 65536);
	const int DeltaSize = Delta.CreateDelta(&Empty, (CSnapshot *) aSnap.base_ptr(), aDelta.base_ptr());
	if(DeltaSize < 0)
		return 0;

	// Compress it the way the server does before putting it on the wire.
	array<unsigned char> aComp;
	aComp.set_size(DeltaSize + DeltaSize / 2 + 4096);
	const int CompSize = CVariableInt::Compress(aDelta.base_ptr(), DeltaSize, aComp.base_ptr(), aComp.size());
	if(CompSize < 0)
		return 0;

	CMsgPacker Packer(NETMSG_SNAPSINGLE, true);
	Packer.AddInt(GameTick);
	// CServer::DoSnapshot uses DeltaTick = -1 for a full snapshot and puts the
	// *distance* (GameTick - DeltaTick) on the wire, so a full snapshot sends
	// GameTick + 1. A 0.7 client's first snapshot is always full.
	Packer.AddInt(GameTick + 1);
	Packer.AddInt(0); // crc (the translator recomputes it)
	Packer.AddInt(CompSize);
	Packer.AddRaw(aComp.base_ptr(), CompSize);
	if(Packer.Error())
		return 0;
	return Translator.TranslateServerToClientChunk(Packer.Data(), Packer.Size(), NETSENDFLAG_VITAL, pOut, MaxOut);
}

TEST(LegacyCompat, OutboundTuneParamsAreSplitFromTuningObject)
{
	// The Tuning snapshot object is 0.8-only; a 0.7 client must receive the same
	// values as the De_TuneParams object instead.
	legacy::CNetworkTranslator Translator;
	CSnapshotBuilder Builder;
	Builder.Init();

	CNetObj_Tuning *pTuning = (CNetObj_Tuning *) Builder.NewItem(NETOBJTYPE_TUNING, 0, sizeof(CNetObj_Tuning));
	ASSERT_NE(pTuning, nullptr);
	for(int i = 0; i < 32; i++)
		pTuning->m_aTuneParams[i] = 500 + i;

	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
	EXPECT_GT(SendSnapshot8AsServer(Translator, Builder, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0);
	EXPECT_TRUE(Translator.TuningValid8());
	EXPECT_EQ(Translator.TuneParams8()[0], 500);
	EXPECT_EQ(Translator.TuneParams8()[31], 531);
}

TEST(LegacyCompat, OutboundTeeInfoBecomesThreeObjects)
{
	// TeeInfo is 0.8-only. The 0.7 client needs PlayerInfo + PlayerInfoRace +
	// De_ClientInfo in its place, and the identity must survive the round trip.
	legacy::CNetworkTranslator Translator;

	// Drive a real snapshot through the translator so TeeInfo is absorbed.
	CSnapshotBuilder Builder;
	Builder.Init();
	CNetObj_TeeInfo *pTeeInfo = (CNetObj_TeeInfo *) Builder.NewItem(NETOBJTYPE_TEEINFO, 0, sizeof(CNetObj_TeeInfo));
	ASSERT_NE(pTeeInfo, nullptr);
	mem_zero(pTeeInfo, sizeof(*pTeeInfo));
	pTeeInfo->m_Flag = TEEFLAG_ADMIN | TEEFLAG_READY | TEEFLAG_LOCAL;
	pTeeInfo->m_Team = 0;
	pTeeInfo->m_Score = 7;
	pTeeInfo->m_LatencyAndCountry = (37 << 16) | 276;
	pTeeInfo->m_RaceStartTick = 1234;
	str_copy_fixed(pTeeInfo->m_aName, "bob", sizeof(pTeeInfo->m_aName));
	str_copy_fixed(pTeeInfo->m_aClan, "clan", sizeof(pTeeInfo->m_aClan));
	for(int p = 0; p < 6; p++)
	{
		str_copy_fixed(pTeeInfo->m_aaSkinPartNames[p], "body", sizeof(pTeeInfo->m_aaSkinPartNames[p]));
		pTeeInfo->m_aUseCustomColors[p] = 1;
		pTeeInfo->m_aSkinPartColors[p] = p;
	}
	array<unsigned char> aSnap;
	aSnap.set_size(Builder.RequiredSize());
	ASSERT_GT(Builder.Finish(aSnap.base_ptr()), 0);

	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
	EXPECT_GT(SendSnapshot8AsServer(Translator, Builder, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0);

	const legacy::CNetworkTranslator::CTeeState8 *pState = Translator.TeeState8(0);
	ASSERT_NE(pState, nullptr);
	EXPECT_TRUE(pState->m_Active);
	EXPECT_STREQ(pState->m_aName, "bob");
	EXPECT_STREQ(pState->m_aClan, "clan");
	EXPECT_EQ(pState->m_Score, 7);
	EXPECT_EQ(pState->m_RaceStartTick, 1234);
	// LatencyAndCountry is split back into its two halves.
	EXPECT_EQ(pState->m_Latency, 37);
	EXPECT_EQ(pState->m_Country, 276);
	// The 0.7-facing flag word keeps only the bits 0.7 knows.
	EXPECT_EQ(pState->m_Flag & ~(TEEFLAG_HIDDEN_IN_BOARD | TEEFLAG_LOCAL), TEEFLAG_ADMIN | TEEFLAG_READY);
}

TEST(LegacyCompat, OutboundClientEnterExpandsToClientInfo)
{
	// 0.8 announces a join with a bare Sv_ClientEnter; 0.7 needs the full
	// Sv_ClientInfo carrying the identity.
	legacy::CNetworkTranslator Translator;

	// Seed identity through a snapshot first, exactly as the server would.
	CSnapshotBuilder Builder;
	Builder.Init();
	CNetObj_TeeInfo *pTeeInfo = (CNetObj_TeeInfo *) Builder.NewItem(NETOBJTYPE_TEEINFO, 3, sizeof(CNetObj_TeeInfo));
	ASSERT_NE(pTeeInfo, nullptr);
	mem_zero(pTeeInfo, sizeof(*pTeeInfo));
	pTeeInfo->m_Team = 0;
	pTeeInfo->m_LatencyAndCountry = (12 << 16) | 99;
	str_copy_fixed(pTeeInfo->m_aName, "zed", sizeof(pTeeInfo->m_aName));
	for(int p = 0; p < 6; p++)
		str_copy_fixed(pTeeInfo->m_aaSkinPartNames[p], "body", sizeof(pTeeInfo->m_aaSkinPartNames[p]));
	array<unsigned char> aSnap;
	aSnap.set_size(Builder.RequiredSize());
	ASSERT_GT(Builder.Finish(aSnap.base_ptr()), 0);

	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
	EXPECT_GT(SendSnapshot8AsServer(Translator, Builder, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0);
	ASSERT_TRUE(Translator.TeeState8(3)->m_Active);

	// Now the enter message.
	CNetMsg_Sv_ClientEnter Enter;
	Enter.m_ClientID = 3;
	CMsgPacker Packer(NETMSGTYPE_SV_CLIENTENTER);
	Enter.Pack(&Packer);

	const int NumOut = Translator.TranslateServerToClientChunk(Packer.Data(), Packer.Size(), NETSENDFLAG_VITAL, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS);
	ASSERT_EQ(NumOut, 1);

	// The emitted message must be the 0.7 Sv_ClientInfo carrying our identity.
	CMsgUnpacker Unpacker(aOut[0].m_pData, aOut[0].m_DataSize);
	ASSERT_FALSE(Unpacker.Error());
	EXPECT_EQ(Unpacker.Type(), protocol7::NETMSGTYPE_SV_CLIENTINFO);
	const protocol7::CNetMsg_Sv_ClientInfo *pInfo = (const protocol7::CNetMsg_Sv_ClientInfo *) protocol7::CNetObjHandler().SecureUnpackMsg(Unpacker.Type(), &Unpacker);
	ASSERT_NE(pInfo, nullptr);
	EXPECT_EQ(pInfo->m_ClientID, 3);
	EXPECT_STREQ(pInfo->m_pName, "zed");
	EXPECT_EQ(pInfo->m_Country, 99);
	EXPECT_EQ(pInfo->m_Team, 0);
}

TEST(LegacyCompat, OutboundClientEnterWithoutIdentityIsDropped)
{
	// If the TeeInfo has not been seen yet there is no identity to send, so the
	// enter must be dropped rather than emitted with an empty name.
	legacy::CNetworkTranslator Translator;
	CNetMsg_Sv_ClientEnter Enter;
	Enter.m_ClientID = 5;
	CMsgPacker Packer(NETMSGTYPE_SV_CLIENTENTER);
	Enter.Pack(&Packer);

	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
	EXPECT_EQ(Translator.TranslateServerToClientChunk(Packer.Data(), Packer.Size(), NETSENDFLAG_VITAL, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0);
}

TEST(LegacyCompat, OutboundGameInfoAndChatSurvive)
{
	legacy::CNetworkTranslator Translator;
	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];

	// Sv_GameInfo exists in both protocols with identical fields.
	CNetMsg_Sv_GameInfo GameInfo;
	GameInfo.m_GameFlags = 7;
	GameInfo.m_ScoreLimit = 11;
	GameInfo.m_TimeLimit = 13;
	GameInfo.m_MatchNum = 2;
	GameInfo.m_MatchCurrent = 1;
	CMsgPacker Packer(NETMSGTYPE_SV_GAMEINFO);
	GameInfo.Pack(&Packer);

	ASSERT_EQ(Translator.TranslateServerToClientChunk(Packer.Data(), Packer.Size(), NETSENDFLAG_VITAL, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 1);
	CMsgUnpacker Unpacker(aOut[0].m_pData, aOut[0].m_DataSize);
	ASSERT_FALSE(Unpacker.Error());
	EXPECT_EQ(Unpacker.Type(), protocol7::NETMSGTYPE_SV_GAMEINFO);

	// Sv_Chat likewise.
	CNetMsg_Sv_Chat Chat;
	Chat.m_Mode = 1;
	Chat.m_ClientID = 2;
	Chat.m_TargetID = -1;
	Chat.m_pMessage = "hello";
	CMsgPacker ChatPacker(NETMSGTYPE_SV_CHAT);
	Chat.Pack(&ChatPacker);
	ASSERT_EQ(Translator.TranslateServerToClientChunk(ChatPacker.Data(), ChatPacker.Size(), NETSENDFLAG_VITAL, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 1);
	CMsgUnpacker ChatUnpacker(aOut[0].m_pData, aOut[0].m_DataSize);
	ASSERT_FALSE(ChatUnpacker.Error());
	EXPECT_EQ(ChatUnpacker.Type(), protocol7::NETMSGTYPE_SV_CHAT);
}

TEST(LegacyCompat, OutboundSystemMessagesPassThrough)
{
	// NETMSG_READY / NETMSG_ENTERGAME share their ids between the protocols, so
	// they must reach the 0.7 client untouched.
	legacy::CNetworkTranslator Translator;
	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];

	CMsgPacker Packer(NETMSG_ENTERGAME, true);
	ASSERT_EQ(Translator.TranslateServerToClientChunk(Packer.Data(), Packer.Size(), NETSENDFLAG_VITAL, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 1);
	EXPECT_EQ(aOut[0].m_DataSize, Packer.Size());
	EXPECT_EQ(mem_comp(aOut[0].m_pData, Packer.Data(), Packer.Size()), 0);
	// The send flags must survive so delivery semantics are preserved.
	EXPECT_EQ(aOut[0].m_Flags, (int) NETSENDFLAG_VITAL);
}

// Decodes the translator's output with 0.7 machinery only and checks the tee
// identity arrives in the objects 0.7 expects.
TEST(LegacyCompat, OutboundChunksDecodeAsValid07Snapshots)
{
	legacy::CNetworkTranslator Translator;

	CSnapshotBuilder Builder;
	Builder.Init();
	CNetObj_TeeInfo *pTeeInfo = (CNetObj_TeeInfo *) Builder.NewItem(NETOBJTYPE_TEEINFO, 0, sizeof(CNetObj_TeeInfo));
	ASSERT_NE(pTeeInfo, nullptr);
	mem_zero(pTeeInfo, sizeof(*pTeeInfo));
	pTeeInfo->m_Flag = TEEFLAG_ADMIN | TEEFLAG_READY;
	pTeeInfo->m_Team = 0;
	pTeeInfo->m_Score = 5;
	pTeeInfo->m_LatencyAndCountry = (21 << 16) | 44;
	pTeeInfo->m_RaceStartTick = -1;
	str_copy_fixed(pTeeInfo->m_aName, "neo", sizeof(pTeeInfo->m_aName));
	str_copy_fixed(pTeeInfo->m_aClan, "c", sizeof(pTeeInfo->m_aClan));
	for(int p = 0; p < 6; p++)
		str_copy_fixed(pTeeInfo->m_aaSkinPartNames[p], "body", sizeof(pTeeInfo->m_aaSkinPartNames[p]));

	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
	ASSERT_GT(SendSnapshot8AsServer(Translator, Builder, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0);

	// Decode the emitted chunk with the 0.7 protocol only.
	CMsgUnpacker Unpacker(aOut[0].m_pData, aOut[0].m_DataSize);
	ASSERT_FALSE(Unpacker.Error());
	EXPECT_EQ(Unpacker.Type(), NETMSG_SNAPSINGLE);
	EXPECT_EQ(Unpacker.GetInt(), 1); // tick
	EXPECT_EQ(Unpacker.GetInt(), 2); // delta distance (full snapshot)
	Unpacker.GetInt(); // crc
	const int PartSize = Unpacker.GetInt();
	ASSERT_GT(PartSize, 0);
	const unsigned char *pPart = Unpacker.GetRaw(PartSize);
	ASSERT_NE(pPart, nullptr);

	// Decompress with the 0.7 int codec.
	array<unsigned char> aDelta;
	aDelta.set_size(PartSize * 8 + 4096);
	const int DeltaSize = CVariableInt::Decompress(pPart, PartSize, aDelta.base_ptr(), aDelta.size());
	ASSERT_GT(DeltaSize, 0);

	// Apply it against an empty 0.7 baseline.
	CSnapshotDelta Delta7;
	legacy::Net7ConfigureSnapshotDelta(&Delta7);
	CSnapshot Empty7;
	Empty7.Clear();
	array<unsigned char> aFull;
	aFull.set_size(DeltaSize * 8 + 65536);
	const int FullSize = Delta7.UnpackDelta(&Empty7, (CSnapshot *) aFull.base_ptr(), aDelta.base_ptr(), DeltaSize);
	ASSERT_GT(FullSize, 0);

	// The 0.7 snapshot must carry the three identity objects, not a TeeInfo.
	const CSnapshot *pSnap = (const CSnapshot *) aFull.base_ptr();
	bool HasPlayerInfo = false, HasRace = false, HasClientInfo = false, HasTeeInfo = false;
	for(int i = 0; i < pSnap->NumItems(); i++)
	{
		const int Type = pSnap->GetItem(i)->Type();
		if(Type == protocol7::NETOBJTYPE_PLAYERINFO)
			HasPlayerInfo = true;
		else if(Type == protocol7::NETOBJTYPE_PLAYERINFORACE)
			HasRace = true;
		else if(Type == protocol7::NETOBJTYPE_DE_CLIENTINFO)
			HasClientInfo = true;
		else if(Type == NETOBJTYPE_TEEINFO)
			HasTeeInfo = true;
	}
	EXPECT_TRUE(HasPlayerInfo) << "0.7 client needs PlayerInfo";
	EXPECT_TRUE(HasRace) << "0.7 client needs PlayerInfoRace";
	EXPECT_TRUE(HasClientInfo) << "0.7 client needs De_ClientInfo";
	EXPECT_FALSE(HasTeeInfo) << "TeeInfo is 0.8-only and must not reach a 0.7 client";

	// Spot-check the identity actually survived the rebuild.
	protocol7::CNetObj_PlayerInfo Info;
	mem_zero(&Info, sizeof(Info));
	bool Found = false;
	for(int i = 0; i < pSnap->NumItems(); i++)
	{
		if(pSnap->GetItem(i)->Type() == protocol7::NETOBJTYPE_PLAYERINFO && pSnap->GetItem(i)->ID() == 0)
		{
			mem_copy(&Info, pSnap->GetItem(i)->Data(), sizeof(Info));
			Found = true;
		}
	}
	ASSERT_TRUE(Found);
	EXPECT_EQ(Info.m_Score, 5);
	EXPECT_EQ(Info.m_Latency, 21);
	// ADMIN (0x1) and READY (0x8) keep their bit positions in 0.7.
	EXPECT_EQ(Info.m_PlayerFlags, protocol7::PLAYERFLAG_ADMIN | protocol7::PLAYERFLAG_READY);
}

// 0.7 CL_* ids differ from the 0.8 ones, so this pins the framing of a few
// representative client messages in both directions.
TEST(LegacyCompat, InboundClientMessagesAreLiftedFrom07)
{
	// CL_STARTINFO: 27 in 0.7 but 33 in 0.8, so the emitted 0.8 id must differ
	// from the received 0.7 one.
	{
		protocol7::CNetMsg_Cl_StartInfo SI;
		mem_zero(&SI, sizeof(SI));
		SI.m_pName = "neo";
		SI.m_pClan = "c";
		SI.m_Country = 44;
		for(int p = 0; p < 6; p++)
			SI.m_apSkinPartNames[p] = "body";
		CMsgPacker Packer7(protocol7::NETMSGTYPE_CL_STARTINFO);
		SI.Pack(&Packer7);

		CMsgUnpacker In(Packer7.Data(), Packer7.Size());
		ASSERT_FALSE(In.Error());
		EXPECT_EQ(In.Type(), protocol7::NETMSGTYPE_CL_STARTINFO);
		EXPECT_NE(protocol7::NETMSGTYPE_CL_STARTINFO, NETMSGTYPE_CL_STARTINFO)
			<< "the two protocols number this message differently; the test is pointless otherwise";

		CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
		legacy::CNetworkTranslator Translator;
		ASSERT_EQ(Translator.TranslateServerChunk(Packer7.Data(), Packer7.Size(), aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 1);

		CMsgUnpacker Out(aOut[0].m_pData, aOut[0].m_DataSize);
		ASSERT_FALSE(Out.Error());
		EXPECT_EQ(Out.Type(), NETMSGTYPE_CL_STARTINFO) << "must be re-framed with the 0.8 id";
		void *pRaw = CNetObjHandler().SecureUnpackMsg(Out.Type(), &Out);
		ASSERT_NE(pRaw, nullptr);
		const CNetMsg_Cl_StartInfo *pInfo = (const CNetMsg_Cl_StartInfo *) pRaw;
		EXPECT_STREQ(pInfo->m_pName, "neo");
		EXPECT_EQ(pInfo->m_Country, 44);
	}

	// CL_SAY shares its id, but must still survive the trip with its payload.
	{
		protocol7::CNetMsg_Cl_Say Say;
		Say.m_Mode = 0;
		Say.m_Target = -1;
		Say.m_pMessage = "hello";
		CMsgPacker Packer7(protocol7::NETMSGTYPE_CL_SAY);
		Say.Pack(&Packer7);

		CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
		legacy::CNetworkTranslator Translator;
		ASSERT_EQ(Translator.TranslateServerChunk(Packer7.Data(), Packer7.Size(), aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 1);
		CMsgUnpacker Out(aOut[0].m_pData, aOut[0].m_DataSize);
		ASSERT_FALSE(Out.Error());
		EXPECT_EQ(Out.Type(), NETMSGTYPE_CL_SAY);
		const CNetMsg_Cl_Say *pSay = (const CNetMsg_Cl_Say *) CNetObjHandler().SecureUnpackMsg(Out.Type(), &Out);
		ASSERT_NE(pSay, nullptr);
		EXPECT_STREQ(pSay->m_pMessage, "hello");
	}

	// CL_CALLVOTE has four fields, and every one has to be copied over. Dropping
	// m_Force silently turns a forced vote into a normal one and ships whatever
	// the uninitialised local held.
	{
		protocol7::CNetMsg_Cl_CallVote Vote;
		Vote.m_Type = "kick";
		Vote.m_Value = "3";
		Vote.m_Reason = "idle";
		Vote.m_Force = 1; // non-zero, so a dropped field cannot alias it
		CMsgPacker Packer7(protocol7::NETMSGTYPE_CL_CALLVOTE);
		Vote.Pack(&Packer7);

		CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
		legacy::CNetworkTranslator Translator;
		ASSERT_EQ(Translator.TranslateServerChunk(Packer7.Data(), Packer7.Size(), aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 1);
		CMsgUnpacker Out(aOut[0].m_pData, aOut[0].m_DataSize);
		ASSERT_FALSE(Out.Error());
		EXPECT_EQ(Out.Type(), NETMSGTYPE_CL_CALLVOTE);
		const CNetMsg_Cl_CallVote *pVote = (const CNetMsg_Cl_CallVote *) CNetObjHandler().SecureUnpackMsg(Out.Type(), &Out);
		ASSERT_NE(pVote, nullptr);
		EXPECT_STREQ(pVote->m_Type, "kick");
		EXPECT_STREQ(pVote->m_Value, "3");
		EXPECT_STREQ(pVote->m_Reason, "idle");
		EXPECT_EQ(pVote->m_Force, 1);
	}

	// CL_SETTEAM: 25 in 0.7, 26 in 0.8.
	{
		protocol7::CNetMsg_Cl_SetTeam Team;
		Team.m_Team = 0;
		CMsgPacker Packer7(protocol7::NETMSGTYPE_CL_SETTEAM);
		Team.Pack(&Packer7);

		CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
		legacy::CNetworkTranslator Translator;
		ASSERT_EQ(Translator.TranslateServerChunk(Packer7.Data(), Packer7.Size(), aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 1);
		CMsgUnpacker Out(aOut[0].m_pData, aOut[0].m_DataSize);
		ASSERT_FALSE(Out.Error());
		EXPECT_EQ(Out.Type(), NETMSGTYPE_CL_SETTEAM) << "0.7 id 25 must become 0.8 id 26";
	}
}

// NETMSG_INFO carries the client's netversion, which the server compares against
// its own. A 0.7 client legitimately reports the 0.7 version, so the translator
// must rewrite it or every legacy peer is rejected as a version mismatch.
TEST(LegacyCompat, InboundInfoRewritesTheVersionString)
{
	CMsgPacker Packer7(NETMSG_INFO, true);
	Packer7.AddString(LEGACY_NET7_NETVERSION, 0);
	Packer7.AddString("", 0);
	Packer7.AddInt(0x0706);
	Packer7.AddInt(1);

	legacy::CNetworkTranslator Translator;
	Translator.SetNetVersion("0.8 deadbeefdeadbeef");

	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
	ASSERT_EQ(Translator.TranslateServerChunk(Packer7.Data(), Packer7.Size(), aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 1);

	CMsgUnpacker Out(aOut[0].m_pData, aOut[0].m_DataSize);
	ASSERT_FALSE(Out.Error());
	EXPECT_EQ(Out.Type(), NETMSG_INFO);
	EXPECT_TRUE(Out.System());
	// The server's own version replaces the 0.7 one; the rest is preserved.
	EXPECT_STREQ(Out.GetString(), "0.8 deadbeefdeadbeef");
	EXPECT_STREQ(Out.GetString(), "");
	EXPECT_EQ(Out.GetInt(), 0x0706);
	EXPECT_EQ(Out.GetInt(), 1);
	EXPECT_FALSE(Out.Error());
}

// NetChar fields carry no terminator and may fill their whole width, so a
// length-based copy would read into neighbouring data. The field is filled
// completely here to prove the copy is width-bounded.
TEST(LegacyCompat, OutboundFullWidthNameIsBoundedByTheField)
{
	legacy::CNetworkTranslator Translator;

	// The wire field is MAX_NAME_ARRAY_SIZE - 1 = 64 bytes and is allowed to be
	// completely full; a length-based copy would run off the end of the packet.
	char aFullName[sizeof(((CNetObj_TeeInfo *) 0)->m_aName) + 1];
	for(int i = 0; i < (int) sizeof(aFullName) - 1; i++)
		aFullName[i] = 'a' + (i % 26);
	aFullName[sizeof(aFullName) - 1] = 0;

	CSnapshotBuilder Builder;
	Builder.Init();
	CNetObj_TeeInfo *pTeeInfo = (CNetObj_TeeInfo *) Builder.NewItem(NETOBJTYPE_TEEINFO, 0, sizeof(CNetObj_TeeInfo));
	ASSERT_NE(pTeeInfo, nullptr);
	mem_zero(pTeeInfo, sizeof(*pTeeInfo));
	pTeeInfo->m_Team = 0;
	// no terminator anywhere in the field
	mem_copy(pTeeInfo->m_aName, aFullName, sizeof(pTeeInfo->m_aName));
	// Poison everything that follows the name field. A length-based copy walks
	// straight into these bytes; a width-bounded one cannot see them. In a real
	// snapshot this region is the next item, so the garbage would be real data.
	for(int i = 0; i < (int) sizeof(pTeeInfo->m_aClan); i++)
		pTeeInfo->m_aClan[i] = 'Z';
	for(int p = 0; p < 6; p++)
		for(int i = 0; i < (int) sizeof(pTeeInfo->m_aaSkinPartNames[p]); i++)
			pTeeInfo->m_aaSkinPartNames[p][i] = 'Q';

	array<unsigned char> aSnap;
	aSnap.set_size(Builder.RequiredSize());
	ASSERT_GT(Builder.Finish(aSnap.base_ptr()), 0);

	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
	ASSERT_GT(SendSnapshot8AsServer(Translator, Builder, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0);

	const legacy::CNetworkTranslator::CTeeState8 *pState = Translator.TeeState8(0);
	ASSERT_NE(pState, nullptr);
	// The name is exactly the field content, not over-read garbage.
	EXPECT_STREQ(pState->m_aName, aFullName);
	EXPECT_EQ(str_length(pState->m_aName), (int) sizeof(aFullName) - 1);
}

// Pins the 0.7 int-packed encoding of the emitted wire bytes against the
// canonical StrToInts/IntsToStr in gamecore.h. The final integer's low byte must
// be cleared, exactly as a real 0.7 server writes it; skipping that step makes
// every name whose length is a multiple of four decode one byte too long.
TEST(LegacyCompat, OutboundIdentityUsesCanonical07IntPacking)
{
	// A name whose length is a multiple of 4 is where the terminator step shows
	// up: without it the 0.7 client sees one extra character.
	const char *pName = "abcdefgh";
	ASSERT_EQ(str_length(pName) % 4, 0);

	legacy::CNetworkTranslator Translator;

	CSnapshotBuilder Builder;
	Builder.Init();
	CNetObj_TeeInfo *pTeeInfo = (CNetObj_TeeInfo *) Builder.NewItem(NETOBJTYPE_TEEINFO, 0, sizeof(CNetObj_TeeInfo));
	ASSERT_NE(pTeeInfo, nullptr);
	mem_zero(pTeeInfo, sizeof(*pTeeInfo));
	mem_copy(pTeeInfo->m_aName, pName, str_length(pName));

	array<unsigned char> aSnap;
	aSnap.set_size(Builder.RequiredSize());
	ASSERT_GT(Builder.Finish(aSnap.base_ptr()), 0);

	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
	ASSERT_GT(SendSnapshot8AsServer(Translator, Builder, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0);

	// Decode the emitted packet with 0.7 machinery only.
	CMsgUnpacker Unpacker(aOut[0].m_pData, aOut[0].m_DataSize);
	ASSERT_FALSE(Unpacker.Error());
	ASSERT_EQ(Unpacker.Type(), NETMSG_SNAPSINGLE);
	Unpacker.GetInt(); // tick
	Unpacker.GetInt(); // delta distance
	Unpacker.GetInt(); // crc
	const int PartSize = Unpacker.GetInt();
	ASSERT_GT(PartSize, 0);
	const unsigned char *pPart = Unpacker.GetRaw(PartSize);
	ASSERT_NE(pPart, nullptr);

	array<unsigned char> aDelta;
	aDelta.set_size(PartSize * 8 + 4096);
	const int DeltaSize = CVariableInt::Decompress(pPart, PartSize, aDelta.base_ptr(), aDelta.size());
	ASSERT_GT(DeltaSize, 0);

	CSnapshotDelta Delta7;
	legacy::Net7ConfigureSnapshotDelta(&Delta7);
	CSnapshot Empty7;
	Empty7.Clear();
	array<unsigned char> aFull;
	aFull.set_size(DeltaSize * 8 + 65536);
	ASSERT_GT(Delta7.UnpackDelta(&Empty7, (CSnapshot *) aFull.base_ptr(), aDelta.base_ptr(), DeltaSize), 0);

	const CSnapshot *pSnap = (const CSnapshot *) aFull.base_ptr();
	const protocol7::CNetObj_De_ClientInfo *pClientInfo = nullptr;
	for(int i = 0; i < pSnap->NumItems(); i++)
	{
		if(pSnap->GetItem(i)->Type() == protocol7::NETOBJTYPE_DE_CLIENTINFO && pSnap->GetItem(i)->ID() == 0)
			pClientInfo = (const protocol7::CNetObj_De_ClientInfo *) pSnap->GetItem(i)->Data();
	}
	ASSERT_NE(pClientInfo, nullptr);

	// The canonical encoder, verbatim from gamecore.h.
	int aExpected[4] = {0, 0, 0, 0};
	{
		int Index = 0;
		for(int n = 0; n < 4; n++)
		{
			char aBuf[4] = {0, 0, 0, 0};
			for(int c = 0; c < 4 && pName[Index]; c++, Index++)
				aBuf[c] = pName[Index];
			aExpected[n] = ((aBuf[0] + 128) << 24) | ((aBuf[1] + 128) << 16) | ((aBuf[2] + 128) << 8) | (aBuf[3] + 128);
		}
		aExpected[3] &= 0xffffff00; // null terminate, the step that matters here
	}
	EXPECT_EQ(aExpected[3] & 0xff, 0);

	// The bytes the 0.7 client actually receives must match the canonical form.
	for(int n = 0; n < 4; n++)
		EXPECT_EQ(pClientInfo->m_aName[n], aExpected[n]) << "word " << n;

	// And decoding them exactly as a 0.7 client's IntsToStr does yields the name.
	char aDecoded[17];
	for(int n = 0; n < 4; n++)
	{
		aDecoded[n * 4 + 0] = ((pClientInfo->m_aName[n] >> 24) & 0xff) - 128;
		aDecoded[n * 4 + 1] = ((pClientInfo->m_aName[n] >> 16) & 0xff) - 128;
		aDecoded[n * 4 + 2] = ((pClientInfo->m_aName[n] >> 8) & 0xff) - 128;
		aDecoded[n * 4 + 3] = (pClientInfo->m_aName[n] & 0xff) - 128;
	}
	aDecoded[16] = 0; // IntsToStr null terminates the last byte
	EXPECT_STREQ(aDecoded, pName);
}

// 0.8 inserted m_MaxHealth/m_MaxArmor *between* m_Armor and m_AmmoCount, so the
// two Character structs share a prefix but not a tail. A prefix mem_copy shifts
// every later field by two and the 0.7 client reads ammo as health, weapon as
// armor, and so on -- it also reports the item as the wrong size. Give every
// field a distinct value so any shift is visible.
TEST(LegacyCompat, OutboundCharacterFieldsAreNotShifted)
{
	legacy::CNetworkTranslator Translator;

	CSnapshotBuilder Builder;
	Builder.Init();
	CNetObj_Character *pChar = (CNetObj_Character *) Builder.NewItem(NETOBJTYPE_CHARACTER, 0, sizeof(CNetObj_Character));
	ASSERT_NE(pChar, nullptr);
	mem_zero(pChar, sizeof(*pChar));

	// Core fields, distinct and recognisable.
	pChar->m_Tick = 11;
	pChar->m_X = 22;
	pChar->m_Y = 33;
	pChar->m_VelX = 44;
	pChar->m_VelY = 55;
	pChar->m_Angle = 66;
	pChar->m_Direction = 1;
	pChar->m_Jumped = 2;
	pChar->m_HookedPlayer = -1;
	pChar->m_HookState = 3;
	pChar->m_HookTick = 77;
	pChar->m_HookX = 88;
	pChar->m_HookY = 99;
	pChar->m_HookDx = 111;
	pChar->m_HookDy = 222;

	// The tail is where the 0.8-only fields are interleaved. These values are
	// deliberately far apart so a two-slot shift cannot alias by accident.
	pChar->m_Health = 1001;
	pChar->m_Armor = 1002;
	pChar->m_MaxHealth = 9999; // 0.8-only, must never surface in 0.7
	pChar->m_MaxArmor = 8888;  // 0.8-only, must never surface in 0.7
	pChar->m_AmmoCount = 1003;
	pChar->m_Weapon = 4;
	pChar->m_Emote = 5;
	pChar->m_AttackTick = 1006;
	pChar->m_TriggeredEvents = 1007;

	array<unsigned char> aSnap;
	aSnap.set_size(Builder.RequiredSize());
	ASSERT_GT(Builder.Finish(aSnap.base_ptr()), 0);

	CNetChunk aOut[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];
	ASSERT_GT(SendSnapshot8AsServer(Translator, Builder, aOut, legacy::CNetworkTranslator::MAX_OUT_CHUNKS), 0);

	// Decode with 0.7 machinery only, as the client would.
	CMsgUnpacker Unpacker(aOut[0].m_pData, aOut[0].m_DataSize);
	ASSERT_FALSE(Unpacker.Error());
	ASSERT_EQ(Unpacker.Type(), NETMSG_SNAPSINGLE);
	Unpacker.GetInt(); // tick
	Unpacker.GetInt(); // delta distance
	Unpacker.GetInt(); // crc
	const int PartSize = Unpacker.GetInt();
	ASSERT_GT(PartSize, 0);
	const unsigned char *pPart = Unpacker.GetRaw(PartSize);
	ASSERT_NE(pPart, nullptr);

	array<unsigned char> aDelta;
	aDelta.set_size(PartSize * 8 + 4096);
	const int DeltaSize = CVariableInt::Decompress(pPart, PartSize, aDelta.base_ptr(), aDelta.size());
	ASSERT_GT(DeltaSize, 0);

	CSnapshotDelta Delta7;
	legacy::Net7ConfigureSnapshotDelta(&Delta7);
	CSnapshot Empty7;
	Empty7.Clear();
	array<unsigned char> aFull;
	aFull.set_size(DeltaSize * 8 + 65536);
	ASSERT_GT(Delta7.UnpackDelta(&Empty7, (CSnapshot *) aFull.base_ptr(), aDelta.base_ptr(), DeltaSize), 0);

	const CSnapshot *pSnap = (const CSnapshot *) aFull.base_ptr();
	const protocol7::CNetObj_Character *pOut = nullptr;
	int OutSize = 0;
	for(int i = 0; i < pSnap->NumItems(); i++)
	{
		if(pSnap->GetItem(i)->Type() == protocol7::NETOBJTYPE_CHARACTER && pSnap->GetItem(i)->ID() == 0)
		{
			pOut = (const protocol7::CNetObj_Character *) pSnap->GetItem(i)->Data();
			OutSize = pSnap->GetItemSize(i);
		}
	}
	ASSERT_NE(pOut, nullptr);

	// The whole point of the reported bug: the 0.7 item must be the 0.7 size.
	EXPECT_EQ(OutSize, (int) sizeof(protocol7::CNetObj_Character));
	EXPECT_EQ((int) sizeof(protocol7::CNetObj_Character), 22 * 4);

	// Core survives verbatim.
	EXPECT_EQ(pOut->m_Tick, 11);
	EXPECT_EQ(pOut->m_X, 22);
	EXPECT_EQ(pOut->m_Y, 33);
	EXPECT_EQ(pOut->m_HookDy, 222);

	// The tail must land on the 0.7 field names, not shifted by two.
	EXPECT_EQ(pOut->m_Health, 1001);
	EXPECT_EQ(pOut->m_Armor, 1002);
	EXPECT_EQ(pOut->m_AmmoCount, 1003) << "a prefix copy would put MaxHealth here";
	EXPECT_EQ(pOut->m_Weapon, 4) << "a prefix copy would put MaxArmor here";
	EXPECT_EQ(pOut->m_Emote, 5);
	EXPECT_EQ(pOut->m_AttackTick, 1006);
	EXPECT_EQ(pOut->m_TriggeredEvents, 1007);

	// The 0.8-only values must not appear anywhere in the emitted item.
	const int *pInts = (const int *) pOut;
	for(int i = 0; i < OutSize / 4; i++)
	{
		EXPECT_NE(pInts[i], 9999) << "MaxHealth leaked into the 0.7 item at word " << i;
		EXPECT_NE(pInts[i], 8888) << "MaxArmor leaked into the 0.7 item at word " << i;
	}

	// The values a real server actually sends: MAX_HEALTH/MAX_ARMOR are both 10.
	// A prefix copy puts 10 into m_Weapon, which is not a valid weapon id, so
	// the 0.7 client's own validation rejects and invalidates the item -- the
	// exact "invalidated index=.. type=10 (Character)" log this guards against.
	EXPECT_NE(pOut->m_Weapon, 10) << "a prefix copy would put MaxArmor's 10 here";
	EXPECT_TRUE(pOut->m_Weapon >= -1 && pOut->m_Weapon < 6) << "m_Weapon must stay a valid weapon id";
}
