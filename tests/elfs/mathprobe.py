from .common import case


CASES = [
    case(
        name="mathprobe",
        command="usr/libexec/tests/mathprobe",
        must_contain=[
            "mathprobe start",
            "mathprobe sin0: PASS",
            "mathprobe cos0: PASS",
            "mathprobe sin_pi: PASS",
            "mathprobe cos_pi: PASS",
            "mathprobe sin_half_pi: PASS",
            "mathprobe cos_half_pi: PASS",
            "mathprobe sin_three_halves_pi: PASS",
            "mathprobe cos_two_pi: PASS",
            "mathprobe tan0: PASS",
            "mathprobe PASS",
        ],
    )
]
