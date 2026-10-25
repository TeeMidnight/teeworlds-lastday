#ifndef GAME_SERVER_SCRIPTING_SCRIPT_COMMANDS_H
#define GAME_SERVER_SCRIPTING_SCRIPT_COMMANDS_H

class CGameContext;

// Registers the reload_scripts / buff / give_item / script_selftest commands.
void RegisterScriptCommands(CGameContext *pGameServer);

#endif
