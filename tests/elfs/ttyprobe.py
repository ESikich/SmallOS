from .common import case


CASES = [
    case(
        "ttyprobe",
        "usr/libexec/tests/ttyprobe",
        must_contain=[
            "ttyprobe start",
            "tcgetattr default: PASS",
            "echo default: PASS",
            "echo off quiet: PASS",
            "raw read first byte: PASS",
            "ctrl-c delivered as data: PASS",
            "ctrl-d canonical eof: PASS",
            "ioctl winsize fd: PASS",
            "ioctl get pgrp: PASS",
            "tcgetattr enotty: PASS",
            "ttyprobe PASS",
        ],
    ),
]
