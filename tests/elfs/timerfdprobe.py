from .common import case


CASES = [
    case(
        name="timerfdprobe",
        command="runelf apps/tests/timerfdprobe",
        must_contain=[
            "timerfdprobe start",
            "timerfdprobe poll ok",
            "timerfdprobe epoll ok",
            "timerfdprobe PASS",
        ],
        timeout=60.0,
    )
]
