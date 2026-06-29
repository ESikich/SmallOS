from .common import case


CASES = [
    case(
        name="tree",
        must_contain=[
            "shelltest: tree begin",
            "/",
            "├── bin/",
            "├── usr/",
            "│   │   ├── hello",
            "└── var/",
            "directories, ",
            " files",
            "shelltest: tree end",
        ],
        timeout=60.0,
    ),
    case(
        name="tree_path",
        must_contain=[
            "shelltest: tree_path begin",
            "usr/bin",
            "├── hello",
            "├── mandel",
            "├── plasma",
            "shelltest: tree_path end",
        ],
        timeout=60.0,
    ),
]
