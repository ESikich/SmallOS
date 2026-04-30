from .common import case


CASES = [
    case(
        name="mkdir_rmdir",
        must_contain=[
            "shelltest: mkdir begin",
            "mkdir: TESTDIR",
            "shelltest: mkdir end",
            "shelltest: fsls_newdir begin",
            "fat16 directory: TESTDIR",
            "shelltest: fsls_newdir end",
            "shelltest: rmdir begin",
            "rmdir: TESTDIR",
            "shelltest: rmdir end",
            "shelltest: fsls_removed begin",
            "fat16: not found: TESTDIR",
            "shelltest: fsls_removed end",
        ],
        timeout=60.0,
    ),
    case(
        name="mkdir_rmdir_nested",
        must_contain=[
            "shelltest: mkdir_nested_parent begin",
            "mkdir: NESTPARENT",
            "shelltest: mkdir_nested_parent end",
            "shelltest: mkdir_nested_child begin",
            "mkdir: NESTPARENT/CHILD",
            "shelltest: mkdir_nested_child end",
            "shelltest: fsls_nested begin",
            "fat16 directory: NESTPARENT",
            "CHILD/",
            "shelltest: fsls_nested end",
            "shelltest: rmdir_nested_child begin",
            "rmdir: NESTPARENT/CHILD",
            "shelltest: rmdir_nested_child end",
            "shelltest: rmdir_nested_parent begin",
            "rmdir: NESTPARENT",
            "shelltest: rmdir_nested_parent end",
            "shelltest: fsls_nested_removed begin",
            "fat16: not found: NESTPARENT",
            "shelltest: fsls_nested_removed end",
        ],
        timeout=60.0,
    ),
]
