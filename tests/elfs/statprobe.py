from .common import case


CASES = [
    case(
        name="statprobe",
        command="runelf apps/tests/statprobe",
        must_contain=[
            "statprobe start",
            "hello: ok size=",
            "demo_dir: ok size=0 dir=1",
            "demo_hello: ok size=",
            "statprobe PASS",
        ],
    )
]
