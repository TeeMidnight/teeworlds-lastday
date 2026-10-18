/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef ENGINE_SHARED_PROTOCOL_H
#define ENGINE_SHARED_PROTOCOL_H

#include <base/system/string.h>

/*
	Connection diagram - How the initialization works.

	Client -> INFO -> Server
		Contains version info, name, and some other info.

	Client <- MAP <- Server
		Contains current map.

	Client -> READY -> Server
		The client has loaded the map and is ready to go,
		but the mod needs to send it's information aswell.
		modc_connected is called on the client and
		mods_connected is called on the server.
		The client should call client_entergame when the
		mod has done it's initialization.

	Client -> ENTERGAME -> Server
		Tells the server to start sending snapshots.
		client_entergame and server_client_enter is called.
*/

enum
{
	NETMSG_NULL = 0,

	// the first thing sent by the client
	// contains the version info for the client
	NETMSG_INFO = 1,

	// sent by server
	NETMSG_MAP_CHANGE, // sent when client should switch map
	NETMSG_MAP_DATA, // map transfer, contains a chunk of the map file
	NETMSG_SERVERINFO,
	NETMSG_CON_READY, // connection is ready, client should send start info
	NETMSG_SNAP, // normal snapshot, multiple parts
	NETMSG_SNAPEMPTY, // empty snapshot
	NETMSG_SNAPSINGLE, // ?
	NETMSG_SNAPSMALL, // todo 0.8: remove unused
	NETMSG_INPUTTIMING, // reports how off the input was
	NETMSG_RCON_AUTH_ON, // rcon authentication enabled
	NETMSG_RCON_AUTH_OFF, // rcon authentication disabled
	NETMSG_RCON_LINE, // line that should be printed to the remote console
	NETMSG_RCON_CMD_ADD,
	NETMSG_RCON_CMD_REM,

	NETMSG_AUTH_CHALLENGE, // unused
	NETMSG_AUTH_RESULT, // unused

	// sent by client
	NETMSG_READY, //
	NETMSG_ENTERGAME,
	NETMSG_INPUT, // contains the inputdata from the client
	NETMSG_RCON_CMD, //
	NETMSG_RCON_AUTH, //
	NETMSG_REQUEST_MAP_DATA, //

	NETMSG_AUTH_START, // unused
	NETMSG_AUTH_RESPONSE, // unused

	// sent by both
	NETMSG_PING,
	NETMSG_PING_REPLY,
	NETMSG_ERROR, // unused

	NETMSG_MAPLIST_ENTRY_ADD, // todo 0.8: move up
	NETMSG_MAPLIST_ENTRY_REM,
};

enum
{
	// NETMSG_MAP_CHANGE flag: the client already owns the map, so it applies
	// the switch without entering its loading state, answering NETMSG_READY,
	// or changing its connection state. Must match the client's value.
	MAP_CHANGE_FLAG_SILENT = 0x1,
	// The new map belongs to a ring, so the world on both sides of the switch
	// is continuous: the client should not fade the old map out over it.
	MAP_CHANGE_FLAG_NO_FADE = 0x2,
};

// NETMSG_MAP_CHANGE ends with the flags above followed by two ints: the new
// map's local view position minus the old map's, in game world units (not map
// tiles). Ordinary map changes send zero for both.

// this should be revised
enum
{
	SERVER_TICK_SPEED = 50,
	SERVERINFO_FLAG_PASSWORD = 0x1,
	SERVERINFO_FLAG_TIMESCORE = 0x2,
	SERVERINFO_LEVEL_MIN = 0,
	SERVERINFO_LEVEL_MAX = 2,

	SERVERINFO_VERSION_LEGACY = -1,
	SERVERINFO_VERSION_CURRENT, // add extend player info

	MAX_CLIENTS = 128,

	// Real connection slots the engine can serve. MAX_CLIENTS is kept above it
	// only because the 0.8 Tee/TeeInfo item id space is wider than the
	// connection table; it is deliberately NOT widened to hold bots, which are
	// a game-layer concept and never own a client slot. sv_max_clients is
	// capped at this value so a real id can never enter a bot range.
	NET_MAX_CLIENTS_07 = 64,

	// Tee/TeeInfo snapshot item id space: [0, MAX_CLIENTS) are real clients
	// (TeeInfoID == ClientID), above that belongs to the game/mod layer and
	// holds its bots. This is a *count* used as an exclusive bound throughout
	// (the generated protocol checks read MAX_TEES-1), so the largest valid tee
	// id is MAX_TEES - 1.
	MAX_TEES = 65535,

	// Bot tee id ranges (see CBotManager). A bot has no client slot; it gets a
	// display tee id from the range that fits the receiving client's protocol.
	// 0.7 peers address a tee with 7 bits and their clients occupy
	// [0, NET_MAX_CLIENTS_07), so their bots start right after that. 0.8 peers
	// have the full 16 bit space and take everything above the 0.7 range.
	// The ranges never overlap each other or a real client id.
	BOT_TEEID_07_MIN = NET_MAX_CLIENTS_07,
	BOT_TEEID_07_MAX = 127,
	BOT_TEEID_08_MIN = 128,
	BOT_TEEID_08_MAX = MAX_TEES - 1,
	// Ids each range can hold. The 0.7 one is tighter, so it bounds how many
	// bots a single client can be shown regardless of protocol.
	MAX_BOTS_07 = BOT_TEEID_07_MAX - BOT_TEEID_07_MIN + 1,
	MAX_BOTS_08 = BOT_TEEID_08_MAX - BOT_TEEID_08_MIN + 1,

	VANILLA_MAX_PLAYERS = 16,

	MAX_INPUT_SIZE = 128,
	MAX_SNAPSHOT_PACKSIZE = 900,

	MAX_NAME_LENGTH = 16,
	MAX_NAME_ARRAY_SIZE = MAX_NAME_LENGTH * UTF8_BYTE_LENGTH + 1,
	MAX_CLAN_LENGTH = 12,
	MAX_CLAN_ARRAY_SIZE = MAX_CLAN_LENGTH * UTF8_BYTE_LENGTH + 1,
	MAX_SKIN_LENGTH = 24,
	MAX_SKIN_ARRAY_SIZE = MAX_SKIN_LENGTH * UTF8_BYTE_LENGTH + 1,

	// message packing
	MSGFLAG_VITAL = 1,
	MSGFLAG_FLUSH = 2,
	MSGFLAG_NORECORD = 4,
	MSGFLAG_RECORD = 8,
	MSGFLAG_NOSEND = 16
};

#endif
