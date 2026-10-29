/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_SERVER_SERVER_H
#define ENGINE_SERVER_SERVER_H

#include <base/tl/array.h>
#include <base/tl/hashtable.h>
#include <base/tl/sorted_array.h>
#include <base/tl/string.h>
#include <engine/server.h>
#include <engine/shared/legacy/network_translator.h>
#include <engine/shared/memheap.h>

struct _json_value;
typedef struct _json_value json_value;

class CSnapIDPool
{
	enum
	{
		MAX_IDS = 16 * 1024,
	};

	class CID
	{
	public:
		short m_Next;
		short m_State; // 0 = free, 1 = allocated, 2 = timed
		int m_Timeout;
	};

	CID m_aIDs[MAX_IDS];

	int m_FirstFree;
	int m_FirstTimed;
	int m_LastTimed;
	int m_Usage;
	int m_InUsage;

public:
	CSnapIDPool();

	void Reset();
	void RemoveFirstTimeout();
	int NewID();
	void TimeoutIDs();
	void FreeID(int ID);
	// whether ID is currently allocated (state 1) in this pool
	bool Owns(int ID) const { return ID >= 0 && ID < MAX_IDS && m_aIDs[ID].m_State == 1; }
};

class CServerBan : public CNetBan
{
	class CServer *m_pServer;

	template<class T>
	int BanExt(T *pBanPool, const typename T::CDataType *pData, int Seconds, const char *pReason);

public:
	class CServer *Server() const { return m_pServer; }

	void InitServerBan(class IConsole *pConsole, class IStorage *pStorage, class CServer *pServer);

	virtual int BanAddr(const NETADDR *pAddr, int Seconds, const char *pReason);
	virtual int BanRange(const CNetRange *pRange, int Seconds, const char *pReason);

	static void ConBanExt(class IConsole::IResult *pResult, void *pUser);
};

class CServer : public IServer
{
	class IGameServer *m_pGameServer;
	class CConfig *m_pConfig;
	class IConsole *m_pConsole;
	class IStorage *m_pStorage;

public:
	class IGameServer *GameServer() { return m_pGameServer; }
	class CConfig *Config() { return m_pConfig; }
	class IConsole *Console() { return m_pConsole; }
	class IStorage *Storage() { return m_pStorage; }

	enum
	{
		AUTHED_NO = 0,
		AUTHED_MOD,
		AUTHED_ADMIN,

		MAX_RCONCMD_SEND = 16,
		MAX_MAPLISTENTRY_SEND = 32,
		MIN_MAPLIST_CLIENTVERSION = 0x0703, // todo 0.8: remove me
		MAX_RCONCMD_RATIO = 8,
	};

	struct CMapListEntry;

	class CClient
	{
	public:
		enum
		{
			STATE_EMPTY = 0,
			STATE_AUTH,
			STATE_CONNECTING,
			STATE_CONNECTING_AS_SPEC,
			STATE_READY,
			STATE_INGAME,

			SNAPRATE_INIT = 0,
			SNAPRATE_FULL,
			SNAPRATE_RECOVER
		};

		class CInput
		{
		public:
			int m_aData[MAX_INPUT_SIZE];
			int m_GameTick; // the tick that was chosen for the input
		};

		// connection state info
		int m_State;
		int m_Latency;
		int m_SnapRate;

		int m_LastAckedSnapshot;
		int m_LastInputTick;
		CSnapshotStorage m_Snapshots;

		CInput m_LatestInput;
		CInput m_aInputs[200]; // TODO: handle input better
		int m_CurrentInput;

		char m_aName[MAX_NAME_ARRAY_SIZE];
		char m_aClan[MAX_CLAN_ARRAY_SIZE];
		int m_ServerInfoVersion;
		int m_Version;
		// optional flags from the end of NETMSG_INFO; 0 for older clients
		int m_ClientInfoFlags;
		int m_Country;
		int m_Score;
		int m_Authed;
		int m_AuthTries;

		int m_MapChunk;
		bool m_NoRconNote;
		bool m_Quitting;
		// Set while a silent map switch waits for NETMSG_SILENT_MAP_STATUS and
		// then for the first snapshot of the new world. Another switch while it
		// is set must be regular; the client may still be applying this one.
		bool m_SilentMapSwitch;
		// The client explicitly answered SILENT_MAP_STATUS_LOADED for that
		// switch. Only then may a post-switch snapshot acknowledgement retire
		// the silent switch above.
		bool m_SilentMapLoadConfirmed;
		// Tick of that silent switch. An input acknowledging a snapshot from
		// that tick or later proves the client applied the new world, so a
		// following switch can be silent again; until then it must be regular.
		int m_SilentMapSwitchTick;

		bool SupportsSilentMapStatus() const { return (m_ClientInfoFlags & CLIENT_INFO_FLAG_SILENT_MAP_STATUS) != 0; }
		const IConsole::CCommandInfo *m_pRconCmdToSend;

		unsigned m_MapID;

		void Reset(bool ResetInput = true);
		// Clears only the map transfer/snapshot state, keeping the score and the
		// connection state. Used by the silent map switch.
		void ResetMapTransfer();
	};

	CClient m_aClients[MAX_CLIENTS];

	CSnapshotDelta m_SnapshotDelta;
	CSnapshotBuilder m_SnapshotBuilder;
	// reusable per-tick buffers, grown on demand instead of fixed 64 KiB stack buffers
	array<unsigned char> m_SnapshotBuildData;
	array<unsigned char> m_SnapshotDeltaData;
	array<unsigned char> m_SnapshotCompData;
	array<unsigned char> m_DemoSnapshotData;
	// One snapshot id pool per floor. Ring neighbours of the same floor are
	// snapped into the same client snapshot, so they share a pool; a different
	// floor never shares a snapshot with this one and gets its own pool.
	struct SFloorIDPool
	{
		char m_aFloor[64];
		CSnapIDPool m_Pool;
	};
	// Floor name -> pool. The key is the pool's own m_aFloor buffer, so it
	// stays valid for as long as the entry exists.
	struct CFloorKey
	{
		static unsigned hash(const char *pKey) { return str_quickhash(pKey); }
		static bool equal(const char *pA, const char *pB) { return str_comp(pA, pB) == 0; }
	};
	hash_table<const char *, SFloorIDPool *, 8, CFloorKey> m_pFloorIDPools;
	CSnapIDPool *IDPoolForMap(unsigned MapID);
	CSnapIDPool *IDPoolForFloor(const char *pFloor);
	CNetServer m_NetServer;

	// Compatibility layer for 0.7 peers: the server itself speaks 0.8, so every
	// chunk to/from a legacy client is translated. One translator per client
	// slot, because each owns its own 0.7 snapshot history and 0.8 baseline.
	legacy::CNetworkTranslator m_aLegacyTranslators[MAX_CLIENTS];
	// scratch for translating an outgoing message into 0.7 chunks
	CNetChunk m_aLegacyOutChunks[legacy::CNetworkTranslator::MAX_OUT_CHUNKS];

	CEcon m_Econ;
	CServerBan m_ServerBan;

	IEngineMap *m_pMap;
	IMapChecker *m_pMapChecker;
	class CMapGen *m_pMapGen;

	int64 m_GameStartTime;
	bool m_RunServer;
	bool m_ShuttingDown;
	int m_RconClientID;
	int m_RconAuthLevel;
	int m_PrintCBIndex;
	char m_aShutdownReason[128];

	// map
	struct CMapInfo
	{
		enum
		{
			MAP_CHUNK_SIZE = NET_MAX_PAYLOAD - NET_MAX_CHUNKHEADERSIZE - 4, // msg type
		};

		char m_aName[64];
		SHA256_DIGEST m_Sha256;
		unsigned m_Crc;
		unsigned char *m_pData;
		int m_Size;
		int m_MapID;
		bool m_Loaded;
		// engine/server/mapgen.h EWorldKind of this map, -1 when unknown
		int m_WorldKind;
		// floor name of the map: "Depths" for "Depths_23", the map name for a
		// hand authored map. Snapshot ids are pooled per floor.
		char m_aFloor[64];

		// Ring geometry of a generated instance, read from its world json. When
		// enabled, offset_x is the global column of this instance's local x = 0
		// and period is the circumference of the whole ring, both in tiles.
		bool m_Ring;
		int m_RingOffsetX;
		int m_RingPeriod;

		const char *GetMapName();
		static void Clean(CMapInfo &This, void *pUser);
	};
	hash_table<unsigned, CMapInfo, 8> m_MapInfos;
	// Generated metadata kept in memory after LoadMap. This is the pack's JSON
	// (or the sidecar's) and saves the game layer from needing a sidecar file.
	struct SMapMeta
	{
		unsigned m_MapID;
		char *m_pData;
		int m_Size;
	};
	array<SMapMeta> m_lMapMeta;
	void SetMapMeta(unsigned MapID, const void *pData, int Size);
	void ClearMapMeta(unsigned MapID);
	unsigned m_MainMapID;

	int m_MapChunksPerRequest;

	int m_RconPasswordSet;
	int m_GeneratedRconPassword;

	// CDemoRecorder m_DemoRecorder;
	CRegister m_Register;
	bool m_ServerInfoNeedsUpdate;

	CServer();
	virtual ~CServer();

	virtual void SetClientName(int ClientID, const char *pName);
	virtual void SetClientClan(int ClientID, char const *pClan);
	virtual void SetClientCountry(int ClientID, int Country);
	virtual void SetClientScore(int ClientID, int Score);

	void Kick(int ClientID, const char *pReason);

	void DemoRecorder_HandleAutoStart();
	bool DemoRecorder_IsRecording();

	int64 TickStartTime(int Tick);

	int Init();

	void InitRconPasswordIfUnset();

	void SetRconCID(int ClientID);
	bool IsAuthed(int ClientID) const;
	bool IsBanned(int ClientID);
	bool IsShuttingDown() const;
	int GetClientInfo(int ClientID, CClientInfo *pInfo) const;
	void GetClientAddr(int ClientID, char *pAddrStr, int Size) const;
	int GetClientVersion(int ClientID) const;
	const char *ClientName(int ClientID) const;
	const char *ClientClan(int ClientID) const;
	int ClientCountry(int ClientID) const;
	bool ClientIngame(int ClientID) const;
	virtual unsigned GetClientMapID(int ClientID) const;

	virtual int SendMsg(CMsgPacker *pMsg, int Flags, int ClientID);

	// Sends one chunk to a client, translating it to the frozen 0.7 protocol
	// first when the peer negotiated the legacy handshake.
	void SendTranslated(CNetChunk *pPacket, int ClientID);

	void DoSnapshot();

	static int NewClientCallback(int ClientID, void *pUser);
	static int DelClientCallback(int ClientID, const char *pReason, void *pUser);

	void SendMap(int ClientID, bool Silent = false, bool NoFade = false, int ViewPosOffsetX = 0, int ViewPosOffsetY = 0);
	void SendConnectionReady(int ClientID);
	void SendRconLine(int ClientID, const char *pLine);
	static void SendRconLineAuthed(const char *pLine, void *pUser, bool Highlighted);

	void SendRconCmdAdd(const IConsole::CCommandInfo *pCommandInfo, int ClientID);
	void SendRconCmdRem(const IConsole::CCommandInfo *pCommandInfo, int ClientID);
	void UpdateClientRconCommands();

	void ProcessClientPacket(CNetChunk *pPacket);

	void SendServerInfo(int ClientID);
	void GenerateServerInfo(CPacker *pPacker, int ServerInfoVersion, bool IncludeClientInfo);
	// return: next StartClientID to continue from, or -1 if done
	int GenerateServerInfoPlayers(CPacker *pPacker, int ServerInfoVersion, int StartClientID);
	virtual void ExpireServerInfo();
	void UpdateRegisterServerInfo();

	void PumpNetwork();

	unsigned PushMapList(const char *pMapName);
	int LoadMap(unsigned MapID);
	int LoadMap(CMapInfo *pInfo);
	void RegisterMapEntrances(const char *pJsonData);
	void RegisterMapEntrances(const json_value &rEntrances);

	void InitRegister(class IEngine *pEngine, class CConfig *pConfig, class IConsole *pConsole, TOKEN SecurityToken);
	void InitInterfaces(IKernel *pKernel);
	int Run();
	void Free();

	static void ConKick(IConsole::IResult *pResult, void *pUser);
	static void ConStatus(IConsole::IResult *pResult, void *pUser);
	static void ConShutdown(IConsole::IResult *pResult, void *pUser);
	static void ConRecord(IConsole::IResult *pResult, void *pUser);
	static void ConStopRecord(IConsole::IResult *pResult, void *pUser);
	static void ConSaveConfig(IConsole::IResult *pResult, void *pUser);
	static void ConLogout(IConsole::IResult *pResult, void *pUser);
	static void ConchainSpecialInfoupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainMaxclientsUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainMaxclientsperipUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainModCommandUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainConsoleOutputLevelUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainRconPasswordSet(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainDatafileUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainMapgenUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	void ApplyGenerationVerboseSettings();

	void RegisterCommands();

	virtual int SnapNewID(unsigned MapID);
	virtual bool SnapFreeID(int ID, unsigned MapID);
	virtual void *SnapNewItem(int Type, int ID, int Size);
	void SnapSetStaticsize(int ItemType, int Size);

	virtual bool SwitchClientMap(int ClientID, unsigned MapID, bool *pSilent = nullptr);
	virtual const char *GetMapName(unsigned MapID);
	virtual const char *GetMapFloorName(unsigned MapID);
	virtual int GetMapWorldKind(unsigned MapID);
	virtual int GetTrainDestinationCount();
	virtual bool GetTrainDestination(int Index, const char **ppName, int *pRingSize);
	virtual bool TakeTrainDestinationChunk(const char *pFloorName, char *pOutMapName, int OutSize);
	virtual bool IsMapInUse(unsigned MapID);
	virtual void UnloadMap(unsigned MapID);
	virtual bool LoadMapForWorld(unsigned MapID);
	virtual bool IsMapAvailable(unsigned MapID);
	virtual void PrefetchMap(unsigned MapID);
	virtual bool RegisterMapName(const char *pName);
	virtual bool GetMapMeta(unsigned MapID, const char **ppData, int *pSize);
};

#endif
