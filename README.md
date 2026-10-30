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
- The train always arrives in the middle of a destination ring: 512 for a
  1024-instance floor. Boarding no longer rolls a random chunk.

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

A bot leaves the manager through CBotManager::ForgetBot(), which is idempotent
and is called from the entity destructor as well as from Die(). A bot can also
disappear when its world is unloaded, when a worm segment is torn down, or when
a script removes it; a leftover table entry would be followed by the next world
unload and read a freed object. CGameWorld's destructor therefore retires any
bot the unload sweep did not claim before deleting the entity, and unloading a
world drops every neighbouring proxy that still borrows it.

Per-Tee tuning
--------------
Water, silent step and the accessory/monster speed bonus are no longer special
cases inside CCharacterCore::Tick. The server builds one CTuningParams per tee
each tick (BuildCharacterTuning), simulates with it, and snaps it as a Tuning
object keyed by that tee's TeeInfoID. A client predicts each tee with its own
object, so water and bot movement match the server. The local tee always gets
an object (even as a spectator, as the prediction fallback); a 0.7 client
receives the local tee's values as its single De_TuneParams.

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

Players can also craft and place their own container. The `crate`, `chest` and
`vault` items carry a `container.kind` field; using Place in the item view
spawns that kind on the floor in front of the player. A placed container gets a
negative storage id (`-1`, `-2`, ...), so it can never collide with the
non-negative ids of map structures; its kind, position and contents are stored
in the same world save and it comes back on reload. Placing a container does not
roll a loot table: it starts empty. An emptied placed container can be picked up
again from its Container page and becomes the item once more; map containers are
world content and cannot be taken.

Stack limits and backpacks
--------------------------
Each item definition may declare `"max_stack": N` (0 or omitted means
unlimited). Weapons, accessories and backpacks are always limited to one copy
per stack regardless of this field. Adding an item fills existing stacks up to
their limit first and then uses empty slots; removing counts across every
matching stack. Rows already saved with a larger count are left alone, so an old
save is never rewritten or truncated by the new limit.

A backpack is an item with a fixed grid:

    "backpack": { "slots": 8 }

The grid is a per-stack `data` payload. It is saved under the item's own
`inventory.<n>.data` key (`slots` plus an `items` array with stable slot
indices), never as ordinary nested inventory. On the Inventory page the grid is
drawn as indented `    ↳` rows directly below its backpack row; clicking a row
takes that stack back into the inventory, and an item's detail page offers
"Store in backpack". Backpacks do not nest.

Dropping a backpack keeps its grid on the ground pickup, and the world save
writes the same payload under the drop's `data` field, so the contents survive
both a pickup and a server restart. `data` is also why a backpack cannot be
stored in a container or listed in the shop: those records only carry an item id
and a count.

Mobs, factions and dialogue
--------------------------
Monsters are content, not C++ classes. One file per creature lives in
`datasrc/mobs/<id>.json` and defines:

- identity, skin, health and armor
- `faction`: `hostile`, `neutral` or `friendly`
- `ai`: `hunter`, `skirmisher`, `sentry`, `wanderer`, `companion` or
  `support`
- abilities: melee, ranged projectile (with an optional `spread` in degrees
  so a shot can be strafed), leap, heal aura, armor aura, enrage
- localized `dialogue` events (spawn, sight, combat, hurt, kill, death, idle,
  low health), with `{player}` substitution and per-event cooldowns
- weighted `drops`; each entry is a normal item id with a count range and a
  chance
- `money`: a G bundle (`min`, `max`, `piles`, `chance`) that scatters into
  physical piles. Money is not an item; each pile is rendered as a weapon
  projectile and credited to the picking player's account ledger

Money, shops and buy orders
---------------------------
Currency is an account ledger, not an inventory stack. It is credited from
monster money drops and from NPC buy orders, and debited by player listings and
NPC shelves.

- A configured NPC may list `buys` next to `sells` in
  `datasrc/bots/npc/<npc_id>.json`. Each entry names the `item`, the package
  `count`, the `price` in G, how many packages the merchant will take
  (`limit`) and `restock_seconds`. The runtime demand is persisted in
  `npc_buy_stock`, so restarting the server does not refill a satisfied
  merchant.
- In the Shop page, expanding a merchant shows its sell shelf and its buy
  orders. Clicking the price row of a buy order removes the package from the
  player's inventory and credits the G in one step; if either side fails, the
  items are put back.
- Money pickups are batched per player over half a second, so a scattered pile
  produces one `+N G` chat notice instead of a line per coin.


Faction rules are deliberately asymmetric. A hostile mob hunts players but
does not start a fight with any other bot: other monsters, whatever their
faction or group, and town NPCs are ignored until one of them damages it.
Every bot remembers the exact attacker for a while, so a stray hit between two
otherwise neutral monsters turns into a duel. A neutral mob does nothing until
attacked, then retaliates for a while. A friendly unit attacks hostile mobs on
sight and never hurts a player; the hostile mob is then free to fight back. The
same rules apply to configured town NPCs: a friendly Medic
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
shared health, movement, contact damage and loot. It alternates between a
surface phase and a burrowed phase (`burrow_ticks` / `surface_ticks` in the
mob JSON). While surfaced it crawls along the first floor below it, is visible
and hittable, and bites nearby players. When it burrows the links disappear,
terrain no longer constrains it, and it tunnels toward its prey while leaving
small dirt puffs on the surface. It is invulnerable until it breaks the
surface again, so the fight has a clear strike window; any body link routes
damage to the whole creature.

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
be in one team. Membership is invitation-only: a leader invites an online player
from the `Team` page (or with `/team invite <player>`), and the invited player
accepts or declines there. The same page lists the current guild, its leader,
online/offline members, pending session invitations and the online teams, plus
the online player list. Offline members are shown by the nickname stored in their
player JSON profile, not by UUID. `/team create <name>` founds a guild (the
menu's reason box works for a short name), `/team accept` and `/team decline`
answer an invitation, and `/team leave`, `/team disband` and
`/team ff on|off` manage the guild; `/t <message>` is the team channel. A
leader leaving the team or disconnecting disbands the guild immediately, so a
team never lingers without someone who can manage it.

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
The catalog structure pins `"instance": 1`, so the encounter always exists on
ring chunk 1 of the floor rather than on a hash-derived instance. It also pins a
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
Flame weapons are built from a dedicated fire particle,
`CFireParticle`, which snaps as the one projectile type the stock 0.7 client
draws without a bullet sprite: `WEAPON_HAMMER`. The client still emits a
bullet-trail particle for every live projectile of that type, so a flying puff
reads as an ember. Each puff keeps its own velocity and is pulled down by
gravity: the spray arcs, stops on the first solid tile and then lingers there
for a short fade before it disappears. The flamethrower spawns a cone of those
puffs; the flame cannon is the heavy, wider, slower-firing variant. Both eat
`fuel_canister` ammo.

The same particle backs the `campfire` scripted prop. Its base is two
crossed lasers (the logs); each emission advances a phase from the top ends of
that cross, so a tall central flame visibly grows from the bottom, widest
at the base and tapering to the tip. Its puffs stay stationary (zero velocity
and zero gravity), so the phase wave, not physics, controls the silhouette. It
emits friendly puffs (owner sentinel
`-4`) on a short timer, so it burns hostile bots but never players, friendly
units or neutral units. It is a proper building:
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
  (`CMapGen::TryAcquireMap` + `IEngineMap::LoadFromMemory`) and only writes
  `<name>.map`/`.json` cache files when tooling asks for them;
- a miss never generates on the server tick thread: the game layer queues the
  instance with `CMapGen::PrefetchMap` and retries the world load on a later
  tick. Server generation only runs on the worker;
- background pre-generation (`StartPreGeneration`, train chunks, ring
  lookahead and entrance targets) checks the pack first: an instance that is
  already packed is reused instead of generated again, and the worker
  re-checks right before generating;
- a cached map from before packs existed is appended once, the first time it is
  loaded, so the pack converges on the whole floor.

The datafile codec prints which file it opens and which layers it decompresses.
Generation opens many base and structure maps, so the generator silences those
progress lines. The generator itself is quiet as well: with the switches off, a
generation reports one `generated <floor>_<seed>` or `failed to generate ...`
line. `dbg_mapgen 1` enables the detailed mapgen/worldgen/automap output
(including the datafile lines of a generation); `dbg_datafile 1` enables the
datafile lines for every map. Datafile errors are always printed.

`mapgentool ensure` drives the cache-file path (`EnsureNewMap`) and
`mapgentool acquire` the in-memory path through `AcquireMapOrGenerate`
(`TryAcquireMap` when the instance already exists):

    mapgentool ensure Verdant 11     # generates and packs on a miss
    rm generatedmaps/.../Verdant_11.*
    mapgentool ensure Verdant 11     # restores from the pack, same bytes
    mapgentool acquire Verdant 22    # bytes straight from the pack, no file

The datafile reader, the datafile writer and the map loader all go through
`base/tl/stream.h`: a `file_stream` for a map on disk, a `memory_read_stream`
for a pack blob, and a `memory_stream` when the generator builds a map in
memory. The same code therefore serves a file, a pack entry and a freshly
generated instance.