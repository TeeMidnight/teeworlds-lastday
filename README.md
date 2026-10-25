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

World packs
-----------
A generated instance is a `.map` plus a `.json` sidecar holding its entrances,
resources and ring geometry. A world pack bundles a shard of one floor into a
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