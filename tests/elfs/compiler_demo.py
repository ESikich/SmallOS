from .common import case


CASES = [
    case(
        name="compiler_demo",
        command="runelf compiler_demo",
        must_contain=[
            "compiler_demo start",
            "writefile: ok",
            "readback: ok",
            "compiler_demo PASS",
        ],
    )
]
