/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_PLAYER_H
#define GAME_SERVER_PLAYER_H

#include "alloc.h"

#include <base/uuid.h>

#include <generated/protocol.h>

#include <game/server/item.h>

class CDatabase;
class CGameWorld;
class CCharacter;
class CGameContext;
class IServer;

enum
{
	WEAPON_GAME = -3, // team switching etc
	WEAPON_SELF = -2, // console kill command
	WEAPON_WORLD = -1, // death tiles etc
};

// accessory loadout slots: passive items worn alongside the weapon slots.
// they are independent of NUM_WEAPONS, which is a protocol limit, so content
// can grow without touching the snapshot layout.
enum
{
	NUM_ACCESSORY_SLOTS = 3,
};

struct CTeeInfos
{
	char m_aaSkinPartNames[NUM_SKINPARTS][MAX_SKIN_ARRAY_SIZE];
	int m_aUseCustomColors[NUM_SKINPARTS];
	int m_aSkinPartColors[NUM_SKINPARTS];
};

// player object
class CPlayer
{
	MACRO_ALLOC_POOL_ID()

public:
	CPlayer(CGameWorld *pWorld, int ClientID, bool Dummy, bool AsSpec = false);
	~CPlayer();

	void Init(int CID);

	void TryRespawn();
	void Respawn();
	void SetTeam(int Team, bool DoChatMsg = true);
	int GetTeam() const { return m_Team; }
	int GetCID() const { return m_ClientID; }
	bool IsDummy() const { return m_Dummy; }

	void Tick();
	void PostTick();
	void Snap(int SnappingClient);

	void OnDirectInput(CNetObj_PlayerInput *NewInput);
	void OnPredictedInput(CNetObj_PlayerInput *NewInput);
	void OnDisconnect();

	void KillCharacter(int Weapon = WEAPON_GAME, bool Clean = true);
	CCharacter *GetCharacter();

	//---------------------------------------------------------
	// this is used for snapping so we know how we can clip the view for the player
	vec2 m_ViewPos;

	// states if the client is chatting, accessing a menu etc.
	int m_PlayerFlags;

	// used for snapping to just update latency if the scoreboard is active
	int m_aActLatency[MAX_CLIENTS];

	// used for spectator mode
	int GetSpectatorID() const { return m_SpectatorID; }
	bool SetSpectatorID(int SpecMode, int SpectatorID);
	bool m_DeadSpecMode;
	bool DeadCanFollow(CPlayer *pPlayer) const;
	void UpdateDeadSpecMode();

	bool m_IsReadyToEnter;
	bool m_IsReadyToPlay;

	bool m_RespawnDisabled;
	bool m_MapLoading;

	// A ring crossing detaches the character across a map switch; the landing
	// position therefore has to survive outside the world. Set when the switch
	// starts and consumed when the new map finishes loading.
	bool m_HasPendingArrival;
	vec2 m_PendingArrivalPos;

	//
	int m_Vote;
	int m_VotePos;
	//
	int m_LastVoteCallTick;
	int m_LastVoteTryTick;
	int m_LastChatTeamTick;
	int m_LastSetTeamTick;
	int m_LastSetSpectatorModeTick;
	int m_LastChangeInfoTick;
	int m_LastEmoteTick;
	int m_LastKillTick;
	int m_LastReadyChangeTick;

	// player skin
	CTeeInfos m_TeeInfos;

	int m_RespawnTick;
	int m_DieTick;
	int m_Score;
	int m_ScoreStartTick;
	int m_LastActionTick;
	int m_TeamChangeTick;

	int m_InactivityTickCounter;

	struct
	{
		int m_TargetX;
		int m_TargetY;
	} m_LatestActivity;

	// network latency calculations
	struct
	{
		int m_Accum;
		int m_AccumMin;
		int m_AccumMax;
		int m_Avg;
		int m_Min;
		int m_Max;
	} m_Latency;

	CGameWorld *GameWorld() const { return m_pWorld; }
	void SwitchWorld(CGameWorld *pWorld);

	struct CStatus
	{
		bool m_HideTip; // game menu
		// mercury chapter (forgetting): 0..100, 0 = clear mind, 100 = total
		// amnesia. rises on death, sinks through recall items / quiet time
		int m_Forgetting;
		int m_Level;
		// loadout (slot layout, persisted): which item res_id is placed on
		// every loadout slot ("" = empty). the loadout is a player-level
		// setting: the character copies it on spawn and the menu edits it
		// through the character.
		bool m_LoadoutSet; // true once the player customized the loadout
		CItemID m_aLoadout[NUM_WEAPONS];
		// accessory slots (persisted like the weapon layout)
		CItemID m_aAccessories[NUM_ACCESSORY_SLOTS];
	} m_Status;

	// account (bound on login / register)
	Uuid m_AccountUuid;
	bool m_LoggedIn;

	// persist the player status (forgetting, inventory, loadout) to/from the
	// database; each field is accessed individually through its json
	// path
	void SaveStatus(class CDatabase *pDB);
	// fill a queued database job with a snapshot of everything SaveStatus
	// writes, so the actual queries can run on the thread pool
	void CaptureStatus(struct CDbJob &Job);
	void LoadStatus(class CDatabase *pDB);

	// capture the current character's slot layout into m_Status.m_aLoadout
	// and mark the loadout as customized (m_Status.m_LoadoutSet)
	void CaptureLoadout();
	// apply m_Status.m_aLoadout onto the character: equip every slot whose
	// item the player still owns (missing items leave the slot empty)
	void ApplyLoadout();

	// capture the current character's accessory layout into m_Status
	void CaptureAccessories();
	// apply m_Status.m_aAccessories onto the character: equip every slot whose
	// item the player still owns (missing items leave the slot empty)
	void ApplyAccessories();

	// drill control: opening the scoreboard next to a drill freezes the
	// character and hands the cursor to it; a jump press releases the drill
	void UpdateDrillControl();
	// release the controlled drill (if any) and unfreeze the character
	void ExitDrillControl();

private:
	CCharacter *m_pCharacter;
	CGameWorld *m_pWorld;

	CGameContext *GameServer() const;
	IServer *Server() const;

	//
	bool m_Spawning;
	int m_ClientID;
	int m_Team;
	bool m_Dummy;

	// snapshot id of the drill this player is steering, or -1
	int m_DrillControlID;
	// previous state of the scoreboard flag, for edge detection
	bool m_ScoreboardHeld;

	// used for spectator mode
	int m_SpecMode;
	int m_SpectatorID;
	bool m_ActiveSpecSwitch;
};

#endif
