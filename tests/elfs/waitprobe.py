from .common import case


CASES = [
    case(
        name="waitprobe",
        command="runelf usr/libexec/tests/waitprobe",
        must_contain=[
            "waitprobe start",
            "getpid positive: PASS",
            "sys_exec pid",
            ": PASS",
            "waitpid child: PASS",
            "waitpid exited: PASS",
            "waitpid status: PASS",
            "waitpid no child: PASS",
            "waitpid no child errno: PASS",
            "kill child spawn: PASS",
            "waitpid wnohang: PASS",
            "kill child: PASS",
            "waitpid killed child: PASS",
            "waitpid signaled: PASS",
            "waitpid termsig: PASS",
            "waitprobe PASS",
        ],
        timeout=60.0,
    )
]
