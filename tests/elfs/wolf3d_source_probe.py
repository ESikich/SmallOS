from .common import case


CASES = [
    case(
        name="wolf3d_source_probe",
        command="runelf usr/libexec/tests/wolf3d-srcprobe",
        must_contain=[
            "wolf3d-source-probe: upstream startup path linked",
        ],
    ),
    case(
        name="wolf3d_source_probe_check_episodes",
        command="runelf usr/libexec/tests/wolf3d-srcprobe --check-episodes",
        must_contain=[
            "wolf3d-source-probe: upstream data extension WL1",
        ],
    ),
    case(
        name="wolf3d_source_probe_init_game",
        command="runelf usr/libexec/tests/wolf3d-srcprobe --init-game",
        must_contain=[
            "wolf3d-source-probe: upstream InitGame completed WL1",
        ],
        timeout=60.0,
    ),
    case(
        name="wolf3d_source_probe_demo_preamble",
        command="runelf usr/libexec/tests/wolf3d-srcprobe --demo-preamble",
        must_contain=[
            "wolf3d-source-probe: upstream DemoLoop prelude completed WL1",
        ],
        timeout=60.0,
    ),
]
