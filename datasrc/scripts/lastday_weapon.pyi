# Type hints for the embedded PocketPy module "lastday_weapon".


def melee_attack(
    owner: int,
    x: float,
    y: float,
    dx: float,
    dy: float,
    damage: int,
    range_scale: float,
    can_harvest: bool,
    hit_style: int,
    swing_effect: bool,
) -> int: ...
