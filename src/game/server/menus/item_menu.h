/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef GAME_SERVER_MENUS_ITEM_MENU_H
#define GAME_SERVER_MENUS_ITEM_MENU_H

#include <game/item_id.h>

// A menu option command has to be a stable string; the namespaced source name is
// readable in logs, the canonical uuid is the fallback for uuid-only items.
// Shared by the item menus and the trade/loadout menus in this directory.
const char *ItemCommand(const CItemID &Id, char *pBuf, int Size);

#endif
