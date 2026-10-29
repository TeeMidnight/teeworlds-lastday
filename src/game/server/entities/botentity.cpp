/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
/* Bot layer ported from teeworlds-carbon (c) TeeMidnight, zlib license.                     */
#include <engine/shared/config.h>
#include <game/server/botmanager.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>
#include <game/server/scripting/scripted_bot_ai.h>
#include <game/server/weapon.h>
#include <game/server/weaponmanager.h>
#include <generated/server_data.h>

#include "botentity.h"

// Standard 0.8 tee skins, so a bot looks like a regular tee. Local to the bot
// layer: the client resolves the names from its own skin database.
static const CBotSkin gs_aBotSkins[] = {
	{{"standard", "", "", "standard", "standard", "standard"}, {1, 0, 0, 1, 1, 0}, {1798004, 0, 0, 1799582, 1869630, 0}},
	{{"kitty", "whisker", "", "standard", "standard", "negative"}, {1, 1, 0, 1, 1, 1}, {8681144, -8229413, 0, 7885547, 8868585, 9043712}},
	{{"standard", "stripes", "", "standard", "standard", "standard"}, {1, 0, 0, 1, 1, 0}, {10187898, 0, 0, 750848, 1944919, 0}},
	{{"bear", "bear", "hair", "standard", "standard", "standard"}, {1, 1, 0, 1, 1, 0}, {1082745, -15634776, 0, 1082745, 1147174, 0}},
	{{"standard", "cammo2", "", "standard", "standard", "standard"}, {1, 1, 0, 1, 1, 0}, {5334342, -11771603, 0, 750848, 1944919, 0}},
	{{"standard", "cammostripes", "", "standard", "standard", "standard"}, {1, 1, 0, 1, 1, 0}, {5334342, -14840320, 0, 750848, 1944919, 0}},
	{{"koala", "twinbelly", "", "standard", "standard", "standard"}, {1, 1, 0, 1, 1, 0}, {184, -15397662, 0, 184, 9765959, 0}},
	{{"standard", "whisker", "", "standard", "standard", "standard"}, {1, 1, 0, 1, 1, 0}, {15911355, -801066, 0, 15043034, 15043034, 0}},
	{{"standard", "stripe", "", "standard", "standard", "standard"}, {1, 0, 0, 1, 1, 0}, {16307835, 0, 0, 184, 9765959, 0}},
	{{"standard", "saddo", "", "standard", "standard", "standard"}, {1, 1, 0, 1, 1, 0}, {7171455, -9685436, 0, 3640746, 5792119, 0}},
	{{"standard", "toptri", "", "standard", "standard", "standard"}, {1, 0, 0, 1, 1, 0}, {6119331, 0, 0, 3640746, 5792119, 0}},
	{{"standard", "twintri", "", "standard", "standard", "standard"}, {1, 1, 0, 1, 1, 0}, {3447932, -14098717, 0, 185, 9634888, 0}},
};

CBotTeeInfo BotMakeRandomTeeInfo(const char *pName)
{
	CBotTeeInfo Info;
	mem_zero(&Info, sizeof(Info));
	Info.m_Skin = gs_aBotSkins[random_int() % (int) (sizeof(gs_aBotSkins) / sizeof(gs_aBotSkins[0]))];
	str_copy(Info.m_aName, pName, sizeof(Info.m_aName));
	str_copy(Info.m_aClan, "", sizeof(Info.m_aClan));
	return Info;
}

int CBotEntity::PickTeeID(CGameWorld *pGameWorld, Uuid BotID)
{
	// The manager hands the id out and keeps it globally unique across every
	// world, so a snapshot's server-internal tee id names exactly one bot.
	CBotManager *pBots = pGameWorld->GameServer()->BotManager();
	return pBots ? pBots->PickTeeID(BotID) : -1;
}

CBotEntity::CBotEntity(CGameWorld *pWorld, vec2 Pos, Uuid BotID, const CBotTeeInfo &TeeInfo, int Team, int Faction) :
	IBotEntity(pWorld, CGameWorld::ENTTYPE_BOTENTITY, Pos, ms_PhysSize)
{
	m_BotID = BotID;
	m_TeeID = PickTeeID(pWorld, BotID);
	m_TeeInfo = TeeInfo;
	m_Team = Team;
	m_Faction = Faction;
	m_Natural = false;
	m_Alive = true;

	m_pAI = nullptr;
	m_aPendingAI[0] = 0;
	m_bPendingAI = false;

	m_MaxHealth = MAX_HEALTH;
	m_MaxArmor = MAX_ARMOR;
	m_Health = MAX_HEALTH;
	m_Armor = 0;

	m_Emote = EMOTE_NORMAL;
	m_EmoteStop = -1;
	m_AttackTick = 0;
	m_TriggeredEvents = 0;

	// bots have no item system, so they hold the one weapon that needs
	// neither an item nor ammo: the hammer (loadout slot 0)
	m_WeaponSlot = WEAPON_HAMMER;
	// hammer defaults; a content variant may raise these through
	// SetMeleeTuning() without touching the weapon table
	m_MeleeDamage = g_pData->m_Weapons.m_aId[WEAPON_HAMMER].m_Damage;
	m_MeleeCooldown = Server()->TickSpeed() / 3;
	m_MeleeRange = GetProximityRadius();

	m_Core.Reset();
	m_Core.Init(&GameWorld()->m_Core, GameWorld()->Collision());
	m_Core.m_Pos = m_Pos;
	m_Tuning = *GameServer()->Tuning();
	m_Core.SetTuning(&m_Tuning);

	// Bind the core to our TeeID, so characters collide with us and can hook us.
	// The id is unique among live bots, which is what makes that table safe.
	if(m_TeeID != -1)
		GameWorld()->m_Core.SetCharacter(m_TeeID, &m_Core);

	m_CursorTarget = vec2(100.0f, 0.0f);

	m_ReckoningTick = 0;
	mem_zero(&m_SendCore, sizeof(m_SendCore));
	mem_zero(&m_ReckoningCore, sizeof(m_ReckoningCore));
	mem_zero(&m_Input, sizeof(m_Input));
	mem_zero(&m_PrevInput, sizeof(m_PrevInput));

	// the default bot keeps the historical chase behaviour; the manager may
	// replace it with another strategy right after construction
	SetAIByName(BotAIDefaultName());

	GameWorld()->InsertEntity(this);
}

void CBotEntity::SetEmote(int Emote, int Tick)
{
	m_Emote = Emote;
	m_EmoteStop = Tick;
}

CBotEntity::~CBotEntity()
{
	SetAI(nullptr);

	// Unlink from the manager first. Die(), ClearBots() and the worm's
	// Despawn() retire a bot before deleting it, but a bot can also be
	// destroyed by a path that never retired it: a world torn down with it
	// still listed, a segment whose Despawn was missed, a script. A stale
	// entry is followed later by ClearBots(), which calls TeeID() on it, and
	// that is a use after free. ForgetBot() is idempotent, so the normal
	// paths only pay a hash lookup.
	//
	// Only forget the id while it still names this object: a fixed spawn is
	// respawned under the same BotID, and its replacement may already be
	// registered when the old entity is finally deleted.
	if(CBotManager *pBots = CBotManager::Active())
	{
		if(pBots->BotByID(m_BotID) == this)
			pBots->ForgetBot(m_BotID);
	}

	// Unbind before the core dies, or a physics tick would read a dangling
	// pointer out of the world's table. Only touch the slot while it still
	// points at this bot: OnBotDeath frees the TeeID before the entity is
	// deleted, and a replacement bot may already have taken it.
	if(m_TeeID != -1 && GameWorld() && GameWorld()->m_Core.GetCharacter(m_TeeID) == &m_Core)
		GameWorld()->m_Core.SetCharacter(m_TeeID, 0);
}

void CBotEntity::MoveToWorld(CGameWorld *pNewWorld, vec2 Pos)
{
	if(!pNewWorld || pNewWorld == GameWorld())
		return;

	CGameWorld *pOldWorld = GameWorld();

	// release from the old world: manager maps, core binding, entity list
	if(pOldWorld->HasBotManager())
		pOldWorld->GetBotManager()->ReleaseBot(m_BotID);
	if(m_TeeID != -1)
		pOldWorld->m_Core.SetCharacter(m_TeeID, 0);
	pOldWorld->RemoveEntity(this);

	// Reparent and bind to the new world's core. The private tee id is global
	// and unique among live bots, so it is already free in the target core and
	// is kept: a hook that already names this bot keeps working across the
	// transfer (the client-facing id is derived from the BotID separately).
	SetGameWorld(pNewWorld);
	m_Core.Init(&pNewWorld->m_Core, pNewWorld->Collision());
	m_Core.m_Pos = Pos;
	m_Core.m_Vel = vec2(0.0f, 0.0f);
	m_Pos = Pos;
	UpdateTuning();
	if(m_TeeID != -1)
		pNewWorld->m_Core.SetCharacter(m_TeeID, &m_Core);

	pNewWorld->InsertEntity(this);
	pNewWorld->BotManager()->AdoptBot(this);
}

void CBotEntity::Reset()
{
	Destroy();
}

void CBotEntity::UpdateTuning()
{
	const bool InWater = GameWorld()->Collision()->TestBox(m_Core.m_Pos,
		vec2(CCharacterCore::PHYS_SIZE, CCharacterCore::PHYS_SIZE / 2), CCollision::COLFLAG_WATER);
	m_Tuning = BuildCharacterTuning(*GameServer()->Tuning(), m_Core.m_SpeedScale, m_Core.m_SilentStep,
		InWater, m_Core.m_Input.m_Jump != 0, m_Core.m_Vel.y);
	m_Core.SetTuning(&m_Tuning);
}

void CBotEntity::Tick()
{
	OnBotTick();

	m_Core.m_Input = m_Input;
	UpdateTuning();
	m_Core.Tick(true, true);

	// handle leaving gamelayer
	if(GameLayerClipped(m_Pos))
	{
		Die(0, WEAPON_WORLD);
		return;
	}

	DoWeapon();
}

void CBotEntity::OnBotTick()
{
	// manage the emote here instead of in the snap function, so a pain or
	// NPC emote expires on the same schedule a player character uses
	if(m_EmoteStop > -1 && m_EmoteStop < Server()->Tick())
		SetEmote(EMOTE_NORMAL, -1);

	mem_copy(&m_PrevInput, &m_Input, sizeof(m_Input));
	// reset the edge-triggered inputs; the action below may set them again
	m_Input.m_Jump = 0;
	m_Input.m_Fire = 0;

	// per-tick strategy hook (scripted AIs run their on_tick here)
	if(m_pAI)
		m_pAI->OnTick(this);

	OnAction();

	// A script may have asked for another strategy from inside a hook. Apply it
	// only now: deleting the strategy that is currently running would be a
	// use-after-free.
	ApplyPendingAI();
}

void CBotEntity::OnAction()
{
	// The default body delegates to the attached strategy. Variants (the NPC)
	// override OnAction and never reach this.
	if(m_pAI)
		m_pAI->OnAction(this);
}

void CBotEntity::SetAI(IBotAI *pAI)
{
	if(m_pAI == pAI)
		return;
	delete m_pAI;
	m_pAI = pAI;
}

bool CBotEntity::SetAIByName(const char *pName)
{
	IBotAI *pAI = CreateBotAI(pName);
	if(!pAI)
		return false;
	SetAI(pAI);
	return true;
}

bool CBotEntity::RequestAI(const char *pName)
{
	if(!pName || !pName[0])
		return false;
	str_copy(m_aPendingAI, pName, sizeof(m_aPendingAI));
	m_bPendingAI = true;
	return true;
}

void CBotEntity::ApplyPendingAI()
{
	if(!m_bPendingAI)
		return;
	m_bPendingAI = false;

	char aName[sizeof(m_aPendingAI)];
	str_copy(aName, m_aPendingAI, sizeof(aName));
	if(!SetAIByName(aName))
		dbg_msg("bot", "unknown bot AI '%s', keeping '%s'", aName, AIName());
}

const char *CBotEntity::AIName() const
{
	return m_pAI ? m_pAI->Name() : "none";
}

void CBotEntity::AimCursor()
{
	// Move the cursor towards the target at a limited speed so the aim looks
	// human instead of snapping instantly. The step is the smaller of the
	// mouse speed and the remaining distance: consuming the whole remaining
	// distance once the target is nearer than one step stops the integer
	// cursor from freezing a few units short of the aim point. The old code
	// skipped the update entirely in that case and also re-projected the
	// cursor to the target's radius, which made short moves jump.
	vec2 Target = vec2(m_Input.m_TargetX, m_Input.m_TargetY);
	const vec2 Delta = m_CursorTarget - Target;
	const float Remaining = length(Delta);
	if(Remaining > 0.0f)
	{
		const float MouseSpeed = random_float() * 32.0f + 8.0f;
		const float Step = minimum(MouseSpeed, Remaining);
		Target += Delta * (Step / Remaining);

		// keep the cursor inside the maximum aim radius
		if(length(Target) > 400.0f)
			Target = normalize(Target) * 400.0f;

		m_Input.m_TargetX = round_to_int(Target.x);
		m_Input.m_TargetY = round_to_int(Target.y);
	}
}

CEntity *CBotEntity::FindTarget()
{
	// a peaceful NPC never acquires a target: it only wanders
	if(GameWorld()->IsNpcWorld())
		return nullptr;

	// chase the closest hittable entity that is not us and is in line of sight
	CEntity *pTarget = GameWorld()->ClosestFlagEntity(GetPos(), 320.0f, CGameWorld::ENTFLAG_HITABLE, this);
	if(pTarget && GameWorld()->Collision()->IntersectLine(GetPos(), GameWorld()->PosInFrame(pTarget), NULL, NULL))
		pTarget = 0;
	return pTarget;
}

int CBotEntity::ReloadTimer()
{
	return m_ReloadTimer.remaining(Server()->Tick());
}

void CBotEntity::DoWeapon()
{
	if(m_ReloadTimer.active(Server()->Tick()))
		return;

	// edge detection: only fire on a fresh press, like a character does
	CInputCount Count = CountInput(m_PrevInput.m_Fire, m_Input.m_Fire);
	if(!Count.m_Presses && !(m_Input.m_Fire & 1))
		return;

	const vec2 Direction = normalize(vec2(m_Input.m_TargetX, m_Input.m_TargetY));

	// The melee implementation is written against CCharacter, so the swing is
	// performed here against the bot's own position rather than through the
	// weapon manager: a bot has no CPlayer, no inventory and no ammo.
	const vec2 ProjStartPos = m_Pos + Direction * GetProximityRadius() * 0.75f;
	GameWorld()->CreateSound(m_Pos, SOUND_HAMMER_FIRE);
	GameWorld()->CreateHammerHit(ProjStartPos);

	const int Dmg = m_MeleeDamage;

	int Hits = 0;
	array<CEntity *> lpEnts;
	lpEnts.hint_size(8);
	const int Num = GameWorld()->FindFlagEntities(ProjStartPos, m_MeleeRange * 0.5f, lpEnts, CGameWorld::ENTFLAG_HITABLE);
	for(int i = 0; i < Num; ++i)
	{
		CHitableEntity *pTarget = static_cast<CHitableEntity *>(lpEnts[i]);
		if(pTarget == this)
			continue;
		if(GameWorld()->Collision()->IntersectLine(ProjStartPos, GameWorld()->PosInFrame(pTarget), NULL, NULL))
			continue;

		vec2 Dir;
		if(length(GameWorld()->PosInFrame(pTarget) - m_Pos) > 0.0f)
			Dir = normalize(GameWorld()->PosInFrame(pTarget) - m_Pos);
		else
			Dir = vec2(0.0f, -1.0f);

		// remember whether a character target was alive, so a variant can tell
		// whether this swing was the killing blow
		CCharacter *pTargetChr = pTarget->ObjType() == CGameWorld::ENTTYPE_CHARACTER ? static_cast<CCharacter *>(pTarget) : nullptr;
		const bool WasAlive = !pTargetChr || pTargetChr->IsAlive();

		pTarget->TakeHit(vec2(0.0f, -1.0f) + normalize(Dir + vec2(0.0f, -1.1f)) * 10.0f, Dir * -1, Dmg, this, WEAPON_HAMMER);
		const bool Killed = WasAlive && pTargetChr && !pTargetChr->IsAlive();
		OnBotHit(pTarget, Killed);
		Hits++;
	}

	m_AttackTick = Server()->Tick();
	// the old countdown blocked this many further ticks before the next swing
	const int Cooldown = Hits ? m_MeleeCooldown : maximum(1, m_MeleeCooldown / 2);
	m_ReloadTimer.start(Server()->Tick(), Cooldown + 1);
}

void CBotEntity::OnBotHit(CEntity *pTarget, bool Killed)
{
	if(m_pAI)
		m_pAI->OnHit(this, pTarget, Killed);
}

void CBotEntity::TickDefered()
{
	static const vec2 ColBox(CCharacterCore::PHYS_SIZE, CCharacterCore::PHYS_SIZE);

	// advance the dummy core used for dead reckoning
	{
		CWorldCore TempWorld;
		TempWorld.m_Tuning = m_Tuning;
		m_ReckoningCore.Init(&TempWorld, GameWorld()->Collision());
		m_ReckoningCore.Tick(false, false);
		m_ReckoningCore.Move();
		m_ReckoningCore.Quantize();
	}

	m_Core.AddDragVelocity();
	m_Core.ResetDragVelocity();

	vec2 StartPos = m_Core.m_Pos;
	vec2 StartVel = m_Core.m_Vel;
	bool StuckBefore = GameWorld()->Collision()->TestBox(m_Core.m_Pos, ColBox);

	m_Core.Move();

	bool StuckAfterMove = GameWorld()->Collision()->TestBox(m_Core.m_Pos, ColBox);
	m_Core.Quantize();
	bool StuckAfterQuant = GameWorld()->Collision()->TestBox(m_Core.m_Pos, ColBox);
	m_Pos = m_Core.m_Pos;

	if(!StuckBefore && (StuckAfterMove || StuckAfterQuant))
	{
		// Hackish solution to get rid of strict-aliasing warning
		union
		{
			float f;
			unsigned u;
		} StartPosX, StartPosY, StartVelX, StartVelY;

		StartPosX.f = StartPos.x;
		StartPosY.f = StartPos.y;
		StartVelX.f = StartVel.x;
		StartVelY.f = StartVel.y;

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "bot STUCK!!! %d %d %d %f %f %f %f %x %x %x %x",
			StuckBefore, StuckAfterMove, StuckAfterQuant,
			StartPos.x, StartPos.y, StartVel.x, StartVel.y,
			StartPosX.u, StartPosY.u, StartVelX.u, StartVelY.u);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}

	m_TriggeredEvents |= m_Core.m_TriggeredEvents;

	if(m_Core.m_Death)
	{
		Die(0, WEAPON_WORLD);
		return;
	}

	// update m_SendCore if needed
	{
		CNetObj_Character Predicted;
		CNetObj_Character Current;
		mem_zero(&Predicted, sizeof(Predicted));
		mem_zero(&Current, sizeof(Current));
		m_ReckoningCore.Write(&Predicted);
		m_Core.Write(&Current);

		// only allow dead reckoning for up to 3 seconds
		if(m_ReckoningTick + Server()->TickSpeed() * 3 < Server()->Tick() || mem_comp(&Predicted, &Current, sizeof(CNetObj_Character)) != 0)
		{
			m_ReckoningTick = Server()->Tick();
			m_SendCore = m_Core;
			m_ReckoningCore = m_Core;
		}
	}
}

void CBotEntity::TickPaused()
{
	++m_AttackTick;
	++m_ReckoningTick;
	if(m_EmoteStop > -1)
		++m_EmoteStop;
}

void CBotEntity::ShiftForSnap(vec2 Delta)
{
	m_Pos += Delta;
	m_Core.m_Pos += Delta;
	m_Core.m_HookPos += Delta;
	m_SendCore.m_Pos += Delta;
	m_SendCore.m_HookPos += Delta;
}

void CBotEntity::Snap(int SnappingClient)
{
	CBotManager *pBotManager = GameWorld()->BotManager();
	if(!pBotManager)
		return;

	// the tee id is per receiver: it depends on this client's protocol and on
	// which bots it is being shown
	const int TeeID = pBotManager->FindClientID(SnappingClient, GetBotID());
	if(TeeID == -1)
		return;

	if(NetworkClipped(SnappingClient))
		return;

	// The client predicts this bot; give it the bot's own tuning (speed scale,
	// silent step, water) under the tee id it knows.
	CNetObj_Tuning *pTuning = static_cast<CNetObj_Tuning *>(Server()->SnapNewItem(NETOBJTYPE_TUNING, TeeID, sizeof(CNetObj_Tuning)));
	if(pTuning)
		mem_copy(pTuning->m_aTuneParams, &m_Tuning, sizeof(pTuning->m_aTuneParams));

	CNetObj_Character *pCharacter = static_cast<CNetObj_Character *>(Server()->SnapNewItem(NETOBJTYPE_CHARACTER, TeeID, sizeof(CNetObj_Character)));
	if(!pCharacter)
		return;

	// write down the core
	if(!m_ReckoningTick)
	{
		// no dead reckoning when paused because the client doesn't know
		// how far to perform the reckoning
		pCharacter->m_Tick = 0;
		m_Core.Write(pCharacter);
	}
	else
	{
		pCharacter->m_Tick = m_ReckoningTick;
		m_SendCore.Write(pCharacter);
	}

	// The core holds the hook target as a server-internal TeeID, but the client
	// resolves m_HookedPlayer against the ids it was told. A bot is shown under
	// a per-client id, so translate it here or the client finds no such tee and
	// draws no hook. Both snap paths do this, since a bot can hook a player too.
	pCharacter->m_HookedPlayer = pBotManager->TranslateTeeIDForClient(SnappingClient, pCharacter->m_HookedPlayer);

	pCharacter->m_Emote = m_Emote;
	pCharacter->m_AmmoCount = 0;
	pCharacter->m_Health = 0;
	pCharacter->m_Armor = 0;
	pCharacter->m_MaxHealth = MAX_HEALTH;
	pCharacter->m_MaxArmor = MAX_ARMOR;
	// an NPC creeping silently does not give its jumps away to the clients
	int TriggeredEvents = m_TriggeredEvents;
	if(m_Core.m_SilentStep)
		TriggeredEvents &= ~(COREEVENTFLAG_GROUND_JUMP | COREEVENTFLAG_AIR_JUMP);
	pCharacter->m_TriggeredEvents = TriggeredEvents;
	pCharacter->m_Weapon = m_WeaponSlot;
	pCharacter->m_AttackTick = m_AttackTick;
	pCharacter->m_Direction = m_Input.m_Direction;

	// health and armor are only revealed to the tee itself (or in a demo), the
	// same rule CCharacter::Snap applies
	if(TeeID == SnappingClient || SnappingClient == -1 ||
		(!Config()->m_SvStrictSpectateMode && SnappingClient >= 0 && GameServer()->m_apPlayers[SnappingClient] &&
			TeeID == GameServer()->m_apPlayers[SnappingClient]->GetSpectatorID()))
	{
		pCharacter->m_Health = m_Health;
		pCharacter->m_Armor = m_Armor;
		pCharacter->m_AmmoCount = -1;
	}

	// The bot's identity. CPlayer::Snap writes TeeInfo for real clients; a bot
	// has no CPlayer, so it emits its own under the same mapped tee id as the
	// character above. This is what gives it a name, clan, skin and bot flag.
	CNetObj_TeeInfo *pTeeInfo = static_cast<CNetObj_TeeInfo *>(Server()->SnapNewItem(NETOBJTYPE_TEEINFO, TeeID, sizeof(CNetObj_TeeInfo)));
	if(!pTeeInfo)
		return;

	int Flags = TEEFLAG_HIDDEN_IN_BOARD;
	if(!m_Alive)
		Flags |= TEEFLAG_DEAD;
	if(SnappingClient == -1)
		Flags |= TEEFLAG_READY;
	pTeeInfo->m_Flag = Flags;
	pTeeInfo->m_LatencyAndCountry = 0;
	pTeeInfo->m_Team = m_Team;
	pTeeInfo->m_Score = 0;
	pTeeInfo->m_RaceStartTick = 0;

	str_copy_fixed(pTeeInfo->m_aName, m_TeeInfo.m_aName, sizeof(pTeeInfo->m_aName));
	str_copy_fixed(pTeeInfo->m_aClan, m_TeeInfo.m_aClan, sizeof(pTeeInfo->m_aClan));
	for(int p = 0; p < NUM_SKINPARTS; p++)
	{
		str_copy_fixed(pTeeInfo->m_aaSkinPartNames[p], m_TeeInfo.m_Skin.m_aaSkinPartNames[p], sizeof(pTeeInfo->m_aaSkinPartNames[p]));
		pTeeInfo->m_aUseCustomColors[p] = m_TeeInfo.m_Skin.m_aUseCustomColors[p];
		pTeeInfo->m_aSkinPartColors[p] = m_TeeInfo.m_Skin.m_aSkinPartColors[p];
	}
}

void CBotEntity::PostSnap()
{
	m_TriggeredEvents = 0;
}

bool CBotEntity::TakeHit(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	return TakeDamage(Force, Source, Dmg, pFrom, Weapon);
}

int CBotEntity::GetHitAttacker()
{
	// Bots have no client slot. A friendly bot uses the blue sentinel; every
	// other bot is a hostile source and uses its own sentinel, because the
	// default survival team is red and a red sentinel would be swallowed by the
	// same-team friendly-fire rule.
	if(m_Faction == BOTFACTION_FRIENDLY)
		return PLAYER_TEAM_FRIENDLY;
	return PLAYER_TEAM_HOSTILE;
}

bool CBotEntity::TakeDamage(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	if(!m_Alive)
		return false;

	// A friendly source (a companion's shot, or a campfire's embers) must not
	// hurt a non-hostile unit. Friendly and neutral bots both ignore it;
	// hostile bots still take the damage, because they are nobody's ally.
	if(m_Faction != BOTFACTION_HOSTILE)
	{
		int From = -1;
		if(pFrom)
		{
			if(pFrom->ObjFlag() & CGameWorld::ENTFLAG_CHILD)
				From = static_cast<CChildEntity *>(pFrom)->GetOwner();
			else
				From = pFrom->GetHitAttacker();
		}
		if(From == PLAYER_TEAM_FRIENDLY)
			return false;
	}

	m_Core.m_Vel += Force;

	// bots are their own team: nothing is friendly fire
	int OldHealth = m_Health, OldArmor = m_Armor;
	if(Dmg)
	{
		if(m_Armor)
		{
			if(Dmg > 1)
			{
				m_Health--;
				Dmg--;
			}

			if(Dmg > m_Armor)
			{
				Dmg -= m_Armor;
				m_Armor = 0;
			}
			else
			{
				m_Armor -= Dmg;
				Dmg = 0;
			}
		}

		m_Health -= Dmg;
	}

	// the manager owns the tee id mapping, so it addresses the event at every
	// client that can see this bot
	GameWorld()->BotManager()->CreateDamage(m_Pos, m_BotID, Source, OldHealth - m_Health, OldArmor - m_Armor, pFrom == this);

	// hit sound for the attacker
	if(pFrom && pFrom->ObjType() == CGameWorld::ENTTYPE_CHARACTER)
	{
		CCharacter *pChr = static_cast<CCharacter *>(pFrom);
		int64 Mask = CmaskOne(pChr->GetCID());
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(GameServer()->m_apPlayers[i] && (GameServer()->m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS || GameServer()->m_apPlayers[i]->m_DeadSpecMode) &&
				GameServer()->m_apPlayers[i]->GetSpectatorID() == pChr->GetCID())
				Mask |= CmaskOne(i);
		}
		GameWorld()->CreateSound(GameServer()->m_apPlayers[pChr->GetCID()]->m_ViewPos, SOUND_HIT, Mask);
	}

	if(m_Health <= 0)
	{
		Die(pFrom, Weapon);
		return false;
	}

	if(Dmg > 2)
		GameWorld()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_LONG);
	else
		GameWorld()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_SHORT);

	SetEmote(EMOTE_PAIN, Server()->Tick() + 500 * Server()->TickSpeed() / 1000);

	return OnBotDamaged(Force, Source, Dmg, pFrom, Weapon);
}

bool CBotEntity::OnBotDamaged(vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	if(m_pAI)
		return m_pAI->OnDamaged(this, Force, Source, Dmg, pFrom, Weapon);
	return true;
}

void CBotEntity::Die(CEntity *pKiller, int Weapon)
{
	if(!m_Alive)
		return;

	m_Alive = false;

	// a nice sound
	GameWorld()->CreateSound(m_Pos, SOUND_PLAYER_DIE);

	// the manager resolves the tee id per client, so it emits the death event
	// for everyone that can see this bot
	GameWorld()->BotManager()->CreateDeath(m_Pos, m_BotID);
	GameWorld()->BotManager()->OnBotDeath(m_BotID);

	(void) pKiller;
	(void) Weapon;

	GameWorld()->DestroyEntity(this);
}

bool CBotEntity::IncreaseHealth(int Amount)
{
	if(m_Health >= m_MaxHealth)
		return false;
	m_Health = clamp(m_Health + Amount, 0, m_MaxHealth);
	return true;
}

bool CBotEntity::IncreaseArmor(int Amount)
{
	if(m_Armor >= m_MaxArmor)
		return false;
	m_Armor = clamp(m_Armor + Amount, 0, m_MaxArmor);
	return true;
}
