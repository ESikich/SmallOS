from .common import case


CASES = [
    case(
        name="edit",
        must_contain=[
            "shelltest: edit begin",
            "edit: wrote var/tmp/EDIT.TXT",
            "shelltest: edit end",
            "shelltest: cat_edit begin",
            "first-line",
            "second-line",
            "shelltest: cat_edit end",
        ],
        timeout=60.0,
    )
]
