from .common import case


CASES = [
    case(
        name="badptrprobe",
        command="runelf apps/tests/badptrprobe",
        must_contain=[
            "badptrprobe start",
            "write unmapped buffer: PASS",
            "open unmapped path: PASS",
            "exec unmapped argv: PASS",
            "open fixture: PASS",
            "fread unmapped buffer: PASS",
            "close fixture: PASS",
            "stat unmapped size out: PASS",
            "stat unmapped dir out: PASS",
            "terminal size bad rows: PASS",
            "terminal size bad cols: PASS",
            "poll overflow nfds: PASS",
            "epoll create: PASS",
            "epoll wait too many: PASS",
            "epoll wait bad events: PASS",
            "close epoll: PASS",
            "badptrprobe PASS",
        ],
        timeout=60.0,
    )
]
