/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_SHARED_LEGACY_NETWORK_TRANSLATOR_H
#define ENGINE_SHARED_LEGACY_NETWORK_TRANSLATOR_H

#include <base/tl/array.h>
#include <base/uuid.h>

#include <engine/shared/network.h>
#include <engine/shared/protocol.h>
#include <engine/shared/snapshot.h>

#include "network7.h"

class CPacker;
class CUnpacker;

/*
	Translates a 0.7 peer's game traffic into the 0.8 world and back.

	The translator owns the 0.7 snapshot history (for decoding incoming deltas)
	and the 0.8 baseline (for encoding outgoing deltas). It never touches the
	frozen 0.7 handshake.

	Chunk output points into translator-owned storage that stays valid until the
	next call to the same method.
*/
namespace legacy
{
	class CNetworkTranslator
	{
	public:
		enum
		{
			// A 0.7 server never has more than this many clients.
			MAX_CLIENTS7 = 64,
			// The outbound identity cache is keyed by 0.7 client id, so it is
			// bounded by MAX_CLIENTS7 rather than the much larger 0.8 tee space.
			MAX_TEES8 = MAX_CLIENTS7,
			// CTuningParams has this many parameters; the 0.8 Tuning object uses
			// the same count (guarded by a static_assert in gamecore.cpp).
			NUM_TUNES = 32,
			// Upper bound on chunks a single input chunk can translate into.
			MAX_OUT_CHUNKS = 256,
		};

		// Identity of one 0.7 client, captured from Sv_ClientInfo. Names and
		// skins are stored NUL-terminated, one byte wider than the 0.8 fields.
		class CClientState7
		{
		public:
			CClientState7() { Reset(); }
			void Reset();

			bool m_Active;
			int m_Local;
			int m_Team;
			int m_Country;
			char m_aName[MAX_NAME_ARRAY_SIZE - 1];
			char m_aClan[MAX_CLAN_ARRAY_SIZE - 1];
			char m_aaSkinPartNames[6][MAX_SKIN_ARRAY_SIZE - 1];
			int m_aUseCustomColors[6];
			int m_aSkinPartColors[6];
		};

		// --- outbound (0.8 server -> 0.7 client) state ---
		// 0.8 folds identity into TeeInfo; 0.7 needs PlayerInfo +
		// PlayerInfoRace + De_ClientInfo, so the latest TeeInfo per tee is cached
		// here and re-emitted as those three objects.
		struct CTeeState8
		{
			bool m_Active;
			int m_Flag;
			int m_Score;
			int m_Team;
			int m_Latency;
			int m_Country;
			int m_RaceStartTick;
			char m_aName[MAX_NAME_ARRAY_SIZE];
			char m_aClan[MAX_CLAN_ARRAY_SIZE];
			// NUM_SKINPARTS is 6 in both protocols; the generated protocol table
			// defines it, but this header deliberately does not include it.
			char m_aaSkinPartNames[6][MAX_SKIN_ARRAY_SIZE];
			int m_aUseCustomColors[6];
			int m_aSkinPartColors[6];
		};


	private:
		CClientState7 m_aClients7[MAX_CLIENTS7];

		// Last tuning captured from Sv_TuneParams.
		bool m_TuningValid;
		int m_aTuneParams7[NUM_TUNES];

		CSnapshotDelta m_Delta7; // configured with the 0.7 object sizes
		CSnapshotDelta m_Delta8; // configured with the 0.8 object sizes, for
		                         // decoding the deltas our own server sends

		// Reassembly of multi-part 0.8 snapshots. Outbound only: a 0.7 client
		// cannot send a snapshot.
		array<unsigned char> m_Incoming8Data;
		unsigned char m_aParts8[CSnapshot::MAX_PARTS / 8];
		int m_NumParts8;
		int m_LastPartSize8;

		// Chunk output storage, one buffer per output chunk.
		array<unsigned char> m_aServerOut[MAX_OUT_CHUNKS];
		array<unsigned char> m_aClientOut[MAX_OUT_CHUNKS];

		CTeeState8 m_aTees8[MAX_TEES8];
		// Which tee each 0.8 client slot is; Sv_ClientEnter carries the tee id.
		bool m_aTeeActive8[MAX_TEES8];
		int m_aTuneParams8[NUM_TUNES];
		bool m_TuningValid8;
		// The netversion string a legacy peer is told the server speaks. The
		// engine layer cannot see game/version.h, so the server injects it; a
		// 0.7 client's own version string would otherwise be rejected as a
		// mismatch by the server's version check.
		const char *m_pNetVersion;

		// 0.7 history of the snapshots we emit, so that a later 0.8 delta can be
		// rebased onto a 0.7 baseline the client actually received.
		CSnapshotStorage m_Snapshots8;
		array<unsigned char> m_Snap8CopyData; // stored copy of an emitted 0.8 snapshot
		array<unsigned char> m_Snap7Out; // scratch for the rebuilt 0.7 snapshot
		CSnapshot m_EmptySnap8;
		CSnapshot *m_pBase7;
		array<unsigned char> m_Base7Data;
		int m_BaseTick7;
		int m_CurrentSentTick8;

		// --- internals ---
		void AbsorbClientInfo(int ClientID, int Local, int Team, int Country,
			const char *pName, const char *pClan, const char *const *papSkinPartNames,
			const int *pUseCustomColors, const int *pSkinPartColors);

		// Outbound helpers (0.8 -> 0.7).
		void AbsorbTeeInfo8(int ID, const CSnapshotItem *pItem, int ItemSize);
		int BuildSnapshot7(const CSnapshot *pSnap8);
		void ResetBase7();
		void SetBase7(int GameTick, int Snap7Size);
		void ResetSnapshotParts();
		int TranslateSnapshotDelta8To7(int GameTick, int DeltaTick, const void *pDelta8, int DeltaSize8, void *pOut);
		int EmitSnapshot7(int GameTick, int DeltaField, int DeltaSize7, const void *pDelta7, CNetChunk *pOutChunks, int MaxOutChunks);
		int TranslateCompleteSnapshot8(int GameTick, int DeltaTick, int CompleteSize, CNetChunk *pOutChunks, int MaxOutChunks);
		int HandleSnapshot8(CUnpacker *pUnpacker, int MsgId, CNetChunk *pOutChunks, int MaxOutChunks);
		void StoreSnapshot8Copy(int GameTick, int DeltaTick);

		// Message-level translation. Returns bytes written to pOut, -1 if the
		// message is dropped, -2 on error.
		int TranslateServerMsg(const void *pMsg7, int Size7, void *pOut, int OutSize);
		int TranslateClientMsg(const void *pMsg8, int Size8, void *pOut, int OutSize);
		int TranslateServerToClientMsg(const void *pMsg8, int Size8, void *pOut, int OutSize);

	public:
		CNetworkTranslator();
		~CNetworkTranslator();

		// Reset all per-session state (baselines, 0.7 client table, tune params).
		void Reset();

		// The netversion string reported to legacy peers on NETMSG_INFO. Must be
		// set by the owner before serving traffic; defaults to the 0.7 version so
		// an unconfigured translator still behaves sanely in tests.
		void SetNetVersion(const char *pNetVersion) { m_pNetVersion = pNetVersion ? pNetVersion : LEGACY_NET7_NETVERSION; }

		// --- server/demo -> 0.8 client -------------------------------------
		// Translate one raw 0.7 chunk (message or snapshot payload, as delivered
		// by CNetClient::Recv) into 0.8 chunks.
		//   pInData/pInSize: the 0.7 chunk payload (a packed message starting with
		//                    its msg id, or a snapshot delta payload).
		//   pOutChunks:      caller-provided array of at least MaxOutChunks chunks.
		//   Returns the number of 0.8 chunks written (0 = absorbed/dropped).
		int TranslateServerChunk(const void *pInData, int InSize, CNetChunk *pOutChunks, int MaxOutChunks);

		// --- 0.8 client -> 0.7 server --------------------------------------
		// Translate one 0.8 chunk produced by the client into 0.7 chunks.
		//   Flags: the NETSENDFLAG_* the chunk was queued with; they are
		//          propagated to the output chunks unchanged. Delivery
		//          semantics must survive translation: NETMSG_INPUT etc. are
		//          sent non-vital, and forcing them vital would make the net
		//          layer resend every input reliably.
		//   Same contract as above otherwise.
		int TranslateClientChunk(const void *pInData, int InSize, int Flags, CNetChunk *pOutChunks, int MaxOutChunks);

		// --- 0.8 server -> 0.7 client (our topology) -----------------------
		// Translate one raw 0.8 chunk produced by our server into 0.7 chunks.
		// Handles both packed messages and snapshot deltas.
		//   Flags: the NETSENDFLAG_* the chunk was queued with, propagated to the
		//          output chunks unchanged.
		//   Returns the number of 0.7 chunks written (0 = absorbed/dropped).
		int TranslateServerToClientChunk(const void *pInData, int InSize, int Flags, CNetChunk *pOutChunks, int MaxOutChunks);

		// --- inspection (tests / integration) ------------------------------
		const CClientState7 *ClientState(int ClientID) const;
		bool TuningValid() const { return m_TuningValid; }
		const int *TuneParams() const { return m_aTuneParams7; }
		// The cached 0.8 identity a 0.7 client would be shown for this tee.
		const CTeeState8 *TeeState8(int TeeID) const;
		bool TuningValid8() const { return m_TuningValid8; }
		const int *TuneParams8() const { return m_aTuneParams8; }
	};
} // namespace legacy

#endif // ENGINE_SHARED_LEGACY_NETWORK_TRANSLATOR_H
