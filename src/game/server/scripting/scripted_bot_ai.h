/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_SCRIPTING_SCRIPTED_BOT_AI_H
#define GAME_SERVER_SCRIPTING_SCRIPTED_BOT_AI_H

#include <base/vmath.h>

#include <string>

class CBotEntity;
class CEntity;
struct SScriptContext;

/*
	Interface: IBotAI
		A bot AI strategy. The body, physics and network of a bot stay in
		CBotEntity; a strategy only decides the input of one tick and may react
		to hits and damage.

		CBotEntity owns its strategy and deletes it. Strategies are never
		shared between bots: CScriptedBotAI keeps per-bot Python state, and a
		future stateful strategy may do the same.
*/
class IBotAI
{
public:
	virtual ~IBotAI() {}

	// Stable name shown in logs and in ctx["extra"]["ai"].
	virtual const char *Name() const = 0;

	// Called once, lazily on the first tick after the strategy was attached.
	virtual void OnSpawn(CBotEntity *pBot) { (void) pBot; }

	// Called once per game tick before OnAction. Optional; the built-in
	// strategies do nothing here.
	virtual void OnTick(CBotEntity *pBot) { (void) pBot; }

	// Decides this tick's input. Required.
	virtual void OnAction(CBotEntity *pBot) = 0;

	// Returning false reports the hit as absorbed, matching
	// CBotEntity::OnBotDamaged and CCharacter::TakeDamage.
	virtual bool OnDamaged(CBotEntity *pBot, vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon)
	{
		(void) pBot;
		(void) Force;
		(void) Source;
		(void) Dmg;
		(void) pFrom;
		(void) Weapon;
		return true;
	}

	// Called after one melee swing landed on a hittable entity.
	virtual void OnHit(CBotEntity *pBot, CEntity *pTarget, bool Killed)
	{
		(void) pBot;
		(void) pTarget;
		(void) Killed;
	}
};

// The task description mentions both names; they are the same contract.
using CBotAI = IBotAI;

// The names sv_bot_ai understands without a script path.
const char *const *BotAIList(int *pCount);
const char *BotAIDefaultName();

// Returns a freshly allocated strategy, or null for an unknown name. The caller
// owns the result and must delete it (CBotEntity::SetAI does).
IBotAI *CreateNativeBotAI(const char *pName);

// Loads pScriptPath through the shared script runtime. pFallback is owned by the
// returned strategy; when null a "chase" strategy is used.
IBotAI *CreateScriptedBotAI(const char *pScriptPath, IBotAI *pFallback = nullptr);

// Resolves any accepted sv_bot_ai value: "chase", "wander", "stand",
// "script:<path>", a "<name>.py" path, or a bare name looked up as
// "scripts/bot_ai/<name>.py". Returns null when nothing matches.
IBotAI *CreateBotAI(const char *pName);

// Whether CreateBotAI would resolve pName. Does not load a module.
bool BotAIIsKnownName(const char *pName);

/*
	Class: CScriptedBotAI
		A strategy that layers Python on top of a native one.

		Each tick (and each damage/hit event) it calls the module's optional
		on_spawn / on_tick / on_action / on_damaged / on_hit functions with the
		shared class-based context (ctx.entity, ctx.player, ctx.state, ...). The
		script drives the bot through the embedded "lastday_bot" module and can
		suppress the native strategy for one tick by setting ctx.consumed (or
		ctx.result["handled"]).

		When the hook does not handle the tick, the wrapped fallback strategy
		runs afterwards, so a script can add behaviour without replacing the
		body-driving logic.
*/
class CScriptedBotAI : public IBotAI
{
public:
	CScriptedBotAI(const char *pScriptPath, IBotAI *pFallback);
	~CScriptedBotAI() override;

	const char *Name() const override { return m_aName; }
	const char *ScriptPath() const { return m_aScript; }
	int Module() const { return m_Module; }
	bool Loaded() const { return m_Module >= 0; }

	void OnSpawn(CBotEntity *pBot) override;
	void OnTick(CBotEntity *pBot) override;
	void OnAction(CBotEntity *pBot) override;
	bool OnDamaged(CBotEntity *pBot, vec2 Force, vec2 Source, int Dmg, CEntity *pFrom, int Weapon) override;
	void OnHit(CBotEntity *pBot, CEntity *pTarget, bool Killed) override;

private:
	bool HasHook(const char *pFunction) const;
	bool CallEvent(CBotEntity *pBot, const char *pFunction, int TargetID, const char *pParams, bool *pHandled, std::string *pResult = nullptr);
	void FillContext(CBotEntity *pBot, const char *pFunction, int TargetID, SScriptContext &Ctx);
	void MakeExtra(CBotEntity *pBot, int TargetID, std::string &Extra) const;

	int m_Module;
	char m_aScript[256];
	char m_aName[272];
	IBotAI *m_pFallback;
	std::string m_State;
	bool m_HandledThisTick;
	bool m_SpawnCalled;
	int m_LastTargetID;
};

#endif // GAME_SERVER_SCRIPTING_SCRIPTED_BOT_AI_H