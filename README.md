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
- Entities: dropped pickups, projectiles and bots cross at the same trigger;
  the live neighbours' players, drops and projectiles are also rendered in the
  current map, and hits and pickups reach across the boundary.
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