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

Generated maps are cached by name: after changing a floor's generation settings,
delete `generatedmaps/<Floor>_<Seed>.map` from the storage save path so the next
request regenerates it. `mapgentool worldjson <Map>` prints the ring geometry a
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
unique private CWorldCore id, so a snapshot tee id names exactly one bot. Bots
are not bound as proxy cores, so a hook cannot reach one across the seam until it
crosses with RingCrosses and is handed to the neighbour world.