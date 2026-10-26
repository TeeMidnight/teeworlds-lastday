"""Runtime self-test used by the script_selftest console command.

It touches the class context, the constructor wrappers and the
state/result/consumed bridge. A missing world makes the snapshot-id check
return -1 rather than fail, which is the expected console behaviour.
"""


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

    # per-floor snapshot id allocation through the host bridge
    snap_id = ctx.snap_new_id()
    ctx.result["snap_id"] = snap_id
    ctx.result["snap_freed"] = ctx.snap_free_id(snap_id) if snap_id >= 0 else False
    ctx.result["world"] = ctx.world

    ctx.consumed = True
    return True
