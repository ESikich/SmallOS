from .common import case


CASES = [
    case(
        "jobctlprobe",
        "usr/libexec/tests/jobctlprobe",
        must_contain=[
            "jobctlprobe start",
            "fork child: PASS",
            "setpgid child: PASS",
            "child pgid: PASS",
            "killpg stop: PASS",
            "waitpid stopped child: PASS",
            "waitpid stopped: PASS",
            "waitpid stopsig: PASS",
            "stopped reported once: PASS",
            "killpg cont: PASS",
            "continued child running: PASS",
            "kill child term: PASS",
            "waitpid killed child: PASS",
            "waitpid signaled: PASS",
            "waitpid termsig: PASS",
            "jobctlprobe PASS",
        ],
        timeout=60.0,
    ),
]
