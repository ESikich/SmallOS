from .common import case


CASES = [
    case(
        name="wolf3d_check",
        must_contain=[
            "shelltest: wolf3d_check begin",
            "wolf3d-source-probe: upstream data extension WL1",
            "shelltest: wolf3d_check end",
        ],
        timeout=60.0,
    )
]
