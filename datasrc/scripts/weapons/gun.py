# Gun: one projectile per shot. Mirrors the native CWeaponGun.
import lastday

SOUND_GUN_FIRE = 0
WEAPON_GUN = 1


def on_fire(ctx):
    ammo = ctx.ammo
    extra = ctx.extra
    direction = ctx.dir

    damage = ammo.damage or extra.default_damage
    start = ctx.pos + direction * extra.proximity_radius * 0.75
    span = int(lastday.tuning("gun_lifetime") * ammo.lifetime * ctx.range_scale * ctx.tick_speed)

    ctx.Projectile(ctx.player, start, direction * ammo.speed, span, damage, False, -1, WEAPON_GUN)
    lastday.sound(ctx.pos.x, ctx.pos.y, SOUND_GUN_FIRE)
    return 0


def on_tick(ctx):
    pass


def on_switch(ctx):
    pass
