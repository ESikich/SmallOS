from .common import case


CASES = [
    case(
        name="args",
        command="runelf args alpha beta",
        must_contain=[
            "argc = 3",
            "argv[0] = args",
            "argv[1] = alpha",
            "argv[2] = beta",
        ],
    )
]
