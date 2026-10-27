"""Runtime self-test used by the script_selftest console command.

It touches the class context, the constructor wrappers and the
state/result/consumed bridge. A missing world makes the snapshot-id and
placement checks return -1/false rather than fail, which is expected on the
console.
"""

import lastday
import lastday_entity


def on_selftest(ctx):
    origin = ctx.Vec2(0.0, 0.0)
    right = ctx.Vec2(1.0, 0.0)
    down = ctx.Vec2(0.0, 1.0)

    ctx.state["ok"] = True
    ctx.result["answer"] = 42
    ctx.result["pos_x"] = ctx.pos.x
    ctx.result["dir_len"] = ctx.dir.length()
    ctx.result["ammo_speed"] = ctx.ammo.speed
    ctx.result["extra_missing"] = ctx.extra.missing

    projectile = ctx.Projectile(None, origin, right, 10, 1, False, -1, 0)
    laser = ctx.Laser(None, origin, right, 100.0, 1)
    health = ctx.HealthDrop(origin, down)
    pickup = ctx.DroppedPickup(origin, origin, "lastday:bandage", 1)
    swing = ctx.MeleeSwing(None, origin, right, 1, 1.0, False, 0, True)

    ctx.result["ctor"] = [projectile.id, laser.id, health.id, pickup.id, swing.hits]
    ctx.result["player_id"] = ctx.Player(3).id
    ctx.result["drop_near"] = ctx.HealthDrop.near(origin, 96.0)

    # placement metadata and world probes (no world in the console test)
    ctx.result["item_drill"] = lastday.item_drill("lastday:iron_drill")
    ctx.result["item_jar"] = lastday.item_health_jar("lastday:health_jar")
    ctx.result["jar_size"] = lastday_entity.health_jar_size()
    ctx.result["floor"] = lastday.world_floor_y(0.0, 0.0, 128.0)
    ctx.result["line"] = lastday.world_test_line(0.0, 0.0, 32.0, 0.0, 4.0)
    ctx.result["drill_near"] = lastday.drill_near(0.0, 0.0, 40.0)
    ctx.result["kind_near"] = lastday_entity.kind_near("campfire", 0.0, 0.0, 64.0)
    # no world in the console test: this must fail cleanly with -1
    ctx.result["spawn_owned"] = lastday_entity.spawn_owned(
        "campfire", 0.0, 0.0, None, -1, 1, 0, 1
    )

    # per-floor snapshot id allocation through the host bridge
    snap_id = ctx.snap_new_id()
    ctx.result["snap_id"] = snap_id
    ctx.result["snap_freed"] = ctx.snap_free_id(snap_id) if snap_id >= 0 else False
    ctx.result["world"] = ctx.world

    ctx.consumed = True
    return True
