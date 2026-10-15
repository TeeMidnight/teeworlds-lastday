#!/usr/bin/env python3
"""Migrate a LastDay sqlite database to the UUID based item format.

Older profiles stored items by their bare resource id ("hammer") in the
inventory ("res_id") and by name in the loadout array. The engine now stores a
canonical item UUID plus the namespaced source name. The engine still reads the
old form as a fallback, but this rewrites the rows so the fallback is never
needed again.

The UUID derivation must match src/game/item_id.cpp exactly; scripts/test
ItemID.NamespacedNameToUuid pins the mapping down.

Usage:
    python3 scripts/migrate_item_ids.py [database/lastday.db] [--dry-run] [--no-backup]
"""

import argparse
import datetime
import hashlib
import json
import os
import shutil
import sqlite3
import uuid

TEEWORLDS_NAMESPACE = bytes.fromhex("e05ddaaac4e64cfbb6425d48e80c0029")


def calculate_uuid(name: str) -> str:
    digest = bytearray(hashlib.md5(TEEWORLDS_NAMESPACE + name.encode("utf-8")).digest())
    digest[6] = (digest[6] & 0x0F) | 0x30  # version 3
    digest[8] = (digest[8] & 0x3F) | 0x80  # RFC 4122 variant
    return str(uuid.UUID(bytes=bytes(digest)))


def item_id(name: str):
    normalized = name if ":" in name else "lastday:" + name
    return calculate_uuid("lastday:item/" + normalized), normalized


def is_uuid(text: str) -> bool:
    if not isinstance(text, str) or len(text) != 36:
        return False
    try:
        uuid.UUID(text)
        return True
    except ValueError:
        return False


def migrate_inventory(inventory):
    changed = False
    if not isinstance(inventory, list):
        return changed
    for entry in inventory:
        if not isinstance(entry, dict):
            continue
        if "id" in entry:
            continue
        name = entry.pop("res_id", None)
        if not name:
            continue
        uid, normalized = item_id(name)
        entry["id"] = uid
        entry["name"] = normalized
        changed = True
    return changed


def migrate_loadout(loadout):
    changed = False
    if not isinstance(loadout, list):
        return changed
    for i, slot in enumerate(loadout):
        if not slot or is_uuid(slot):
            continue
        uid, _ = item_id(slot)
        loadout[i] = uid
        changed = True
    return changed


def migrate_sanity(data):
    if "sanity" not in data:
        return False
    if "forgetting" not in data:
        data["forgetting"] = max(0, min(100, 100 - int(data["sanity"])))
    del data["sanity"]
    return True


def migrate_drop_container(node):
    """Rewrite any res_id into id/name and any item name into a uuid."""
    changed = False
    if isinstance(node, dict):
        if "res_id" in node and "id" not in node:
            uid, normalized = item_id(node.pop("res_id"))
            node["id"] = uid
            node["name"] = normalized
            changed = True
        if "item" in node and isinstance(node["item"], str) and node["item"] and not is_uuid(node["item"]):
            node["item"] = item_id(node["item"])[0]
            changed = True
        for value in node.values():
            changed |= migrate_drop_container(value)
    elif isinstance(node, list):
        for value in node:
            changed |= migrate_drop_container(value)
    return changed


def migrate_player_data(raw):
    try:
        data = json.loads(raw)
    except (TypeError, ValueError):
        return raw, False
    if not isinstance(data, dict):
        return raw, False
    changed = migrate_inventory(data.get("inventory"))
    changed |= migrate_loadout(data.get("loadout"))
    changed |= migrate_sanity(data)
    if not changed:
        return raw, False
    return json.dumps(data, ensure_ascii=False, separators=(",", ":")), True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("db", nargs="?", default="database/lastday.db")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--no-backup", action="store_true")
    args = parser.parse_args()

    if not os.path.exists(args.db):
        raise SystemExit("database not found: " + args.db)

    con = sqlite3.connect(args.db)
    con.row_factory = sqlite3.Row

    updated_players = 0
    updated_worlds = 0
    updated_drops = 0

    players = con.execute("SELECT rowid, uuid, data FROM players").fetchall()
    for row in players:
        new_data, changed = migrate_player_data(row["data"])
        if changed:
            updated_players += 1
            print("player %s -> item ids migrated" % row["uuid"])
            if not args.dry_run:
                con.execute("UPDATE players SET data = ? WHERE rowid = ?", (new_data, row["rowid"]))

    for table, counter_name in (("world_saves", "worlds"), ("dropped_items", "drops")):
        try:
            rows = con.execute("SELECT rowid, data FROM " + table).fetchall()
        except sqlite3.OperationalError:
            continue
        for row in rows:
            try:
                data = json.loads(row["data"])
            except (TypeError, ValueError):
                continue
            if not migrate_drop_container(data):
                continue
            if table == "world_saves":
                updated_worlds += 1
            else:
                updated_drops += 1
            if not args.dry_run:
                con.execute("UPDATE %s SET data = ? WHERE rowid = ?" % table,
                            (json.dumps(data, ensure_ascii=False, separators=(",", ":")), row["rowid"]))

    if args.dry_run:
        con.rollback()
    else:
        con.commit()
    con.close()

    if not args.no_backup and not args.dry_run:
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        backup = "%s.bak-%s" % (args.db, stamp)
        shutil.copyfile(args.db, backup)
        print("backup written to " + backup)

    print("done: players=%d worlds=%d drops=%d%s" % (
        updated_players, updated_worlds, updated_drops,
        " (dry run)" if args.dry_run else ""))


if __name__ == "__main__":
    main()
