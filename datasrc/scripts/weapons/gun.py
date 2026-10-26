"""Gun: one projectile per shot. Mirrors the native CWeaponGun."""

import lastday


SOUND_GUN_FIRE = 0
WEAPON_GUN = 1
MUZZLE_OFFSET = 0.75


def on_fire(ctx):
    ammo = ctx.ammo
    extra = ctx.extra
    direction = ctx.dir
    damage = ammo.damage or extra.default_damage
    muzzle = ctx.pos + direction * extra.proximity_radius * MUZZLE_OFFSET
    lifetime_ticks = int(
        lastday.tuning("gun_lifetime") * ammo.lifetime * ctx.range_scale * ctx.tick_speed
    )

    ctx.Projectile(
        ctx.player,
        muzzle,
        direction * ammo.speed,
        lifetime_ticks,
        damage,
        False,
        -1,
        WEAPON_GUN,
    )
    lastday.sound(ctx.pos.x, ctx.pos.y, SOUND_GUN_FIRE)
    return 0
