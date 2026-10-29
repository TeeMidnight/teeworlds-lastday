/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds Last Day - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <engine/console.h>
#include <engine/shared/config.h>
#include <game/server/currency.h>
#include <game/server/database/database.h>
#include <game/server/gamecontext.h>
#include <game/server/item.h>
#include <game/server/player.h>
#include <game/server/scripting/buff_system.h>
#include <game/server/scripting/script_commands.h>
#include <game/server/scripting/script_runtime.h>
#include <game/server/scripting/scripted_entity.h>
#include <game/server/weaponmanager.h>

static void ConReloadScripts(IConsole::IResult *pResult, void *pUserData)
{
	(void) pResult;
	CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	const bool ScriptsOk = pRuntime->ReloadAll();
	const bool ItemsOk = pGameServer->Item() && pGameServer->Item()->ReloadScripts();
	if(pGameServer->Buffs())
		pGameServer->Buffs()->ReloadScripts();
	if(pGameServer->ScriptedEntities())
		pGameServer->ScriptedEntities()->ReloadScripts();
	WeaponManager()->ReloadScripts();
	pGameServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "script",
		ScriptsOk && ItemsOk ? "scripts reloaded" : "some scripts failed to reload; see the log");
}

static void ConScriptSelfTest(IConsole::IResult *pResult, void *pUserData)
{
	(void) pResult;
	CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
	CScriptRuntime *pRuntime = CScriptRuntime::Get();
	const int Module = pRuntime->LoadModule("scripts/selftest.py");
	if(Module < 0)
	{
		pGameServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "script", "selftest: could not load scripts/selftest.py");
		return;
	}

	SScriptContext Ctx;
	Ctx.m_pGameServer = pGameServer;
	Ctx.m_ClientID = -1;
	Ctx.m_Now = pGameServer->Server()->Tick();
	Ctx.m_TickSpeed = pGameServer->Server()->TickSpeed();
	str_copy(Ctx.m_aEvent, "selftest", sizeof(Ctx.m_aEvent));
	if(!pRuntime->Call(Module, "on_selftest", Ctx))
	{
		pGameServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "script", pRuntime->LastError());
		return;
	}

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "selftest: consumed=%d state=%s result=%s",
		Ctx.m_Consumed ? 1 : 0, Ctx.m_State.c_str(), Ctx.m_Result.c_str());
	pGameServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "script", aBuf);
}

static void ConBuff(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
	const int ClientID = pResult->GetInteger(0);
	const char *pBuffId = pResult->GetString(1);
	const int Seconds = pResult->NumArguments() > 2 ? pResult->GetInteger(2) : -1;
	const int Stacks = pResult->NumArguments() > 3 ? pResult->GetInteger(3) : 1;
	if(!pGameServer->Buffs() || !pGameServer->Buffs()->Add(ClientID, pBuffId,
					    Seconds < 0 ? -1 : Seconds * pGameServer->Server()->TickSpeed(), Stacks, ClientID))
	{
		pGameServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "script", "could not apply buff");
		return;
	}
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "applied buff '%s' to client %d", pBuffId, ClientID);
	pGameServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "script", aBuf);
}

static void ConGiveItem(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
	const int ClientID = pResult->GetInteger(0);
	const char *pItemId = pResult->GetString(1);
	const int Count = pResult->NumArguments() > 2 ? pResult->GetInteger(2) : 1;
	if(!pGameServer->Item() || !pGameServer->Item()->AddItem(ClientID, pItemId, Count))
	{
		pGameServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "script", "could not give item");
		return;
	}
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "gave %d x '%s' to client %d", Count, pItemId, ClientID);
	pGameServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "script", aBuf);
}

static void ConGiveCurrency(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
	const int ClientID = pResult->GetInteger(0);
	const int Amount = pResult->GetInteger(1);
	CPlayer *pPlayer = ClientID >= 0 && ClientID < MAX_CLIENTS ? pGameServer->m_apPlayers[ClientID] : nullptr;
	CDatabase *pDB = pGameServer->Database();
	if(!pPlayer || !pDB || pPlayer->m_AccountUuid == UUID_ZEROED || Amount <= 0)
	{
		pGameServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "script", "could not give currency");
		return;
	}
	const int64 Delta = minimum((int64) Amount, CURRENCY_MAX_AMOUNT);
	char aReason[CURRENCY_REASON_SIZE];
	str_format(aReason, sizeof(aReason), "admin grant via give_currency");
	if(!pDB->AddCurrency(pPlayer->m_AccountUuid, Delta, aReason))
	{
		pGameServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "script", "could not give currency");
		return;
	}
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "gave %lld G to client %d", (long long) Delta, ClientID);
	pGameServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "script", aBuf);
}

void RegisterScriptCommands(CGameContext *pGameServer)
{
	pGameServer->Console()->Register("reload_scripts", "", CFGFLAG_SERVER, ConReloadScripts, pGameServer,
		"Reload item/buff/weapon/entity scripts in place");
	pGameServer->Console()->Register("script_selftest", "", CFGFLAG_SERVER, ConScriptSelfTest, pGameServer,
		"Run a trivial Python callback through the host bridge");
	pGameServer->Console()->Register("buff", "i[client] s[buff] ?i[seconds] ?i[stacks]", CFGFLAG_SERVER, ConBuff, pGameServer,
		"Apply a buff to a client");
	pGameServer->Console()->Register("give_item", "i[client] s[item] ?i[count]", CFGFLAG_SERVER, ConGiveItem, pGameServer,
		"Give an item to a client");
	pGameServer->Console()->Register("give_currency", "i[client] i[amount]", CFGFLAG_SERVER, ConGiveCurrency, pGameServer,
		"Give G to a client (written to the currency ledger)");
}
