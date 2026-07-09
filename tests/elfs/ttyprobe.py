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
            "ttyname_r slave: PASS",
            "ptsname_r master: PASS",
            "ioctl get pgrp: PASS",
            "tcgetattr enotty: PASS",
            "ttyname_r enotty: PASS",
            "ttyprobe PASS",
        ],
    ),
]
