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
]
