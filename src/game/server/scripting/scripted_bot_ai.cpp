/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>
#include <engine/storage.h>
#include <game/server/botmanager.h>
#include <game/server/entities/botentity.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/scripting/script_runtime.h>
#include <game/server/scripting/scripted_bot_ai.h>

#include <pybind11/embed.h>
#include <string>

namespace py = pybind11;

static const char *const gs_apBotAINames[] = {"chase", "wander", "stand"};

static bool ScriptExists(const char *pPath)
{
	if(!pPath || !pPath[0])
		return false;
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	IStorage *pStorage = pRuntime ? pRuntime->Storage() : nullptr;
	if(!pStorage)
		return false;

	void *pData = nullptr;
	unsigned DataSize = 0;
	if(!pStorage->ReadFile(pPath, IStorage::TYPE_ALL, &pData, &DataSize))
		return false;
	free(pData);
	return true;
}

// Resolves a sv_bot_ai / set_ai value to a script path. A bare name is looked
// up under scripts/bot_ai/, which is where the shipped examples live.
static bool BuildScriptPath(const char *pName, char *pPath, int PathSize)
{
	if(!pName || !pName[0])
		return false;

	const char *pAfterPrefix = str_startswith(pName, "script:");
	if(pAfterPrefix)
	{
		str_copy(pPath, pAfterPrefix, PathSize);
		return pPath[0] != 0;
	}

	if(str_endswith(pName, ".py"))
	{
		str_copy(pPath, pName, PathSize);
		return true;
	}

	str_format(pPath, PathSize, "scripts/bot_ai/%s.py", pName);
	return true;
}

static bool IsNativeName(const char *pName)
{
	if(!pName)
		return false;
	for(const char *pNative : gs_apBotAINames)
		if(str_comp_nocase(pName, pNative) == 0)
			return true;
	return false;
}

static bool JsonBoolTrue(const std::string &Json, const char *pKey)
{
	const std::string Key = std::string("\"") + pKey + "\"";
	size_t Pos = Json.find(Key);
	if(Pos == std::string::npos)
		return false;
	Pos = Json.find(':', Pos + Key.size());
	if(Pos == std::string::npos)
		return false;
	Pos++;
	while(Pos < Json.size() && (Json[Pos] == ' ' || Json[Pos] == '\t'))
		Pos++;
	return Json.compare(Pos, 4, "true") == 0;
}

static std::string JsonEscape(const char *pText)
{
	std::string Out;
	for(const char *p = pText; p && *p; p++)
	{
		const unsigned char Ch = (unsigned char) *p;
		if(Ch == '"' || Ch == '\\')
		{
			Out += '\\';
			Out += (char) Ch;
		}
		else if(Ch < 0x20)
			Out += ' ';
		else
			Out += (char) Ch;
	}
	return Out;
}

static py::tuple Vec2Tuple(float x, float y)
{
	py::tuple Result(2);
	Result[0] = py::cast(x);
	Result[1] = py::cast(y);
	return Result;
}

static CGameContext *Game()
{
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	return pRuntime ? pRuntime->GameServer() : nullptr;
}

// Finds any entity by its server snap id. Ids are global among live entities,
// so a scan of the live worlds is exact; scripts only call this a few times a
// tick, not in the physics hot path.
struct SEntityLookup
{
	int m_ID;
	CEntity *m_pEntity;
};

static CEntity *FindWorldEntityByID(CGameWorld *pWorld, int ID)
{
	if(!pWorld || ID < 0)
		return nullptr;
	for(int Type = 0; Type < CGameWorld::NUM_ENTTYPES; Type++)
	{
		for(CGameWorld::TypeRange Range = pWorld->DoTypeRange(Type); !Range.empty(); Range.pop_front())
		{
			CEntity *pEnt = Range.front();
			if(pEnt && pEnt->GetID() == ID)
				return pEnt;
		}
	}
	return nullptr;
}

static CEntity *FindEntityByID(int ID)
{
	CGameContext *pGame = Game();
	if(!pGame || ID < 0)
		return nullptr;

	SEntityLookup Data = {ID, nullptr};
	pGame->m_pWorlds.for_each([](CGameWorld *&pWorld, void *pUser) -> void
	{
		SEntityLookup *pData = static_cast<SEntityLookup *>(pUser);
		if(pData->m_pEntity || !pWorld)
			return;
		pData->m_pEntity = FindWorldEntityByID(pWorld, pData->m_ID);
	}, &Data);
	return Data.m_pEntity;
}

static CBotEntity *FindBotByID(int ID)
{
	CEntity *pEnt = FindEntityByID(ID);
	if(!pEnt || pEnt->ObjType() != CGameWorld::ENTTYPE_BOTENTITY)
		return nullptr;
	return static_cast<CBotEntity *>(pEnt);
}

static CEntity *FindTargetNear(CBotEntity *pBot, float Radius)
{
	if(!pBot)
		return nullptr;
	CGameWorld *pWorld = pBot->GameWorld();
	if(!pWorld || pWorld->IsNpcWorld())
		return nullptr;

	if(Radius <= 0.0f)
		Radius = 320.0f;
	Radius = clamp(Radius, 1.0f, 4096.0f);

	CEntity *pTarget = pWorld->ClosestFlagEntity(pBot->GetPos(), Radius, CGameWorld::ENTFLAG_HITABLE, pBot);
	if(pTarget && pWorld->Collision()->IntersectLine(pBot->GetPos(), pWorld->PosInFrame(pTarget), NULL, NULL))
		pTarget = nullptr;
	return pTarget;
}

static bool IsVisibleWorld(CGameWorld *pWorld, CGameWorld *pOther)
{
	if(pWorld == pOther)
		return true;
	CGameWorld *apNeighbours[2];
	vec2 aOffsets[2];
	const int Num = pWorld ? pWorld->RingNeighbours(apNeighbours, aOffsets, 2) : 0;
	for(int i = 0; i < Num; i++)
		if(apNeighbours[i] == pOther)
			return true;
	return false;
}

static bool HasLineOfSight(CBotEntity *pBot, CEntity *pTarget)
{
	if(!pBot || !pTarget || pTarget == pBot)
		return false;
	CGameWorld *pWorld = pBot->GameWorld();
	if(!pWorld || !IsVisibleWorld(pWorld, pTarget->GameWorld()))
		return false;
	return !pWorld->Collision()->IntersectLine(pBot->GetPos(), pWorld->PosInFrame(pTarget), NULL, NULL);
}

// --- embedded module lastday_bot -----------------------------------------

static bool BotSetInput(int Entity, int Direction, bool Jump, bool Fire, float TargetX, float TargetY)
{
	CBotEntity *pBot = FindBotByID(Entity);
	if(!pBot || !pBot->IsAlive())
		return false;

	pBot->SetDirection(Direction);
	pBot->SetJump(Jump);
	pBot->SetFire(Fire);

	if(absolute(TargetX) < 1.0e7f && absolute(TargetY) < 1.0e7f && (TargetX != 0.0f || TargetY != 0.0f))
	{
		vec2 Target(TargetX, TargetY);
		if(length(Target) > 400.0f)
			Target = normalize(Target) * 400.0f;
		pBot->SetAimTarget(Target);
	}
	return true;
}

static py::tuple BotGetPos(int Entity)
{
	CBotEntity *pBot = FindBotByID(Entity);
	if(!pBot)
		return Vec2Tuple(0.0f, 0.0f);
	return Vec2Tuple(pBot->GetPos().x, pBot->GetPos().y);
}

static int BotFindTarget(int Entity, float Radius)
{
	CBotEntity *pBot = FindBotByID(Entity);
	CEntity *pTarget = FindTargetNear(pBot, Radius);
	return pTarget ? pTarget->GetID() : -1;
}

static bool BotHasLineOfSight(int Entity, int TargetEntity)
{
	CBotEntity *pBot = FindBotByID(Entity);
	if(!pBot)
		return false;
	return HasLineOfSight(pBot, FindEntityByID(TargetEntity));
}

static bool BotSetAI(int Entity, const std::string &Name)
{
	CBotEntity *pBot = FindBotByID(Entity);
	if(!pBot || Name.empty())
		return false;
	if(!BotAIIsKnownName(Name.c_str()))
		return false;
	// Deferred: the call may come from this very bot's on_action hook, and
	// deleting the running strategy there would be a use-after-free.
	return pBot->RequestAI(Name.c_str());
}

static void BotLog(const std::string &Message)
{
	dbg_msg("bot", "%s", Message.c_str());
}

// --- native strategies ----------------------------------------------------

class CChaseBotAI : public IBotAI
{
public:
	const char *Name() const override { return "chase"; }

	void OnAction(CBotEntity *pBot) override
	{
		CEntity *pTarget = pBot->FindTarget();
		if(pTarget)
		{
			const vec2 Target = pBot->GameWorld()->PosInFrame(pTarget);

			// walk towards the target, but only correct while noticeably off
			if(absolute(Target.x - pBot->GetPos().x) > 48.0f)
				pBot->SetDirection((int) sign(Target.x - pBot->GetPos().x));
			else
				pBot->SetDirection(0);

			if((Target.y - pBot->GetPos().y < -48.0f) && !(pBot->Core()->m_Jumped & 1) && (pBot->Core()->m_Vel.y > -1.25f))
				pBot->SetJump(true);

			if(distance(Target, pBot->GetPos()) < 48.0f && (random_int() % 100) < 8 && pBot->ReloadTimer() <= 0)
				pBot->SetFire(true);

			pBot->SetAimTarget(Target - pBot->GetPos());
		}
		else
		{
			// wander: occasionally jump or change direction
			if(random_int() % 1000 < 25)
				pBot->SetJump(true);

			if(random_int() % 1000 < 55)
			{
				const int Direction = random_int() % 3 - 1;
				pBot->SetDirection(Direction);
				if(Direction)
					pBot->SetAimTarget(vec2(length(pBot->CursorTarget()) * Direction, 0.0f));
			}
		}

		pBot->AimCursor();
	}
};

class CWanderBotAI : public IBotAI
{
public:
	const char *Name() const override { return "wander"; }

	void OnAction(CBotEntity *pBot) override
	{
		// no target: occasionally jump or change direction
		if(random_int() % 1000 < 25)
			pBot->SetJump(true);

		if(random_int() % 1000 < 55)
		{
			const int Direction = random_int() % 3 - 1;
			pBot->SetDirection(Direction);
			if(Direction)
				pBot->SetAimTarget(vec2(length(pBot->CursorTarget()) * Direction, 0.0f));
		}

		pBot->AimCursor();
	}
};

class CStandBotAI : public IBotAI
{
public:
	const char *Name() const override { return "stand"; }

	void OnAction(CBotEntity *pBot) override
	{
		// Idle: no walking, no jumping, no attacking. The aim cursor still
		// tracks a visible target so the tee looks alive.
		pBot->SetDirection(0);
		pBot->SetJump(false);

		CEntity *pTarget = pBot->FindTarget();
		if(pTarget)
			pBot->SetAimTarget(pBot->GameWorld()->PosInFrame(pTarget) - pBot->GetPos());

		pBot->AimCursor();
	}
};

const char *const *BotAIList(int *pCount)
{
	if(pCount)
		*pCount = (int) (sizeof(gs_apBotAINames) / sizeof(gs_apBotAINames[0]));
	return gs_apBotAINames;
}

const char *BotAIDefaultName()
{
	return "chase";
}

IBotAI *CreateNativeBotAI(const char *pName)
{
	if(!pName || !pName[0] || str_comp_nocase(pName, "chase") == 0)
		return new CChaseBotAI();
	if(str_comp_nocase(pName, "wander") == 0)
		return new CWanderBotAI();
	if(str_comp_nocase(pName, "stand") == 0)
		return new CStandBotAI();
	return nullptr;
}

IBotAI *CreateScriptedBotAI(const char *pScriptPath, IBotAI *pFallback)
{
	if(!pScriptPath || !pScriptPath[0])
	{
		delete pFallback;
		return nullptr;
	}
	if(!pFallback)
		pFallback = new CChaseBotAI();
	return new CScriptedBotAI(pScriptPath, pFallback);
}

IBotAI *CreateBotAI(const char *pName)
{
	if(!pName || !pName[0])
		pName = BotAIDefaultName();

	if(IsNativeName(pName))
		return CreateNativeBotAI(pName);

	// Any other name is a script: either "script:<path>", a path that ends in
	// .py, or a bare example name under scripts/bot_ai/.
	char aPath[256];
	if(!BuildScriptPath(pName, aPath, sizeof(aPath)))
		return nullptr;
	if(!ScriptExists(aPath))
		return nullptr;
	return CreateScriptedBotAI(aPath, new CChaseBotAI());
}

bool BotAIIsKnownName(const char *pName)
{
	if(!pName || !pName[0])
		return false;
	if(IsNativeName(pName))
		return true;

	char aPath[256];
	if(!BuildScriptPath(pName, aPath, sizeof(aPath)))
		return false;
	return ScriptExists(aPath);
}

CScriptedBotAI::CScriptedBotAI(const char *pScriptPath, IBotAI *pFallback)
{
	m_Module = -1;
	str_copy(m_aScript, pScriptPath ? pScriptPath : "", sizeof(m_aScript));
	str_format(m_aName, sizeof(m_aName), "script:%s", m_aScript);
	m_pFallback = pFallback ? pFallback : new CChaseBotAI();
	m_State = "{}";
	m_HandledThisTick = false;
	m_SpawnCalled = false;
	m_LastTargetID = -1;

	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	if(!pRuntime || !pRuntime->Ready())
	{
		dbg_msg("bot", "scripted bot AI '%s' created before the script runtime was ready", m_aScript);
		return;
	}

	m_Module = pRuntime->LoadModule(m_aScript);
	if(m_Module < 0)
		dbg_msg("bot", "scripted bot AI '%s' failed to load, using '%s'", m_aScript, m_pFallback->Name());
	else
		dbg_msg("bot", "scripted bot AI '%s' loaded (module %d)", m_aScript, m_Module);
}

CScriptedBotAI::~CScriptedBotAI()
{
	delete m_pFallback;
	m_pFallback = nullptr;
}

bool CScriptedBotAI::HasHook(const char *pFunction) const
{
	if(m_Module < 0 || !pFunction)
		return false;
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	return pRuntime && pRuntime->HasFunction(m_Module, pFunction);
}

void CScriptedBotAI::MakeExtra(CBotEntity *pBot, int TargetID, std::string &Extra) const
{
	Extra = "{\"ai\":\"";
	Extra += JsonEscape(Name());
	Extra += "\",\"health\":";
	Extra += std::to_string(pBot ? pBot->GetHealth() : 0);
	Extra += ",\"target_id\":";
	Extra += std::to_string(TargetID);
	Extra += "}";
}

void CScriptedBotAI::FillContext(CBotEntity *pBot, const char *pFunction, int TargetID, SScriptContext &Ctx)
{
	Ctx.m_pGameServer = pBot->GameServer();
	Ctx.m_ClientID = -1;
	Ctx.m_EntityID = pBot->GetID();
	Ctx.m_WorldID = pBot->GameWorld()->WorldID();
	Ctx.m_Pos = pBot->GetPos();
	Ctx.m_Now = pBot->Server()->Tick();
	Ctx.m_TickSpeed = pBot->Server()->TickSpeed();
	Ctx.m_State = m_State;
	str_copy(Ctx.m_aKind, "bot", sizeof(Ctx.m_aKind));
	str_copy(Ctx.m_aEvent, pFunction, sizeof(Ctx.m_aEvent));
	MakeExtra(pBot, TargetID, Ctx.m_Extra);
}

bool CScriptedBotAI::CallEvent(CBotEntity *pBot, const char *pFunction, int TargetID, const char *pParams, bool *pHandled, std::string *pResult)
{
	if(pHandled)
		*pHandled = false;
	if(pResult)
		pResult->clear();
	if(!HasHook(pFunction))
		return false;

	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	if(!pRuntime || !pRuntime->Enabled())
		return false;

	SScriptContext Ctx;
	FillContext(pBot, pFunction, TargetID, Ctx);
	if(pParams)
		Ctx.m_Params = pParams;

	if(!pRuntime->Call(m_Module, pFunction, Ctx))
	{
		dbg_msg("bot", "scripted bot AI '%s': %s failed: %s", m_aScript, pFunction, pRuntime->LastError());
		return false;
	}

	m_State = Ctx.m_State;
	if(pResult)
		*pResult = Ctx.m_Result;
	// The class-based context exposes the suppression flag as ctx.consumed,
	// which the runtime copies back. result["handled"] is accepted too, so a
	// script written against the older dict wording keeps working.
	if(pHandled)
		*pHandled = Ctx.m_Consumed || JsonBoolTrue(Ctx.m_Result, "handled");
	return true;
}
void CScriptedBotAI::OnSpawn(CBotEntity *pBot)
{
	bool Handled = false;
	CallEvent(pBot, "on_spawn", -1, nullptr, &Handled);
}

void CScriptedBotAI::OnTick(CBotEntity *pBot)
{
	m_HandledThisTick = false;

	if(!m_SpawnCalled)
	{
		m_SpawnCalled = true;
		OnSpawn(pBot);
	}

	if(m_pFallback)
		m_pFallback->OnTick(pBot);

	bool Handled = false;
	if(CallEvent(pBot, "on_tick", m_LastTargetID, nullptr, &Handled))
		m_HandledThisTick = Handled;
}

void CScriptedBotAI::OnAction(CBotEntity *pBot)
{
	// A handled on_tick owns this tick; the native strategy stays out.
	if(m_HandledThisTick)
		return;

	bool Handled = false;
	if(HasHook("on_action"))
	{
		CEntity *pTarget = pBot->FindTarget();
		const int TargetID = pTarget ? pTarget->GetID() : -1;
		if(CallEvent(pBot, "on_action", TargetID, nullptr, &Handled))
			m_LastTargetID = TargetID;
	}

	if(Handled)
		return;

	// The hook did not take over: the native strategy still drives the body.
	if(m_pFallback)
		m_pFallback->OnAction(pBot);
}

bool CScriptedBotAI::OnDamaged(CBotEntity *pBot, vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
{
	(void) Force;

	bool Result = m_pFallback ? m_pFallback->OnDamaged(pBot, Force, Source, Dmg, pFrom, Weapon) : true;
	if(!HasHook("on_damaged"))
		return Result;

	std::string Params = "{\"damage\":";
	Params += std::to_string(Dmg);
	Params += ",\"weapon\":";
	Params += std::to_string(Weapon);
	Params += ",\"source\":[";
	Params += std::to_string(Source.x);
	Params += ",";
	Params += std::to_string(Source.y);
	Params += "]}";

	bool Handled = false;
	std::string ScriptResult;
	if(CallEvent(pBot, "on_damaged", pFrom ? pFrom->GetID() : -1, Params.c_str(), &Handled, &ScriptResult) &&
		JsonBoolTrue(ScriptResult, "absorbed"))
		Result = false;
	return Result;
}

void CScriptedBotAI::OnHit(CBotEntity *pBot, CEntity *pTarget, bool Killed)
{
	if(m_pFallback)
		m_pFallback->OnHit(pBot, pTarget, Killed);

	if(!HasHook("on_hit"))
		return;

	bool Handled = false;
	CallEvent(pBot, "on_hit", pTarget ? pTarget->GetID() : -1,
		Killed ? "{\"killed\":true}" : "{\"killed\":false}", &Handled);
}

PYBIND11_EMBEDDED_MODULE(lastday_bot, m)
{
	m.def("set_input", [](int Entity, int Direction, bool Jump, bool Fire, float TargetX, float TargetY)
	{
		return BotSetInput(Entity, Direction, Jump, Fire, TargetX, TargetY);
	});
	m.def("get_pos", [](int Entity)
	{ return BotGetPos(Entity); });
	m.def("find_target", [](int Entity, float Radius)
	{ return BotFindTarget(Entity, Radius); });
	m.def("has_line_of_sight", [](int Entity, int TargetEntity)
	{ return BotHasLineOfSight(Entity, TargetEntity); });
	m.def("set_ai", [](int Entity, const std::string &Name)
	{ return BotSetAI(Entity, Name); });
	m.def("log", [](const std::string &Message)
	{ BotLog(Message); });
}
