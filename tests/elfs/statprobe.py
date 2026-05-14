from .common import case


CASES = [
    case(
        name="statprobe",
        command="runelf usr/libexec/tests/statprobe alpha beta",
        must_contain=[
            "statprobe start",
            "hello: ok size=",
            "demo_dir: ok size=0 dir=1",
            "demo_hello: ok size=",
            "statprobe posix dir: PASS",
            "statprobe metadata: PASS",
            "statprobe access: PASS",
            "statprobe PASS",
        ],
    )
]
