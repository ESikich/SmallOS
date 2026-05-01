from .common import case


CASES = [
    case(
        name="heapprobe",
        command="runelf heapprobe",
        must_contain=[
            "heapprobe start",
            "malloc a: PASS",
            "malloc b: PASS",
            "calloc c: PASS",
            "calloc zeroed: PASS",
            "realloc a: PASS",
            "realloc preserved: PASS",
            "reuse after free: PASS",
            "heapprobe PASS",
        ],
    )
]
