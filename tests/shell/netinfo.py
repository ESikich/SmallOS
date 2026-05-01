CASES = [
    {
        "name": "netinfo",
        "must_contain": [
            "shelltest: netinfo begin",
            "netinfo:",
            "e1000:",
            "shelltest: netinfo end",
        ],
    },
    {
        "name": "netsend",
        "must_contain": [
            "shelltest: netsend begin",
            "netsend: queued test frame",
            "shelltest: netsend end",
        ],
    },
    {
        "name": "netrecv",
        "must_contain": [
            "shelltest: netrecv begin",
            "netrecv: no packet",
            "shelltest: netrecv end",
        ],
    },
    {
        "name": "arpgw",
        "must_contain": [
            "shelltest: arpgw begin",
            "arpgw: who-has 10.0.2.2 from 10.0.2.15",
            "arpgw:",
            "is-at",
            "shelltest: arpgw end",
        ],
        "timeout": 60.0,
    },
]
