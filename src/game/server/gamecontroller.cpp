/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */
/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/math.h>
#include <base/uuid.h>
#include <engine/shared/config.h>
#include <game/collision.h>
#include <game/localization_format.h>
#include <game/mapitems.h>
#include <game/server/currency.h>
#include <game/server/database/account.h>
#include <game/server/database/database.h>
#include <game/version.h>
#include <generated/server_data.h>

#include "entities/character.h"
#include "entities/laser.h"
#include "entities/pickup.h"
#include "entities/projectile.h"
#include "entities/resource.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "item.h"
#include "player.h"
#include "scripting/scripted_weapon.h"
#include "team.h"
#include "trade.h"
#include "weapon.h"
#include "weaponmanager.h"

#include <cmath>

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

	// mercury chapter: dying erodes the memory. every death raises the
	// forgetting meter (+20, cap 100). milestones whisper what was lost; a
	// death that would reach 100 triggers total amnesia: the meter collapses
	// to 60 so the game never hard-locks (quiet time / recall items lower it
	// further)
	CPlayer *pVictimPlayer = pVictim->GetPlayer();
	if(pVictimPlayer)
	{
		CLanguageScope LanguageScope(pVictimPlayer->Language());
		const int OldForgetting = pVictimPlayer->m_Status.m_Forgetting;
		int NewForgetting = clamp(OldForgetting + 20, 0, 100);
		if(NewForgetting >= 100)
		{
			NewForgetting = 60;
			GameServer()->SendChat(-1, CHAT_WHISPER, pVictimPlayer->GetCID(),
				Localize("You forget who you are entirely.", "Mercury"));
		}
		else
		{
			if(OldForgetting < 50 && NewForgetting >= 50)
				GameServer()->SendChat(-1, CHAT_WHISPER, pVictimPlayer->GetCID(),
					Localize("You start to forget how to craft some things.", "Mercury"));
			else if(OldForgetting < 75 && NewForgetting >= 75)
				GameServer()->SendChat(-1, CHAT_WHISPER, pVictimPlayer->GetCID(),
					Localize("You can no longer tell what many of your belongings are.", "Mercury"));
		}
		pVictimPlayer->m_Status.m_Forgetting = NewForgetting;
	}

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
	CPlayer *pPlayer = pChr->GetPlayer();
	// wear the player's saved accessories first: they raise the health cap, so
	// the initial heal below fills the boosted maximum
	if(pPlayer)
		pPlayer->ApplyAccessories();
	pChr->IncreaseHealth(pChr->MaxHealth());

	if(pChr->NumWeaponsHeld() == 0)
	{
		if(pPlayer && pPlayer->m_Status.m_LoadoutSet)
		{
			// restore the player's saved slot layout
			pPlayer->ApplyLoadout();
		}
		else
		{
			// first spawn / no custom layout yet: equip all owned weapons
			int Slot = 0;
			for(int i = 0; i < WeaponManager()->NumWeapons() && Slot < NUM_WEAPONS; i++)
			{
				IWeaponInterface *pWeapon = WeaponManager()->GetWeaponByIndex(i);
				if(pWeapon && GameServer()->Item()->HasItem(pChr->GetCID(), pWeapon->Name()))
					pChr->EquipWeaponSlot(Slot++, CItemID::FromName(pWeapon->Name()));
			}
			// Scripted weapons (everything but ninja, and future native
			// weapons) are not in the native registry. A scripted entry that
			// overrides a native name is already equipped above, so only add
			// the missing names.
			for(int i = 0; i < WeaponManager()->NumScriptedWeapons() && Slot < NUM_WEAPONS; i++)
			{
				IWeaponInterface *pWeapon = WeaponManager()->GetScriptedWeaponByIndex(i);
				if(!pWeapon || !GameServer()->Item()->HasItem(pChr->GetCID(), pWeapon->Name()))
					continue;
				bool AlreadyEquipped = false;
				for(int s = 0; s < NUM_WEAPONS; s++)
				{
					const CItemID SlotItem = pChr->WeaponAtSlot(s);
					if(!SlotItem.IsZero() &&
						str_comp(GameServer()->Item()->GetWeaponName(SlotItem), pWeapon->Name()) == 0)
					{
						AlreadyEquipped = true;
						break;
					}
				}
				if(!AlreadyEquipped)
					pChr->EquipWeaponSlot(Slot++, CItemID::FromName(pWeapon->Name()));
			}
		}
	}

	// make sure the active slot actually holds a weapon
	if(pChr->GetActiveWeapon().IsZero())
	{
		for(int i = 0; i < NUM_WEAPONS; i++)
		{
			if(!pChr->WeaponAtSlot(i).IsZero())
			{
				pChr->SetWeapon(i);
				break;
			}
		}
	}
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
			// weapon pickups are intentionally not spawned anymore: weapons are
			// items now and have to be crafted / given through the item system
	}

	if(Type != -1)
	{
		new CPickup(pWorld, Type, Pos);
		return true;
	}

	return false;
}

bool CGameController::OnExtraTile(CGameWorld *pWorld, int Index, vec2 Pos)
{
	// A train platform is a marker, not a teleport: it only gets a collision
	// flag so the travel menu can ask whether the player stands on one. The
	// fixed floor entrances that installed their own teleport flags are gone.
	if(Index == TILE_TRAIN_ENTRANCE)
	{
		pWorld->Collision()->SetFlagFor(Pos, CCollision::COLFLAG_TRAIN_ENTRANCE);
		return true;
	}
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
	CLanguageScope LanguageScope(pPlayer->Language());
	SendSystemChat(pPlayer->GetCID(), pPlayer->m_IsReadyToPlay ? Localize("Action state: running", "Action") : Localize("Action state: silent", "Action"));
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

	// 0.8: prediction flags moved from the removed CNetObj_GameDataPrediction
	// object into CNetObj_GameData::m_PredictionFlags.
	pGameData->m_PredictionFlags = GAMEPREDICTIONFLAG_EVENT | GAMEPREDICTIONFLAG_INPUT | GAMEPREDICTIONFLAG_NO_SMOOTH_CAMERA;

	// 0.8: the De_GameInfo demo object is gone; game info is delivered by the
	// Sv_GameInfo message via SendGameInfo() (see OnPlayerConnect / CheckGameInfo).
}

void CGameController::Tick()
{
	// check for inactive players
	DoActivityCheck();
}

bool CGameController::IsFriendlyFire(int ClientID1, int ClientID2, int Damage) const
{
	(void) Damage;
	if(ClientID1 == ClientID2)
		return false;

	CPlayer *pPlayer1 = ClientID1 >= 0 && ClientID1 < MAX_CLIENTS ? GameServer()->m_apPlayers[ClientID1] : nullptr;
	CPlayer *pPlayer2 = ClientID2 >= 0 && ClientID2 < MAX_CLIENTS ? GameServer()->m_apPlayers[ClientID2] : nullptr;
	if(!pPlayer1 || !pPlayer2)
		return false;

	// Only a shared team can make a hit friendly. The leader's friendly-fire
	// switch decides whether it is blocked; players without a team (or in
	// different teams) can always fight each other.
	CTeamSystem *pTeams = GameServer()->Teams();
	if(!pTeams || !pTeams->SameTeam(pPlayer1->m_AccountUuid, pPlayer2->m_AccountUuid))
		return false;
	const SGameTeam *pTeam = pTeams->TeamFor(pPlayer1->m_AccountUuid);
	return pTeam && !pTeam->m_FriendlyFire;
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
	// Pick the safest point instead of a random one: a train arrival must not
	// land next to a monster that already camped the spawn point.
	Eval.m_RandomSpawn = false;

	EvaluateSpawnType(&Eval, 0);
	EvaluateSpawnType(&Eval, 1);
	EvaluateSpawnType(&Eval, 2);

	*pOutPos = Eval.m_Pos;
	return Eval.m_Got;
}

bool CGameController::CanSpawnBot(CGameWorld *pWorld, vec2 *pOutPos) const
{
	if(!pWorld || !pOutPos)
		return false;

	// Natural monsters deliberately do not use the player spawn list. Sharing
	// those points made a floor fill every spawn with a monster, so a player
	// who stepped off the train landed on one. Instead, pick a random open
	// surface tile away from players, bots, spawn points and train platforms.
	CCollision *pCollision = pWorld->Collision();
	const int Width = pCollision->GetWidth();
	const int Height = pCollision->GetHeight();
	if(Width <= 8 || Height <= 8)
		return false;

	const float MinPlayerDistance = 16.0f * 32.0f;
	for(int Attempt = 0; Attempt < 256; Attempt++)
	{
		const int X = 4 + random_int() % (Width - 8);
		for(int Y = 4; Y < Height - 2; Y++)
		{
			const vec2 P(X * 32.0f + 16.0f, Y * 32.0f + 16.0f);
			if(pCollision->CheckPoint(P))
				continue;
			if(pCollision->CheckPoint(vec2(P.x, P.y - 32.0f)))
				continue;
			if(!pCollision->CheckPoint(vec2(P.x, P.y + 32.0f)))
				continue;
			if(pCollision->TestBox(P, vec2(48.0f, 48.0f), CCollision::COLFLAG_TRAIN_ENTRANCE))
				break;

			bool TooClose = false;
			for(CGameWorld::TypeRange r = pWorld->DoTypeRange(CGameWorld::ENTTYPE_CHARACTER); !r.empty() && !TooClose; r.pop_front())
				TooClose = distance(P, r.front()->GetPos()) < MinPlayerDistance;
			for(CGameWorld::TypeRange r = pWorld->DoTypeRange(CGameWorld::ENTTYPE_BOTENTITY); !r.empty() && !TooClose; r.pop_front())
				TooClose = distance(P, r.front()->GetPos()) < 4.0f * 32.0f;
			// keep the player spawn points themselves clear
			for(int t = 0; t < 3 && !TooClose; t++)
				for(int s = 0; s < pWorld->m_alSpawnPoints[t].size() && !TooClose; s++)
					TooClose = distance(P, pWorld->m_alSpawnPoints[t][s]) < 2.0f * 32.0f;
			if(TooClose)
				break;

			*pOutPos = P;
			return true;
		}
	}
	return false;
}

float CGameController::EvaluateSpawnPos(CSpawnEval *pEval, vec2 Pos) const
{
	// A small random tie-breaker keeps a quiet spawn area from always choosing
	// the same point while the danger terms still dominate.
	float Score = random_float() * 0.001f;

	for(CGameWorld::TypeRange r = pEval->m_pWorld->DoTypeRange(CGameWorld::ENTTYPE_PROJECTILE); !r.empty(); r.pop_front())
	{
		CProjectile *pProj = static_cast<CProjectile *>(r.front());
		// team mates are not as dangerous as enemies
		float Scoremod = 1.0f;
		if(pEval->m_FriendlyTeam != -1 && pProj->GetOwner() >= 0 && GameServer()->m_apPlayers[pProj->GetOwner()] &&
			GameServer()->m_apPlayers[pProj->GetOwner()]->GetTeam() == pEval->m_FriendlyTeam)
			Scoremod = 0.5f;

		// GetPos() takes an age in seconds; 0 is the launch point, which is
		// enough to keep a spawn from being chosen inside a fresh volley.
		float d = distance(Pos, pProj->GetPos(0.0f));
		Score += Scoremod * (d == 0 ? 1000000000.0f : 1.0f / d);
	}

	// A monster sitting on a spawn point is the immediate danger here.
	for(CGameWorld::TypeRange r = pEval->m_pWorld->DoTypeRange(CGameWorld::ENTTYPE_BOTENTITY); !r.empty(); r.pop_front())
	{
		float d = distance(Pos, r.front()->GetPos());
		Score += d == 0 ? 1000000000.0f : 1.0f / d;
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
		// bots occupy a spawn point exactly like a character does, so they must
		// be part of the occupancy test too
		Num += pEval->m_pWorld->FindEntities(pEval->m_pWorld->m_alSpawnPoints[Type][i], 64, lpEnts, CGameWorld::ENTTYPE_BOTENTITY);
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

bool CGameController::IsUsingBot() const
{
	return Config()->m_SvBots > 0;
}

// team
bool CGameController::CanChangeTeam(CPlayer *pPlayer, int JoinTeam) const
{
	// Guests may join the game with a nickname-derived profile. Logging in is
	// optional and only upgrades the profile to a permanent account.
	(void) pPlayer;
	(void) JoinTeam;
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
	if(!pChr)
		return false;

	CGameWorld *pWorld = pChr->GameWorld();

	// A ring instance has no edge of its own: past the trigger the character
	// steps into the neighbouring instance. The trigger is a half plane, so
	// testing the destination is enough, and the arrival lands on the inner
	// third of the strip, inside the other trigger, so it cannot bounce.
	if(pWorld->IsRing())
	{
		int Direction = 0;
		if(NewPos.x >= pWorld->RingRightTriggerX())
			Direction = 1;
		else if(NewPos.x <= pWorld->RingLeftTriggerX())
			Direction = -1;
		if(Direction != 0)
		{
			char aTarget[64];
			float ArrivalX = 0.0f;
			if(pWorld->RingNeighbour(Direction, aTarget, sizeof(aTarget), NewPos.x, &ArrivalX))
			{
				dbg_msg("game", "ring crossing %s -> %s at x %.0f", pWorld->m_aMapName, aTarget, NewPos.x);
				pChr->GameServer()->Server()->RegisterMapName(aTarget);
				bool Deferred = false;
				if(!pChr->GameServer()->SwitchPlayerWorld(pChr->GetPlayer(), str_quickhash(aTarget), vec2(ArrivalX, NewPos.y), true, false, &Deferred))
				{
					// The neighbour is still being generated. Keep the character
					// on this side of the seam; the next tick retries once the
					// map is ready. This is rare because the game layer keeps a
					// prefetch window around the occupied instance.
					if(Deferred)
					{
						pChr->TeleTo(LastPos, true);
						return true;
					}
					return false;
				}
				return true;
			}
		}
	}

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

		// On a cylinder LastPos and NewPos can be the same place on opposite
		// sides of the seam, so the straight line between them would be the
		// length of the world and sweep through every entrance in it. The delta
		// is therefore taken the short way round.
		vec2 Delta = NewPos - LastPos;
		if(pWorld->Collision()->WrapsX())
		{
			const float W = (float) pWorld->Collision()->GetWidth() * 32.0f;
			if(W > 0.0f)
			{
				while(Delta.x > W * 0.5f)
					Delta.x -= W;
				while(Delta.x < -W * 0.5f)
					Delta.x += W;
			}
		}

		const float Distance = length(Delta);
		const int Steps = maximum(1, (int) Distance);
		// sample the whole path from LastPos to NewPos (every step is less
		// than one tile, so no tile of the path can be skipped)
		const vec2 Step = Delta / (float) Steps;
		for(int i = 0; i <= Steps; i++)
		{
			if(FindEntrance(&EntranceIndex))
				break;
			Pos = Pos + Step;
		}
		if(EntranceIndex >= 0)
		{
			const char *pTarget = pWorld->m_lEntrances[EntranceIndex].m_aTargetMap;
			pChr->GameServer()->Server()->RegisterMapName(pTarget);
			bool Deferred = false;
			if(!pChr->GameServer()->SwitchPlayerWorld(pChr->GetPlayer(), str_quickhash(pTarget), vec2(0.0f, 0.0f), false, false, &Deferred))
			{
				// The target world is still being generated. Put the character
				// back where the movement started; the entrance is retried on a
				// later tick when the world is ready.
				if(Deferred)
				{
					pChr->TeleTo(LastPos, true);
					return true;
				}
				return false;
			}
			return true;
		}
		return false;
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
	CDatabase *pDB = pSelf->GameServer()->Database();

	if(!pPlayer)
		return;
	if(!pDB)
	{
		pSelf->SendSystemChat(ClientID, "Account", "The database is not available.");
		return;
	}
	if(pResult->NumArguments() < 2)
	{
		pSelf->SendSystemChat(ClientID, "Account", "Usage: /register <username> <password>");
		return;
	}
	if(pPlayer->m_LoggedIn)
	{
		pSelf->SendSystemChat(ClientID, "Account", "You are already logged in.");
		return;
	}

	const char *pUsername = pResult->GetString(0);
	const char *pPassword = pResult->GetString(1);

	CDatabase::SPlayerData Row;
	if(pDB->FindByName(pUsername, Row))
	{
		pSelf->SendSystemChat(ClientID, "Account", "This username is already taken.");
		return;
	}

	mem_zero(&Row, sizeof(Row));
	// 0.8: time_uuid() was replaced by random_uuid() from <base/uuid.h>
	Row.m_Uuid = random_uuid();
	str_copy(Row.m_aUsername, pUsername, sizeof(Row.m_aUsername));
	HashPassword(Row.m_aPasswordHash, sizeof(Row.m_aPasswordHash), pPassword);

	if(!pDB->InsertPlayer(Row))
	{
		pSelf->SendSystemChat(ClientID, "Account", "Failed to create the account.");
		return;
	}

	const bool WasGuest = pPlayer->m_Guest;
	// capture the guest identity before the account uuid replaces it: currency
	// lives on the account ledger, so it has to be moved explicitly
	const Uuid GuestAccount = pPlayer->m_AccountUuid;
	pPlayer->m_AccountUuid = Row.m_Uuid;
	pPlayer->m_LoggedIn = true;
	pPlayer->m_Guest = false;
	pSelf->GameServer()->EnsureNpcAggroFor(pPlayer);
	pSelf->GameServer()->Trade()->RebindSeller(ClientID, pPlayer->m_AccountUuid);
	if(pPlayer->GetTeam() == TEAM_SPECTATORS)
		pSelf->DoTeamChange(pPlayer, TEAM_RED, false);

	// A guest upgrading to an account keeps the inventory it already earned;
	// a completely fresh account gets the starting kit.
	if(!WasGuest)
		pSelf->GameServer()->GiveStartingKit(ClientID);
	else if(GuestAccount != UUID_ZEROED)
	{
		// The inventory stays because it is not reloaded here, but currency is
		// account scoped: move the guest's ledger balance onto the new account
		// so registering does not silently empty the player's wallet.
		int64 Balance = 0;
		if(pDB->GetCurrencyBalance(GuestAccount, &Balance) && Balance > 0)
		{
			char aGuest[UUID_MAXSTRSIZE];
			char aFromReason[CURRENCY_REASON_SIZE];
			char aToReason[CURRENCY_REASON_SIZE];
			format_uuid(GuestAccount, aGuest, sizeof(aGuest));
			str_format(aFromReason, sizeof(aFromReason), "account upgrade: guest balance moved to the new account");
			str_format(aToReason, sizeof(aToReason), "account upgrade: balance moved from guest %s", aGuest);
			if(!pDB->TransferCurrency(GuestAccount, Row.m_Uuid, Balance, aFromReason, aToReason))
				dbg_msg("currency", "failed to move guest balance %lld for client %d", (long long) Balance, ClientID);
		}
	}

	pPlayer->SaveStatus(pDB); // persist the initial status fields
	pSelf->SendSystemChat(ClientID, "Account", "Account created. You are now logged in.");
}

void CGameController::ComLogin(IConsole::IResult *pResult, void *pContext)
{
	CCommandManager::SCommandContext *pCmdContext = static_cast<CCommandManager::SCommandContext *>(pContext);
	CGameController *pSelf = static_cast<CGameController *>(pCmdContext->m_pContext);
	int ClientID = pCmdContext->m_ClientID;
	CPlayer *pPlayer = pSelf->GameServer()->m_apPlayers[ClientID];
	CDatabase *pDB = pSelf->GameServer()->Database();

	if(!pPlayer)
		return;
	if(!pDB)
	{
		pSelf->SendSystemChat(ClientID, "Account", "The database is not available.");
		return;
	}
	if(pResult->NumArguments() < 2)
	{
		pSelf->SendSystemChat(ClientID, "Account", "Usage: /login <username> <password>");
		return;
	}
	if(pPlayer->m_LoggedIn)
	{
		pSelf->SendSystemChat(ClientID, "Account", "You are already logged in.");
		return;
	}

	const char *pUsername = pResult->GetString(0);
	const char *pPassword = pResult->GetString(1);

	CDatabase::SPlayerData Row;
	if(!pDB->FindByName(pUsername, Row))
	{
		pSelf->SendSystemChat(ClientID, "Account", "No such account.");
		return;
	}
	if(!VerifyPassword(Row.m_aPasswordHash, pPassword))
	{
		pSelf->SendSystemChat(ClientID, "Account", "Wrong password.");
		return;
	}

	pPlayer->m_AccountUuid = Row.m_Uuid;
	pPlayer->m_LoggedIn = true;
	pPlayer->m_Guest = false;
	pSelf->GameServer()->EnsureNpcAggroFor(pPlayer);
	pSelf->GameServer()->Trade()->RebindSeller(ClientID, pPlayer->m_AccountUuid);
	if(pPlayer->GetTeam() == TEAM_SPECTATORS)
		pSelf->DoTeamChange(pPlayer, TEAM_RED, false);
	pPlayer->TryRespawn();
	pPlayer->LoadStatus(pDB);
	pPlayer->SaveNickname(pDB);
	pSelf->SendSystemChat(ClientID, "Account", "Logged in.");
}

void CGameController::ComLanguage(IConsole::IResult *pResult, void *pContext)
{
	CCommandManager::SCommandContext *pCmdContext = static_cast<CCommandManager::SCommandContext *>(pContext);
	CGameController *pSelf = static_cast<CGameController *>(pCmdContext->m_pContext);
	const int ClientID = pCmdContext->m_ClientID;
	CPlayer *pPlayer = pSelf->GameServer()->m_apPlayers[ClientID];

	if(!pPlayer)
		return;

	if(pResult->NumArguments() == 0)
	{
		pSelf->SendSystemChat(ClientID, "Language", "Available languages:");
		for(int i = 0; i < g_Localization.NumLanguages(); i++)
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "%s%s (%s)", i == pPlayer->Language() ? "* " : "  ",
				g_Localization.LanguageName(i), g_Localization.LanguageCode(i));
			pSelf->SendSystemChat(ClientID, aBuf);
		}
		pSelf->SendSystemChat(ClientID, "Language", "Usage: /lang <code>");
		return;
	}

	if(!pPlayer->SetLanguage(pResult->GetString(0)))
	{
		pSelf->SendSystemChat(ClientID, "Language", "Unknown language. Use /lang to list available languages.");
		return;
	}

	pSelf->GameServer()->SavePlayerData(pPlayer);

	CLanguageScope LanguageScope(pPlayer->Language());
	const std::string Msg = LocalizeFormat("Language", "Language set to {}.", g_Localization.LanguageName(pPlayer->Language()));
	pSelf->GameServer()->SendChat(-1, CHAT_ALL, ClientID, Msg.c_str());
}

void CGameController::ComContainer(IConsole::IResult *pResult, void *pContext)
{
	(void) pResult;
	CCommandManager::SCommandContext *pCmdContext = static_cast<CCommandManager::SCommandContext *>(pContext);
	CGameController *pSelf = static_cast<CGameController *>(pCmdContext->m_pContext);
	const int ClientID = pCmdContext->m_ClientID;

	// both this command and the main menu entry resolve their target the same
	// way, so there is exactly one definition of "the container I am using"
	if(!pSelf->GameServer()->AimedContainer(ClientID))
	{
		pSelf->SendSystemChat(ClientID, "Container", "Aim at a container first.");
		return;
	}
	pSelf->GameServer()->GameMenu()->SetPlayerPage(ClientID, "CONTAINER");
}

void CGameController::ComTeam(IConsole::IResult *pResult, void *pContext)
{
	CCommandManager::SCommandContext *pCmdContext = static_cast<CCommandManager::SCommandContext *>(pContext);
	CGameController *pSelf = static_cast<CGameController *>(pCmdContext->m_pContext);
	const int ClientID = pCmdContext->m_ClientID;
	CGameContext *pGame = pSelf->GameServer();
	CPlayer *pPlayer = pGame->m_apPlayers[ClientID];
	CTeamSystem *pTeams = pGame->Teams();
	if(!pPlayer || !pTeams)
		return;

	const char *pArgs = pResult->NumArguments() > 0 ? pResult->GetString(0) : "";
	if(!pArgs[0])
	{
		pGame->GameMenu()->SetPlayerPage(ClientID, "TEAM");
		return;
	}

	char aError[128] = "";
	if(str_startswith(pArgs, "create "))
	{
		const char *pName = str_skip_whitespaces_const(pArgs + str_length("create "));
		if(pTeams->CreateTeam(pPlayer->m_AccountUuid, pName, aError, sizeof(aError)))
		{
			pSelf->SendSystemChat(ClientID, "Team", "Team created.");
			pGame->GameMenu()->SetPlayerPage(ClientID, "TEAM");
		}
		else
			pSelf->SendSystemChat(ClientID, "Team", aError);
	}
	else if(str_startswith(pArgs, "invite "))
	{
		const char *pName = str_skip_whitespaces_const(pArgs + str_length("invite "));
		int TargetClientID = -1;
		int Matches = 0;
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!pGame->Server()->ClientIngame(i) || str_comp_nocase(pGame->Server()->ClientName(i), pName) != 0)
				continue;
			TargetClientID = i;
			Matches++;
		}
		if(TargetClientID < 0)
			pSelf->SendSystemChat(ClientID, "Team", "No online player has that name.");
		else if(Matches > 1)
			pSelf->SendSystemChat(ClientID, "Team", "Several players have that name. Use the Team menu to pick one.");
		else if(pTeams->InviteMember(pPlayer->m_AccountUuid, pGame->m_apPlayers[TargetClientID]->m_AccountUuid, aError, sizeof(aError)))
			pSelf->SendSystemChat(ClientID, "Team", "Invitation sent.");
		else
			pSelf->SendSystemChat(ClientID, "Team", aError);
	}
	else if(str_comp(pArgs, "accept") == 0)
	{
		if(pTeams->AcceptInvite(pPlayer->m_AccountUuid, aError, sizeof(aError)))
			pSelf->SendSystemChat(ClientID, "Team", "Joined the team.");
		else
			pSelf->SendSystemChat(ClientID, "Team", aError);
	}
	else if(str_comp(pArgs, "decline") == 0)
	{
		if(pTeams->DeclineInvite(pPlayer->m_AccountUuid, aError, sizeof(aError)))
			pSelf->SendSystemChat(ClientID, "Team", "Invitation declined.");
		else
			pSelf->SendSystemChat(ClientID, "Team", aError);
	}
	else if(str_comp(pArgs, "leave") == 0)
	{
		const bool WasLeader = pTeams->IsLeader(pPlayer->m_AccountUuid);
		if(pTeams->LeaveTeam(pPlayer->m_AccountUuid, aError, sizeof(aError)))
			pSelf->SendSystemChat(ClientID, "Team", WasLeader ? "The team was disbanded because the leader left." : "Left the team.");
		else
			pSelf->SendSystemChat(ClientID, "Team", aError);
	}
	else if(str_comp(pArgs, "disband") == 0)
	{
		if(pTeams->DisbandTeam(pPlayer->m_AccountUuid, aError, sizeof(aError)))
			pSelf->SendSystemChat(ClientID, "Team", "Team disbanded.");
		else
			pSelf->SendSystemChat(ClientID, "Team", aError);
	}
	else if(str_comp(pArgs, "ff on") == 0 || str_comp(pArgs, "ff off") == 0)
	{
		const bool Enabled = str_comp(pArgs, "ff on") == 0;
		if(pTeams->SetFriendlyFire(pPlayer->m_AccountUuid, Enabled, aError, sizeof(aError)))
			pSelf->SendSystemChat(ClientID, "Team", Enabled ? "Friendly fire enabled." : "Friendly fire disabled.");
		else
			pSelf->SendSystemChat(ClientID, "Team", aError);
	}
	else
	{
		pSelf->SendSystemChat(ClientID, "Team", "Usage: /team create <name>, /team invite <player>, /team accept, /team decline, /team leave, /team disband, /team ff on|off");
		pGame->GameMenu()->SetPlayerPage(ClientID, "TEAM");
	}
}

void CGameController::ComTeamChat(IConsole::IResult *pResult, void *pContext)
{
	CCommandManager::SCommandContext *pCmdContext = static_cast<CCommandManager::SCommandContext *>(pContext);
	CGameController *pSelf = static_cast<CGameController *>(pCmdContext->m_pContext);
	const int ClientID = pCmdContext->m_ClientID;
	CGameContext *pGame = pSelf->GameServer();
	CPlayer *pPlayer = pGame->m_apPlayers[ClientID];
	CTeamSystem *pTeams = pGame->Teams();
	if(!pPlayer || !pTeams)
		return;
	const SGameTeam *pTeam = pTeams->TeamFor(pPlayer->m_AccountUuid);
	if(!pTeam)
	{
		pSelf->SendSystemChat(ClientID, "Team", "You are not in a team.");
		return;
	}

	const char *pText = pResult->NumArguments() > 0 ? pResult->GetString(0) : "";
	if(!pText[0])
		return;

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "[%s] %s: %s", Localize("Team", "Team"), pGame->Server()->ClientName(ClientID), pText);
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pMember = pGame->m_apPlayers[i];
		if(pMember && pTeams->TeamFor(pMember->m_AccountUuid) == pTeam)
			pGame->SendChat(-1, CHAT_ALL, i, aBuf);
	}
}

void CGameController::ComHelp(IConsole::IResult *pResult, void *pContext)
{
	(void) pResult;
	CCommandManager::SCommandContext *pCmdContext = static_cast<CCommandManager::SCommandContext *>(pContext);
	CGameController *pSelf = static_cast<CGameController *>(pCmdContext->m_pContext);
	const int ClientID = pCmdContext->m_ClientID;

	pSelf->SendSystemChat(ClientID, "Menu Help", "Move with A and D, jump with space, and use the mouse to hook and shoot.");
	pSelf->SendSystemChat(ClientID, "Menu Help", "Type /register <username> <password> to keep your progress; /login restores it.");
	pSelf->SendSystemChat(ClientID, "Menu Help", "Open the vote menu for Inventory (equipment is inside it), Craft, Shop, Team and the New Player Guide.");
	pSelf->GameServer()->GameMenu()->SetPlayerPage(ClientID, "HELP");
}

void CGameController::RegisterChatCommands(CCommandManager *pManager)
{
	pManager->AddCommand("about", "About the mod", "", Com_About, this);
	pManager->AddCommand("info", "About the mod", "", Com_About, this);
	pManager->AddCommand("register", "Register a new account", "s[username] s[password]", ComRegister, this);
	pManager->AddCommand("login", "Log in to an account", "s[username] s[password]", ComLogin, this);
	pManager->AddCommand("lang", "Set your language", "?s[code]", ComLanguage, this);
	pManager->AddCommand("container", "Open the container you are aiming at", "", ComContainer, this);
	pManager->AddCommand("help", "New player guide", "", ComHelp, this);
	pManager->AddCommand("team", "Team menu / create / invite / accept / decline / leave / disband / friendly fire", "?r[args]", ComTeam, this);
	pManager->AddCommand("t", "Team chat", "r[text]", ComTeamChat, this);
}

void CGameController::SendSystemChat(int TargetID, const char *pMsg)
{
	GameServer()->SendChat(-1, CHAT_ALL, TargetID, pMsg);
}

void CGameController::SendSystemChat(int TargetID, const char *pContext, const char *pText)
{
	CLanguageScope LanguageScope(GameServer()->LanguageOf(TargetID));
	GameServer()->SendChat(-1, CHAT_ALL, TargetID, Localize(pText, pContext));
}
