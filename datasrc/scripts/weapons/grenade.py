"""Grenade: explosive projectile, full auto. Mirrors CWeaponGrenade."""

import lastday


SOUND_GRENADE_FIRE = 2
SOUND_GRENADE_EXPLODE = 6
WEAPON_GRENADE = 3
MUZZLE_OFFSET = 0.75


def on_fire(ctx):
    ammo = ctx.ammo
    extra = ctx.extra
    direction = ctx.dir
    damage = ammo.damage or extra.default_damage
    muzzle = ctx.pos + direction * extra.proximity_radius * MUZZLE_OFFSET
    lifetime_ticks = int(
        lastday.tuning("grenade_lifetime") * ammo.lifetime * ctx.range_scale * ctx.tick_speed
    )

    ctx.Projectile(
        ctx.player,
        muzzle,
        direction * ammo.speed,
        lifetime_ticks,
        damage,
        True,
        SOUND_GRENADE_EXPLODE,
        WEAPON_GRENADE,
    )
    lastday.sound(ctx.pos.x, ctx.pos.y, SOUND_GRENADE_FIRE)
    return 0
