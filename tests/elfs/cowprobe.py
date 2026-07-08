from .common import case


CASES = [
    case(
        name="cowprobe",
        command="usr/libexec/tests/cowprobe",
        must_contain=[
            "cowprobe demand reserve: PASS",
            "cowprobe demand touch: PASS",
            "cowprobe fork chain: PASS",
            "cowprobe file private: PASS",
            "cowprobe munmap split: PASS",
            "cowprobe mprotect fork: PASS",
            "cowprobe fork frame drop: PASS",
            "cowprobe parent globals: PASS",
            "cowprobe parent heap: PASS",
            "cowprobe parent mmap: PASS",
            "cowprobe parent stack: PASS",
            "cowprobe parent writes: PASS",
            "cowprobe mprotect write: PASS",
            "cowprobe wait: PASS",
            "cowprobe child frames released: PASS",
            "cowprobe: PASS",
        ],
        timeout=90.0,
    )
]
