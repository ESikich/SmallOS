CASES = [
    {
        "name": "netinfo",
        "must_contain": [
            "shelltest: netinfo begin",
            "netinfo:",
            "nic:",
            "sockets:",
            "tcp:",
            "tcp buffers:",
            "shelltest: netinfo end",
        ],
    },
    {
        "name": "ip",
        "must_contain": [
            "shelltest: ip begin",
            "link:",
            "inet:",
            "gateway:",
            "shelltest: ip end",
        ],
    },
    {
        "name": "ip_addr",
        "must_contain": [
            "shelltest: ip_addr begin",
            "inet:",
            "shelltest: ip_addr end",
        ],
    },
    {
        "name": "ip_route",
        "must_contain": [
            "shelltest: ip_route begin",
            "route:",
            "shelltest: ip_route end",
        ],
    },
    {
        "name": "ip_dns",
        "must_contain": [
            "shelltest: ip_dns begin",
            "dns:",
            "shelltest: ip_dns end",
        ],
    },
    {
        "name": "ipconfig",
        "must_contain": [
            "shelltest: ipconfig begin",
            "link:",
            "route:",
            "sockets:",
            "shelltest: ipconfig end",
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
    {
        "name": "pinggw",
        "must_contain": [
            "shelltest: pinggw begin",
            "pinggw: 10.0.2.2 from 10.0.2.15",
            "ping: 10.0.2.2 reply",
            "pinggw: ok",
            "shelltest: pinggw end",
        ],
        "timeout": 60.0,
    },
    {
        "name": "ping",
        "must_contain": [
            "shelltest: ping begin",
            "ping: 10.0.2.2 from 10.0.2.15",
            "ping: 10.0.2.2 reply",
            "ping: ok",
            "shelltest: ping end",
        ],
        "timeout": 60.0,
    },
]
