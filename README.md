Teeworlds LastDay
=================
This is the rework of old teeworlds survival craft modification LastDay.

It's based on Teeworlds Archive 0.7.6

Contains LLM-Generated codes (Reviewed)
Ring worlds
-----------
The generated euclidean floors (Verdant, Hollow, Highlands, Depths) are rings.
Each floor's seeds 1..1024 are consecutive windows of one shared Perlin field,
so neighbouring instances generate the 75-tile strip they overlap identically
and the 1024 instances close into one world that can be walked around.

- Geometry: `stride = width - shared_margin`, `period = size * stride`; instance
  `n` starts at global column `(n-1) * stride`.
- Crossing: past 2/3 of the shared strip the character steps into the right
  neighbour, past 1/3 into the left one; the landing sits on the same global
  column, so the world transfer keeps its position and cannot bounce.
- Entities: the ring view of a neighbour is generic, not a per type list. Any
  entity is found by the normal queries across the boundary, rendered there via
  ShiftForSnap (each entity shifts whatever it draws from), and an entity that
  opts in with RingCrosses follows the boundary transfer (drops, projectiles,
  bots). A bot the client was not told about renders nothing rather than the
  wrong tee.
- Resources: a node is only spawned past the shared margin, so each physical
  node has a single owner and is never mined twice.
- The neighbours of an occupied ring instance are kept loaded.

Enable it in `datasrc/maps/worlds/<Floor>.json`:

    "generation": {
        "ring": { "enabled": true, "size": 1024, "shared_margin": 75, "base_seed": 1101 }
    }

Generated maps are cached by name and mod version: they live under
`generatedmaps/LastDay/<MOD_VERSION>/` in the storage save path. After changing
a floor's generation settings, delete
`generatedmaps/LastDay/<MOD_VERSION>/<Floor>_<Seed>.map` so the next request
regenerates it. `mapgentool worldjson <Map>` prints the ring geometry a
generated map carries.

Cross-boundary hooks and player collisions work through proxy cores: each ring
instance binds its live neighbours' players into its own world core under their
real client id, translated onto its axis, and forwards any hook drag to the real
character. The mirrored snapshot gives the client the same characters, so its
prediction matches.

Bots are covered by a single global `CBotManager` for the whole game rather than
one per instance. Its per-client slot table is global, so the bots of the
client's own world and of its live neighbours resolve to distinct ids and the
neighbour mirror can render them; the manager also hands each bot a globally
unique private CWorldCore id, so a snapshot tee id names exactly one bot.

Bots are proxied as well: a live neighbour's bot is borrowed into this world's
core under that same private id, translated onto the axis, so a hook or a
collision on this side reaches it and the drag is forwarded to the real bot at
the end of the tick. Since bot ids and real client ids never overlap, a world
can hold player and bot proxies at the same time.

Storage containers
-------------------
Generated map structures carry storage containers. A container holds one
unlimited stack per item kind, so its `capacity` is the number of *different*
kinds it can hold, not the number of items. Kinds live in
`datasrc/containers.json`:

| kind | capacity |
|---|---|
| crate | 1 |
| chest | 2 |
| vault | 3 |

The first contents of a generated container come from a loot table in
`datasrc/loot/<table>.json`; there is one file per table, and a table is not
tied to a container kind. A table rolls `rolls_min..rolls_max` *distinct* item
kinds (capped by the container capacity); entries are chosen by weight and
carry a descriptive `rarity` of common / uncommon / rare / legendary. Item ids
use the normal namespaced form, so an addon item is written as
`addon:steel_ingot`.

The roll happens once, deterministically, when the instance is first loaded and
its result becomes world state. A container players emptied is still saved (as
an empty item list), so reloading or regenerating the same instance cannot
refill it.

Structures are also one JSON file each, under `datasrc/maps/struct/<id>.json`.
A variant lists its containers and, for each container, the loot table that
structure wants it to use:

    "containers": [
        { "kind": "chest", "loot": "supply_cache" },
        { "kind": "crate", "loot": "scavenger_cache" }
    ]

The generator places each one on a reachable floor tile inside the pasted
region and records its kind, loot table and stable map-instance id in the
instance metadata, so the server can spawn it and put its contents back later.

Containers are used from the world: aim at one and open the main menu (or type
`/container`). The Container page lists the container's slots and the player's
inventory; click a slot to take a stack back or an inventory item to store it.
The vote reason box carries an optional amount, exactly like the inventory
menu's use/drop actions. Container contents are world state: they are written
with the instance's world save when the world is unloaded, keyed by the stable
id, so they survive a world reload. Cached instances keep the containers they
were generated with: after editing a structure file or a world's `structs`,
remove the floor's pack (or pick unused seeds) to regenerate them.

Mobs, factions and dialogue
--------------------------
Monsters are content, not C++ classes. One file per creature lives in
`datasrc/mobs/<id>.json` and defines:

- identity, skin, health and armor
- `faction`: `hostile`, `neutral` or `friendly`
- `ai`: `hunter`, `skirmisher`, `sentry`, `wanderer`, `companion` or
  `support`
- abilities: melee, ranged projectile, leap, heal aura, armor aura, enrage
- localized `dialogue` events (spawn, sight, combat, hurt, kill, death, idle,
  low health), with `{player}` substitution and per-event cooldowns
- weighted `drops`; each entry is a normal item id with a count range and a
  chance

Faction rules are deliberately asymmetric. A hostile mob attacks players,
friendly units, neutral units and hostile mobs of a different `group`
(`fight_hostile_groups`). A neutral mob does nothing until attacked, then
retaliates for a while. A friendly unit attacks hostile mobs only and never
hurts a player. The same rules apply to configured town NPCs: a friendly Medic
never gains aggro or starts a fight, while the neutral Blacksmith gains aggro
when a player hits it and, because it sets `attack_hostiles`, also defends the
square against hostile monsters. A neutral NPC without that flag only answers
the exact creature that hit it. Its aggro ledger also decays passively: every
`decay_seconds` an online player loses `decay_amount` (Blacksmith: 1 per 10s),
in addition to the `kill_decay` applied when the NPC kills that player. Chasing
requires a hit within the last 30 seconds, so stale ledger values cannot make a
neutral NPC attack someone merely for walking close.

`datasrc/mobs/pool.json` is the weighted random population of euclidean
floors. `datasrc/mobs/spawns.json` places fixed content: the friendly Ranger in
the Connector, for example. Fixed spawns respawn after death; a world that owns
them does not also roll random mobs.

`burrower` is a segmented worm: the head and body are real tee-shaped bot
entities, so it reads as a chain of tees, while one `CWormEntity` owns the
shared health, the burrow/surface cycle, contact damage and loot. It phases
through terrain instead of carving it, is only hittable while surfaced, and any
body link routes damage to the whole creature.

Hostile, neutral and friendly combat uses the `PLAYER_TEAM_HOSTILE` and
`PLAYER_TEAM_FRIENDLY` damage sentinels. They keep hostile hits from being
swallowed by the survival mode's red team friendly-fire rule, and keep allied
shots from hurting players, without changing the protocol teams.

Natural monster density
------------------------
`sv_bots` is a boolean switch: 0 disables bots, 1 enables them. There is no
per-server count any more. A world may hold at most one *naturally spawned*
monster per 48 tiles of width (`width / 48`, minimum 1). Fixed content, NPCs,
bosses and boss summons do not consume that budget; a worm counts as one
monster even though its body is several tee links.

Guest profiles
---------------
Logging in is optional. A client without an account joins immediately and gets a
deterministic guest UUID derived from its current nickname. Its inventory,
loadout, level and team membership are stored under that UUID in the normal
player row (with an `@guest:<name>` username and no usable password), so a
reconnect with the same nickname restores its progress. Registering converts the
same live inventory into a real account instead of resetting it; logging into a
different account loads that account's profile instead.

Teams
-----
Teams are guilds: membership is unlimited and persisted in the `teams` and
`team_members` SQLite/PostgreSQL tables. Every account (guest UUID included) can
be in one team. The `Team` page of the vote menu lists the current guild, its
leader, online/offline members and the online teams that can be joined, plus the
online player list. `/team create <name>` founds a guild (the menu's reason box
works for a short name), `/team leave`, `/team disband` and
`/team ff on|off` manage it, and `/t <message>` is the team channel. A leader
who disconnects hands the guild to another online member; with nobody else
online the offline leader keeps it.

The scoreboard clan of every player is their team name, and is empty when they
are not in a team. The leader's friendly-fire switch only affects players who
share that team: with it off their direct hits, projectiles and explosions are
blocked; different teams and unteamed players can always fight.

Building locks
---------------
Placeable buildings carry an owner account and a lock:

- `self` — only the owner may use or dismantle it
- `team` — the owner and their current team may use or dismantle it
- `public` — anyone may

New drills and health jars start at `self`; legacy saves without a lock load as
`public`, preserving their old behaviour. Aim at a building while standing
within four tiles and the main menu shows its name, current lock and — for the
owner — the three lock actions directly on the top-level page. Drill control,
harvest collection, health-jar healing and damage/dismantling all consult the
lock. The owner/lock values are part of the world save. Scripts can read the
same state through `lastday_entity.allowed_player_near`, `can_use` and
`lock_mode`; the health jar uses `allowed_player_near` instead of owner-only
proximity.

Boss sanctums
-------------
Each generated euclidean floor carries exactly one `frost_sanctum` structure.
Its seed is derived from the floor name and the structure id, so the encounter
exists on one deterministic instance per floor rather than on every instance.
The catalog structure is `datasrc/maps/struct/frost_sanctum.json`; it pins a
theme variant per floor (frost, jungle, desert or grass) and places a vault plus
a boss and its guards. The world metadata now carries a `mobs` array, and the
server spawns it as non-natural content: neither the boss nor its summons take a
slot from the one-monster-per-48-tiles budget.

The boss (`datasrc/mobs/frost_warden.json`) has armor, a shotgun volley, a
leap and an enrage phase, and summons two spitters at 55% health. Its drops
include the Warden Sigil accessory, ancient cores, heaven ore and a chance at a
laser. The vault uses the existing relic loot table.

Killing the boss records its spawn group in the world save (`boss_dead`). On a
reload the whole group — boss and guards — is retired, so a cleared sanctum
stays cleared instead of silently refilling. `MOD_VERSION` was bumped so old
generated maps and packs are not reused; delete the old
`generatedmaps/LastDay/<old version>` and `mappacks/LastDay/<old version>`
directories if they are still present.

Fire content
------------
Flame weapons are built from the one projectile type the stock 0.7 client draws
without a bullet sprite: `WEAPON_HAMMER`. The client still emits a bullet-trail
particle for every live projectile of that type, and `CProjectile` gives it no
velocity on either side, so a short-lived stationary puff reads as an ember. The
flamethrower spawns a cone of those puffs; the flame cannon is the heavy,
wider, slower-firing variant. Both eat `fuel_canister` ammo.

The same particle backs the `campfire` scripted prop. Its base is a
laser-drawn stone ring with crossed sticks; each emission advances a phase from
that base outward, so a tall central cone visibly grows from the bottom. It emits
friendly puffs (owner sentinel `-4`) on a short timer, so it burns hostile bots but
never players, friendly units or neutral units. It is a proper building:
`spawn_owned` records the placer's account, the lock and the saved owner, and
the item drops back when the fire is dismantled. A campfire is persistent world
content; its flames are not. The scripts are
`datasrc/scripts/weapons/flamethrower.py`,
`datasrc/scripts/entities/campfire.py` and
`datasrc/scripts/items/campfire.py`; the recipes are under
`datasrc/craft/weapon` and `datasrc/craft/tool`.

World packs
-----------
A generated instance is a `.map` plus a `.json` sidecar holding its entrances,
resources, containers and ring geometry. A world pack bundles a shard of one floor into a
single datafile-v5 file, one item per map:

    header (magic, json size, map size, flags)
    world json, padded to 4 bytes
    the map as a complete v4 datafile, padded to 4 bytes

The pack index is item id 0 (json): format, floor, shard and one record per map
(name, seed, size, crc, sha256). v5 compresses the whole item section as one
zstd block, so every map is stored together with its metadata; each data blob,
if a pack uses any, is compressed on its own.

    mapgentool pack <pack-file> <floor> [seed...]   build a pack
    mapgentool unpack <pack-file>                   extract map + json again

Keep packs sharded (a few dozen instances): opening a pack decompresses all of
its items at once. The extracted map stays a v4 datafile, because 0.7 clients
cannot read v5 maps, so the server always serves the plain file it served
before.

A pack that is written to at runtime uses the append-only layout instead
(`src/engine/shared/mappack.*`): header, then per map one zstd blob and one
index record, chained backwards, with a footer naming the newest record. Reading
one map decompresses one blob; adding one appends a blob, a 96 byte record and
the 20 byte footer, and never rewrites what is already there. The footer is
written last, so an interrupted append leaves the previous content readable
(and only costs the space of the aborted blob). A datafile v5 cannot do this:
its item section is one zstd stream and its offset tables precede the payloads.

    mapgentool mappack add <pack-file> <floor> [seed...]
    mapgentool mappack list <pack-file>
    mapgentool mappack get <pack-file> <map-name>

A replaced name is a new record that supersedes the old one; the superseded
blob stays as garbage until the pack is compacted:

    mapgentool mappack compact <pack-file>

Compaction rebuilds the pack from its live entries into `<pack>.compact` and
renames it into place, so an interrupted run leaves the original untouched. It
is a maintenance action: a running server keeps the handle of the pack it
opened (on POSIX it keeps working and picks up the compacted file on restart).

One writer per pack: two concurrent appenders would race on the footer.

The generator uses one such pack per floor automatically
(`mappacks/LastDay/<MOD_VERSION>/<Floor>.mappack`, `sv_mappack 1`):

- every instance it generates is appended to its floor's pack right away;
- an instance is generated **into memory** (`CMapCreator::SaveMapToMemory`, the
  datafile writer itself writes through a stream), so no staging file exists at
  any point;
- the server loads an instance straight out of the pack
  (`CMapGen::AcquireMap` + `IEngineMap::LoadFromMemory`) and only writes
  `<name>.map`/`.json` cache files when tooling asks for them;
- a miss generates the instance, packs it, and serves it from memory;
- background pre-generation (`StartPreGeneration`, train chunks) checks the
  pack first: a rolled instance that is already packed is reused instead of
  generated again, and the worker re-checks right before generating;
- a cached map from before packs existed is appended once, the first time it is
  loaded, so the pack converges on the whole floor.

`mapgentool ensure` drives the cache-file path (`EnsureNewMap`) and
`mapgentool acquire` the in-memory path the server uses:

    mapgentool ensure Verdant 11     # generates and packs on a miss
    rm generatedmaps/.../Verdant_11.*
    mapgentool ensure Verdant 11     # restores from the pack, same bytes
    mapgentool acquire Verdant 22    # bytes straight from the pack, no file

The datafile reader, the datafile writer and the map loader all go through
`base/tl/stream.h`: a `file_stream` for a map on disk, a `memory_read_stream`
for a pack blob, and a `memory_stream` when the generator builds a map in
memory. The same code therefore serves a file, a pack entry and a freshly
generated instance.