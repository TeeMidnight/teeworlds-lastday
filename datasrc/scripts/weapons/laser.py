"""Laser: hitscan beam, full auto."""

import lastday


SOUND_LASER_FIRE = 8


def on_fire(ctx):
    ammo = ctx.ammo
    extra = ctx.extra
    damage = ammo.damage or extra.default_damage
    reach = lastday.tuning("laser_reach") * ammo.speed * ctx.range_scale

    ctx.Laser(ctx.player, ctx.pos, ctx.dir, reach, damage)
    lastday.sound(ctx.pos.x, ctx.pos.y, SOUND_LASER_FIRE)
    return 0
