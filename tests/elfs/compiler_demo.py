from .common import case


CASES = [
    case(
        name="compiler_demo",
        command="runelf apps/tests/compiler_demo alpha beta",
        must_contain=[
            "compiler_demo start",
            "writefile: ok",
            "readback: ok",
            "writefile_path: ok",
            "nested readback: ok",
            "compiler_demo PASS",
        ],
    )
]
