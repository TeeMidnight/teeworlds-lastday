/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/math.h>
#include <engine/shared/config.h>
#include <cmath>

#include <game/mapitems.h>
#include <game/version.h>
#include <generated/server_data.h>

#include "entities/character.h"
#include "entities/laser.h"
#include "entities/pickup.h"
#include "entities/projectile.h"
#include "entities/resource.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "player.h"

#include <game/server/database/account.h>
#include <game/server/database/playerdb.h>

CGameController::CGameController(CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
	m_pConfig = m_pGameServer->Config();
	m_pServer = m_pGameServer->Server();

	m_GameStartTick = Server()->Tick();
}

// activity
void CGameController::DoActivityCheck()
{
	if(Config()->m_SvInactiveKickTime == 0)
		return;

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(GameServer()->m_apPlayers[i] && !GameServer()->m_apPlayers[i]->IsDummy() && (GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS || Config()->m_SvInactiveKick > 0) &&
			!Server()->IsAuthed(i) && (GameServer()->m_apPlayers[i]->m_InactivityTickCounter > Config()->m_SvInactiveKickTime * Server()->TickSpeed() * 60))
		{
			if(GameServer()->m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS)
			{
				if(Config()->m_SvInactiveKickSpec)
					Server()->Kick(i, "Kicked for inactivity");
			}
			else
			{
				switch(Config()->m_SvInactiveKick)
				{
					case 1:
					{
						// move player to spectator
						DoTeamChange(GameServer()->m_apPlayers[i], TEAM_SPECTATORS);
					}
					break;
					case 2:
					{
						// move player to spectator if the reserved slots aren't filled yet, kick him otherwise
						int Spectators = 0;
						for(int j = 0; j < MAX_CLIENTS; ++j)
							if(GameServer()->m_apPlayers[j] && GameServer()->m_apPlayers[j]->GetTeam() == TEAM_SPECTATORS)
								++Spectators;
						if(Spectators >= Config()->m_SvMaxClients - GameServer()->GetMaxPlayerSlots())
							Server()->Kick(i, "Kicked for inactivity");
						else
							DoTeamChange(GameServer()->m_apPlayers[i], TEAM_SPECTATORS);
					}
					break;
					case 3:
					{
						// kick the player
						Server()->Kick(i, "Kicked for inactivity");
					}
				}
			}
		}
	}
}

bool CGameController::GetPlayersReadyState(int WithoutID)
{
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(i == WithoutID)
			continue; // skip
		if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS && !GameServer()->m_apPlayers[i]->m_IsReadyToPlay)
			return false;
	}

	return true;
}

void CGameController::SetPlayersReadyState(bool ReadyState)
{
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS && (ReadyState || !GameServer()->m_apPlayers[i]->m_DeadSpecMode))
			GameServer()->m_apPlayers[i]->m_IsReadyToPlay = ReadyState;
	}
}

// event
int CGameController::OnCharacterDeath(CCharacter *pVictim, CPlayer *pKiller, int Weapon)
{
	// update spectator modes for dead players in survival
	for(int i = 0; i < MAX_CLIENTS; ++i)
		if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->m_DeadSpecMode)
			GameServer()->m_apPlayers[i]->UpdateDeadSpecMode();
	// do scoreing
	if(!pKiller || Weapon == WEAPON_GAME)
		return 0;
	if(pKiller == pVictim->GetPlayer())
		pVictim->GetPlayer()->m_Score--; // suicide or world
	else
		pKiller->m_Score++; // normal kill
	if(Weapon == WEAPON_SELF)
		pVictim->GetPlayer()->m_RespawnTick = Server()->Tick() + Server()->TickSpeed() * 3.0f;

	return 0;
}

void CGameController::OnCharacterSpawn(CCharacter *pChr)
{
	// default health
	pChr->IncreaseHealth(10);

	// give default weapons
	pChr->GiveWeapon(WEAPON_HAMMER, -1);
	pChr->GiveWeapon(WEAPON_GUN, 10);
}

bool CGameController::OnEntity(CGameWorld *pWorld, int Index, vec2 Pos)
{
	int Type = -1;

	switch(Index)
	{
		case ENTITY_SPAWN:
			pWorld->m_alSpawnPoints[0].add(Pos);
			break;
		case ENTITY_SPAWN_RED:
			pWorld->m_alSpawnPoints[1].add(Pos);
			break;
		case ENTITY_SPAWN_BLUE:
			pWorld->m_alSpawnPoints[2].add(Pos);
			break;
		case ENTITY_ARMOR_1:
			Type = PICKUP_ARMOR;
			break;
		case ENTITY_HEALTH_1:
			Type = PICKUP_HEALTH;
			break;
		case ENTITY_WEAPON_SHOTGUN:
			Type = PICKUP_SHOTGUN;
			break;
		case ENTITY_WEAPON_GRENADE:
			Type = PICKUP_GRENADE;
			break;
		case ENTITY_WEAPON_LASER:
			Type = PICKUP_LASER;
			break;
		case ENTITY_POWERUP_NINJA:
			Type = PICKUP_NINJA;
	}

	if(Type != -1)
	{
		new CPickup(pWorld, Type, Pos);
		return true;
	}

	return false;
}

enum
{
	TILE_FLOOR_ENTRANCE_1 = 35,
	TILE_FLOOR_ENTRANCE_2 = 36,

	COLFLAG_ENTRANCE_1_FLAG = 1 << 4,
	COLFLAG_ENTRANCE_2_FLAG = 1 << 5,
};

bool CGameController::OnExtraTile(CGameWorld *pWorld, int Index, vec2 Pos)
{
	/*
		Example: Do some thing like:

		int Flag = -1;
		switch(Index)
		{
			case TILE_START: Flag = COLFLAG_START; break;
			case TILE_FINISH: Flag = COLFLAG_FINISH; break;
		}
		if(Flag == -1)
			return false;
		pChr->GameWorld()->Collision()->SetFlagFor(Pos, Flag);
		return true;
	*/

	int Flag = -1;
	switch(Index)
	{
		case TILE_FLOOR_ENTRANCE_1: Flag = COLFLAG_ENTRANCE_1_FLAG; break;
		case TILE_FLOOR_ENTRANCE_2: Flag = COLFLAG_ENTRANCE_2_FLAG; break;
	}
	if(Flag != -1)
		pWorld->Collision()->SetFlagFor(Pos, Flag);
	return false;
}

void CGameController::OnPlayerConnect(CPlayer *pPlayer)
{
	int ClientID = pPlayer->GetCID();
	pPlayer->Respawn();

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "team_join player='%d:%s' team=%d", ClientID, Server()->ClientName(ClientID), pPlayer->GetTeam());
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// update game info
	SendGameInfo(ClientID);
}

void CGameController::OnPlayerDisconnect(CPlayer *pPlayer)
{
	pPlayer->OnDisconnect();

	int ClientID = pPlayer->GetCID();
	if(Server()->ClientIngame(ClientID))
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "leave player='%d:%s'", ClientID, Server()->ClientName(ClientID));
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "game", aBuf);
	}

	if(pPlayer->GetTeam() != TEAM_SPECTATORS)
	{
		--m_RealPlayerNum;
	}
}

void CGameController::OnPlayerInfoChange(CPlayer *pPlayer)
{
}

void CGameController::OnPlayerReadyChange(CPlayer *pPlayer)
{
	// change players ready state
	pPlayer->m_IsReadyToPlay ^= 1;
}

// general
void CGameController::Snap(int SnappingClient)
{
	CNetObj_GameData *pGameData = static_cast<CNetObj_GameData *>(Server()->SnapNewItem(NETOBJTYPE_GAMEDATA, 0, sizeof(CNetObj_GameData)));
	if(!pGameData)
		return;

	pGameData->m_GameStartTick = m_GameStartTick;
	pGameData->m_GameStateFlags = 0;
	pGameData->m_GameStateEndTick = 0; // no timer/infinite = 0, on end = GameEndTick, otherwise = GameStateEndTick

	CNetObj_GameDataPrediction *pGameDataPrediction = static_cast<CNetObj_GameDataPrediction *>(Server()->SnapNewItem(NETOBJTYPE_GAMEDATAPREDICTION, 0, sizeof(CNetObj_GameDataPrediction)));
	if(!pGameDataPrediction)
		return;

	pGameDataPrediction->m_PredictionFlags = GAMEPREDICTIONFLAG_EVENT | GAMEPREDICTIONFLAG_INPUT;
	// demo recording
	if(SnappingClient == -1)
	{
		CNetObj_De_GameInfo *pGameInfo = static_cast<CNetObj_De_GameInfo *>(Server()->SnapNewItem(NETOBJTYPE_DE_GAMEINFO, 0, sizeof(CNetObj_De_GameInfo)));
		if(!pGameInfo)
			return;

		pGameInfo->m_GameFlags = 0;
		pGameInfo->m_ScoreLimit = 0;
		pGameInfo->m_TimeLimit = 0;
		pGameInfo->m_MatchNum = 0;
		pGameInfo->m_MatchCurrent = 1;
	}
}

void CGameController::Tick()
{
	// check for inactive players
	DoActivityCheck();
}

bool CGameController::IsFriendlyFire(int ClientID1, int ClientID2, int Damage) const
{
	if(ClientID1 == ClientID2)
		return false;

	if(!GameServer()->m_apPlayers[ClientID1] || !GameServer()->m_apPlayers[ClientID2])
		return false;

	return true;
}

bool CGameController::IsFriendlyTeamFire(int Team1, int Team2, int Damage) const
{
	return Team1 == Team2;
}

int CGameController::GetPlayerCheckTeam(CPlayer *pPlayer) const
{
	if(!pPlayer)
		return TEAM_RED;
	return pPlayer->GetTeam();
}

void CGameController::SendGameInfo(int ClientID)
{
	CNetMsg_Sv_GameInfo GameInfoMsg;
	GameInfoMsg.m_GameFlags = 0;
	GameInfoMsg.m_ScoreLimit = 0;
	GameInfoMsg.m_TimeLimit = 0;
	GameInfoMsg.m_MatchNum = 0;
	GameInfoMsg.m_MatchCurrent = 1;
	Server()->SendPackMsg(&GameInfoMsg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientID);
}

// spawn
bool CGameController::CanSpawn(CGameWorld *pWorld, int Team, vec2 *pOutPos) const
{
	// spectators can't spawn
	if(Team == TEAM_SPECTATORS)
		return false;

	CSpawnEval Eval(pWorld);
	Eval.m_RandomSpawn = true;

	EvaluateSpawnType(&Eval, 0);
	EvaluateSpawnType(&Eval, 1);
	EvaluateSpawnType(&Eval, 2);

	*pOutPos = Eval.m_Pos;
	return Eval.m_Got;
}

float CGameController::EvaluateSpawnPos(CSpawnEval *pEval, vec2 Pos) const
{
	float Score = 0.0f;

	for(CGameWorld::TypeRange r = pEval->m_pWorld->DoTypeRange(CGameWorld::ENTTYPE_PROJECTILE); !r.empty(); r.pop_front())
	{
		CCharacter *pChr = static_cast<CCharacter *>(r.front());
		// team mates are not as dangerous as enemies
		float Scoremod = 1.0f;
		if(pEval->m_FriendlyTeam != -1 && pChr->GetPlayer()->GetTeam() == pEval->m_FriendlyTeam)
			Scoremod = 0.5f;

		float d = distance(Pos, pChr->GetPos());
		Score += Scoremod * (d == 0 ? 1000000000.0f : 1.0f / d);
	}

	return Score;
}

void CGameController::EvaluateSpawnType(CSpawnEval *pEval, int Type) const
{
	// get spawn point
	for(int i = 0; i < pEval->m_pWorld->m_alSpawnPoints[Type].size(); i++)
	{
		// check if the position is occupado
		array<CEntity *> lpEnts;
		lpEnts.hint_size(8);
		int Num = pEval->m_pWorld->FindEntities(pEval->m_pWorld->m_alSpawnPoints[Type][i], 64, lpEnts, CGameWorld::ENTTYPE_CHARACTER);
		vec2 Positions[5] = {vec2(0.0f, 0.0f), vec2(-32.0f, 0.0f), vec2(0.0f, -32.0f), vec2(32.0f, 0.0f), vec2(0.0f, 32.0f)}; // start, left, up, right, down
		int Result = -1;
		for(int Index = 0; Index < 5 && Result == -1; ++Index)
		{
			Result = Index;
			for(int c = 0; c < Num; ++c)
				if(pEval->m_pWorld->Collision()->CheckPoint(pEval->m_pWorld->m_alSpawnPoints[Type][i] + Positions[Index]) ||
					distance(lpEnts[c]->GetPos(), pEval->m_pWorld->m_alSpawnPoints[Type][i] + Positions[Index]) <= lpEnts[c]->GetProximityRadius())
				{
					Result = -1;
					break;
				}
		}
		if(Result == -1)
			continue; // try next spawn point

		vec2 P = pEval->m_pWorld->m_alSpawnPoints[Type][i] + Positions[Result];
		float S = pEval->m_RandomSpawn ? (Result + random_float()) : EvaluateSpawnPos(pEval, P);
		if(!pEval->m_Got || pEval->m_Score > S)
		{
			pEval->m_Got = true;
			pEval->m_Score = S;
			pEval->m_Pos = P;
		}
	}
}

bool CGameController::GetStartRespawnState() const
{
	return false;
}

// team
bool CGameController::CanChangeTeam(CPlayer *pPlayer, int JoinTeam) const
{
	// joining the game (leaving spectator) requires a logged-in account;
	// server bots/dummies are exempt
	if(JoinTeam != TEAM_SPECTATORS && !pPlayer->IsDummy() && !pPlayer->m_LoggedIn)
		return false;
	return true;
}

bool CGameController::CanJoinTeam(int Team, int NotThisID) const
{
	return true;
}

int CGameController::ClampTeam(int Team) const
{
	if(Team < TEAM_RED)
		return TEAM_SPECTATORS;
	return TEAM_RED;
}

void CGameController::DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg)
{
	Team = ClampTeam(Team);
	if(Team == pPlayer->GetTeam())
		return;

	int OldTeam = pPlayer->GetTeam();
	pPlayer->SetTeam(Team);

	int ClientID = pPlayer->GetCID();

	// notify clients
	CNetMsg_Sv_Team Msg;
	Msg.m_ClientID = ClientID;
	Msg.m_Team = Team;
	Msg.m_Silent = DoChatMsg ? 0 : 1;
	Msg.m_CooldownTick = pPlayer->m_TeamChangeTick;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "team_join player='%d:%s' team=%d->%d", ClientID, Server()->ClientName(ClientID), OldTeam, Team);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// update effected game settings
	if(OldTeam != TEAM_SPECTATORS)
	{
		--m_RealPlayerNum;
	}
	if(Team != TEAM_SPECTATORS)
	{
		++m_RealPlayerNum;
	}
	OnPlayerInfoChange(pPlayer);
	GameServer()->OnClientTeamChange(ClientID);

	// reset inactivity counter when joining the game
	if(OldTeam == TEAM_SPECTATORS)
		pPlayer->m_InactivityTickCounter = 0;
}

int CGameController::GetStartTeam()
{
	return TEAM_RED;
}

bool CGameController::HandleCharacterTiles(CCharacter *pChr, vec2 LastPos, vec2 NewPos)
{
	static const vec2 ColBox(CCharacterCore::PHYS_SIZE, CCharacterCore::PHYS_SIZE);
	if(!pChr)
		return false;

	CGameWorld *pWorld = pChr->GameWorld();

	// generated worlds define their entrances as tile regions in the map
	// info; teleport the character when it enters one of these regions.
	// The whole movement path is checked (like the collision box movement
	// test) so fast characters cannot skip over a single-tile entrance row.
	if(pWorld->m_lEntrances.size())
	{
		vec2 Pos = LastPos;
		const auto FindEntrance = [&](int *pEntranceIndex)
		{
			// the tile of a position: use floor so that positions inside a
			// tile (e.g. interpolated path samples) are attributed correctly
			const int TileX = (int) floor(Pos.x / 32.0f);
			const int TileY = (int) floor(Pos.y / 32.0f);
			for(int i = 0; i < pWorld->m_lEntrances.size(); i++)
			{
				const CGameWorld::CEntranceInfo &Entrance = pWorld->m_lEntrances[i];
				if(TileX >= Entrance.m_StartX && TileX <= Entrance.m_EndX &&
					TileY >= Entrance.m_StartY && TileY <= Entrance.m_EndY)
				{
					*pEntranceIndex = i;
					return true;
				}
			}
			return false;
		};

		int EntranceIndex = -1;
		const float Distance = distance(NewPos, LastPos);
		const int Steps = maximum(1, (int) Distance);
		// sample the whole path from LastPos to NewPos (every step is less
		// than one tile, so no tile of the path can be skipped)
		const vec2 Step = (NewPos - LastPos) / (float) Steps;
		for(int i = 0; i <= Steps; i++)
		{
			if(FindEntrance(&EntranceIndex))
				break;
			Pos = Pos + Step;
		}
		if(EntranceIndex >= 0)
		{
			pChr->GameServer()->SwitchPlayerWorld(pChr->GetPlayer(), str_quickhash(pWorld->m_lEntrances[EntranceIndex].m_aTargetMap));
			return true;
		}
		return false;
	}

	// the main world (e.g. Connector) uses the classic entrance tiles and
	// the directly named entrance maps from the map info
	int Flag = pWorld->Collision()->TestBoxMoveAt(LastPos, NewPos, ColBox);
	if(Flag & COLFLAG_ENTRANCE_1_FLAG)
	{
		pChr->GameServer()->SwitchPlayerWorld(pChr->GetPlayer(), str_quickhash(pWorld->m_aEntrances[0]));
		return true;
	}
	if(Flag & COLFLAG_ENTRANCE_2_FLAG)
	{
		pChr->GameServer()->SwitchPlayerWorld(pChr->GetPlayer(), str_quickhash(pWorld->m_aEntrances[1]));
		return true;
	}
	return false;
}

void CGameController::Com_About(IConsole::IResult *pResult, void *pContext)
{
	CCommandManager::SCommandContext *pCmdContext = static_cast<CCommandManager::SCommandContext *>(pContext);
	CGameController *pSelf = static_cast<CGameController *>(pCmdContext->m_pContext);
	int ClientID = pCmdContext->m_ClientID;
	pSelf->SendSystemChat(ClientID, MOD_NAME " v" MOD_VERSION " by Bamcane");
}

void CGameController::ComRegister(IConsole::IResult *pResult, void *pContext)
{
	CCommandManager::SCommandContext *pCmdContext = static_cast<CCommandManager::SCommandContext *>(pContext);
	CGameController *pSelf = static_cast<CGameController *>(pCmdContext->m_pContext);
	int ClientID = pCmdContext->m_ClientID;
	CPlayer *pPlayer = pSelf->GameServer()->m_apPlayers[ClientID];
	CPlayerDB *pDB = pSelf->GameServer()->PlayerDB();

	if(!pPlayer)
		return;
	if(!pDB)
	{
		pSelf->SendSystemChat(ClientID, Localize("The database is not available.", "Account"));
		return;
	}
	if(pResult->NumArguments() < 2)
	{
		pSelf->SendSystemChat(ClientID, Localize("Usage: /register <username> <password>", "Account"));
		return;
	}
	if(pPlayer->m_LoggedIn)
	{
		pSelf->SendSystemChat(ClientID, Localize("You are already logged in.", "Account"));
		return;
	}

	const char *pUsername = pResult->GetString(0);
	const char *pPassword = pResult->GetString(1);

	CPlayerDB::SPlayerData Row;
	if(pDB->FindByName(pUsername, Row))
	{
		pSelf->SendSystemChat(ClientID, Localize("This username is already taken.", "Account"));
		return;
	}

	mem_zero(&Row, sizeof(Row));
	Row.m_Uuid = time_uuid();
	str_copy(Row.m_aUsername, pUsername, sizeof(Row.m_aUsername));
	HashPassword(Row.m_aPasswordHash, sizeof(Row.m_aPasswordHash), pPassword);

	if(!pDB->InsertPlayer(Row))
	{
		pSelf->SendSystemChat(ClientID, Localize("Failed to create the account.", "Account"));
		return;
	}

	pPlayer->m_AccountUuid = Row.m_Uuid;
	pPlayer->m_LoggedIn = true;
	if(pPlayer->GetTeam() == TEAM_SPECTATORS)
		pSelf->DoTeamChange(pPlayer, TEAM_RED, false);
	pPlayer->SaveStatus(pDB); // persist the initial status fields
	pSelf->SendSystemChat(ClientID, Localize("Account created. You are now logged in.", "Account"));
}

void CGameController::ComLogin(IConsole::IResult *pResult, void *pContext)
{
	CCommandManager::SCommandContext *pCmdContext = static_cast<CCommandManager::SCommandContext *>(pContext);
	CGameController *pSelf = static_cast<CGameController *>(pCmdContext->m_pContext);
	int ClientID = pCmdContext->m_ClientID;
	CPlayer *pPlayer = pSelf->GameServer()->m_apPlayers[ClientID];
	CPlayerDB *pDB = pSelf->GameServer()->PlayerDB();

	if(!pPlayer)
		return;
	if(!pDB)
	{
		pSelf->SendSystemChat(ClientID, Localize("The database is not available.", "Account"));
		return;
	}
	if(pResult->NumArguments() < 2)
	{
		pSelf->SendSystemChat(ClientID, Localize("Usage: /login <username> <password>", "Account"));
		return;
	}
	if(pPlayer->m_LoggedIn)
	{
		pSelf->SendSystemChat(ClientID, Localize("You are already logged in.", "Account"));
		return;
	}

	const char *pUsername = pResult->GetString(0);
	const char *pPassword = pResult->GetString(1);

	CPlayerDB::SPlayerData Row;
	if(!pDB->FindByName(pUsername, Row))
	{
		pSelf->SendSystemChat(ClientID, Localize("No such account.", "Account"));
		return;
	}
	if(!VerifyPassword(Row.m_aPasswordHash, pPassword))
	{
		pSelf->SendSystemChat(ClientID, Localize("Wrong password.", "Account"));
		return;
	}

	pPlayer->m_AccountUuid = Row.m_Uuid;
	pPlayer->m_LoggedIn = true;
	if(pPlayer->GetTeam() == TEAM_SPECTATORS)
		pSelf->DoTeamChange(pPlayer, TEAM_RED, false);
	pPlayer->TryRespawn();
	pPlayer->LoadStatus(pDB);
	pSelf->SendSystemChat(ClientID, Localize("Logged in.", "Account"));
}

void CGameController::RegisterChatCommands(CCommandManager *pManager)
{
	pManager->AddCommand("about", "About the mod", "", Com_About, this);
	pManager->AddCommand("info", "About the mod", "", Com_About, this);
	pManager->AddCommand("register", "Register a new account", "s[username] s[password]", ComRegister, this);
	pManager->AddCommand("login", "Log in to an account", "s[username] s[password]", ComLogin, this);
}

bool CGameController::CanCharacterWeaponFullAuto(CCharacter *pChr, int Weapon)
{
	return Weapon == WEAPON_GRENADE || Weapon == WEAPON_SHOTGUN || Weapon == WEAPON_LASER;
}

void CGameController::SendSystemChat(int TargetID, const char *pMsg)
{
	GameServer()->SendChat(-1, CHAT_ALL, TargetID, pMsg);
}

int CGameController::OnCharacterFireWeapon(CCharacter *pChr, vec2 Direction, int Weapon)
{
	if(!pChr)
		return 0;

	int ClientID = pChr->GetCID();
	vec2 ChrPos = pChr->GetPos();
	vec2 ProjStartPos = ChrPos + Direction * pChr->GetProximityRadius() * 0.75f;

	int ReloadTimer = 0;
	switch(Weapon)
	{
		case WEAPON_HAMMER:
		{
			pChr->GameWorld()->CreateSound(ChrPos, SOUND_HAMMER_FIRE);

			array<CEntity *> lpEnts;
			lpEnts.hint_size(8);
			int Hits = 0;
			const int Num = pChr->GameWorld()->FindFlagEntities(ProjStartPos, pChr->GetProximityRadius() * 0.5f, lpEnts, CGameWorld::ENTFLAG_HITABLE);
			for(int i = 0; i < Num; ++i)
			{
				CCharacter *pTarget = static_cast<CCharacter *>(lpEnts[i]);

				if((pTarget == pChr) || pChr->GameWorld()->Collision()->IntersectLine(ProjStartPos, pTarget->GetPos(), NULL, NULL))
					continue;

				// set his velocity to fast upward (for now)
				if(length(pTarget->GetPos() - ProjStartPos) > 0.0f)
					pChr->GameWorld()->CreateHammerHit(pTarget->GetPos() - normalize(pTarget->GetPos() - ProjStartPos) * pChr->GetProximityRadius() * 0.5f);
				else
					pChr->GameWorld()->CreateHammerHit(ProjStartPos);

				vec2 Dir;
				if(length(pTarget->GetPos() - ChrPos) > 0.0f)
					Dir = normalize(pTarget->GetPos() - ChrPos);
				else
					Dir = vec2(0.f, -1.f);

				pTarget->TakeHit(vec2(0.f, -1.f) + normalize(Dir + vec2(0.f, -1.1f)) * 10.0f, Dir * -1, g_pData->m_Weapons.m_Hammer.m_pBase->m_Damage,
					pChr, Weapon);
				Hits++;
			}

			// harvest resources with the hammer (default 1 hardness per hit)
			array<CEntity *> lpResEnts;
			const int NumRes = pChr->GameWorld()->FindEntities(ProjStartPos, pChr->GetProximityRadius() * 0.5f + 20.0f, lpResEnts, CGameWorld::ENTTYPE_RESOURCE);
			for(int i = 0; i < NumRes; ++i)
			{
				CResourceEntity *pRes = static_cast<CResourceEntity *>(lpResEnts[i]);
				if(pRes->IsRespawning())
					continue;
				if(pChr->GameWorld()->Collision()->IntersectLine(ProjStartPos, pRes->GetPos(), NULL, NULL))
					continue;

				// remember the resource id before the hit: the entity is
				// destroyed when it gets depleted
				const bool Depleted = pRes->RemainingHardness() <= 1;
				char aResId[32];
				if(Depleted)
					str_copy(aResId, pRes->ResId(), sizeof(aResId));

				pRes->TakeHit(1);

				if(Depleted)
				{
					CPlayer *pPlayer = pChr->GetPlayer();
					if(pPlayer && !GameServer()->Item()->AddItem(pPlayer->GetCID(), aResId, 1)) continue;
					pChr->GameWorld()->CreateSound(pRes->GetPos(), SOUND_PICKUP_ARMOR);
				}
				else
				{
					pChr->GameWorld()->CreateHammerHit(pRes->GetPos());
				}
				Hits++;
			}

			// if we Hit anything, we have to wait for the reload
			if(Hits)
				ReloadTimer = Server()->TickSpeed() / 3;
		}
		break;

		case WEAPON_GUN:
		{
			new CProjectile(pChr->GameWorld(), WEAPON_GUN,
				ClientID,
				ProjStartPos,
				Direction,
				(int) (Server()->TickSpeed() * GameServer()->Tuning()->m_GunLifetime),
				g_pData->m_Weapons.m_Gun.m_pBase->m_Damage, false, 0, -1, WEAPON_GUN);

			pChr->GameWorld()->CreateSound(ChrPos, SOUND_GUN_FIRE);
		}
		break;

		case WEAPON_SHOTGUN:
		{
			int ShotSpread = 2;

			for(int i = -ShotSpread; i <= ShotSpread; ++i)
			{
				float Spreading[] = {-0.185f, -0.070f, 0, 0.070f, 0.185f};
				float a = angle(Direction);
				a += Spreading[i + 2];
				float v = 1 - (absolute(i) / (float) ShotSpread);
				float Speed = mix((float) GameServer()->Tuning()->m_ShotgunSpeeddiff, 1.0f, v);
				new CProjectile(pChr->GameWorld(), WEAPON_SHOTGUN,
					ClientID,
					ProjStartPos,
					vec2(cosf(a), sinf(a)) * Speed,
					(int) (Server()->TickSpeed() * GameServer()->Tuning()->m_ShotgunLifetime),
					g_pData->m_Weapons.m_Shotgun.m_pBase->m_Damage, false, 0, -1, WEAPON_SHOTGUN);
			}

			pChr->GameWorld()->CreateSound(ChrPos, SOUND_SHOTGUN_FIRE);
		}
		break;

		case WEAPON_GRENADE:
		{
			new CProjectile(pChr->GameWorld(), WEAPON_GRENADE,
				ClientID,
				ProjStartPos,
				Direction,
				(int) (Server()->TickSpeed() * GameServer()->Tuning()->m_GrenadeLifetime),
				g_pData->m_Weapons.m_Grenade.m_pBase->m_Damage, true, 0, SOUND_GRENADE_EXPLODE, WEAPON_GRENADE);

			pChr->GameWorld()->CreateSound(ChrPos, SOUND_GRENADE_FIRE);
		}
		break;

		case WEAPON_LASER:
		{
			new CLaser(pChr->GameWorld(), ChrPos, Direction, GameServer()->Tuning()->m_LaserReach, ClientID, g_pData->m_Weapons.m_aId[WEAPON_LASER].m_Damage);
			pChr->GameWorld()->CreateSound(ChrPos, SOUND_LASER_FIRE);
		}
		break;

		case WEAPON_NINJA:
		{
			pChr->DoNinjaFire(Direction, g_pData->m_Weapons.m_Ninja.m_Movetime * Server()->TickSpeed() / 1000);
			pChr->GameWorld()->CreateSound(ChrPos, SOUND_NINJA_FIRE);
		}
		break;
	}
	if(!ReloadTimer)
		ReloadTimer = g_pData->m_Weapons.m_aId[Weapon].m_Firedelay * Server()->TickSpeed() / 1000;

	return ReloadTimer;
}
