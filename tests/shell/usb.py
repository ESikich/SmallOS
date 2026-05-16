CASES = [
    {
        "name": "usbports",
        "must_contain": [
            "shelltest: usbports begin",
            "usbports: passive port dump",
            "shelltest: usbports end",
        ],
    },
    {
        "name": "usbdiag",
        "must_contain": [
            "shelltest: usbdiag begin",
            "usbdiag: begin",
            "usbports: passive port dump",
            "usbdiag: dry-run connected non-low OHCI ports",
            "usbdiag: done",
            "shelltest: usbdiag end",
        ],
    },
]
