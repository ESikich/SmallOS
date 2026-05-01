from .common import case


CASES = [
    case(
        name="cp_mv",
        must_contain=[
            "shelltest: compiler_demo begin",
            "compiler_demo PASS",
            "shelltest: compiler_demo end",
            "shelltest: cp begin",
            "cp: compiler.out -> compiler.copy",
            "shelltest: cp end",
            "shelltest: fsread_copy begin",
            "fsread: compiler.copy",
            "shelltest: fsread_copy end",
            "shelltest: mv begin",
            "mv: compiler.copy -> compiler.moved",
            "shelltest: mv end",
            "shelltest: fsread_moved begin",
            "fsread: compiler.moved",
            "shelltest: fsread_moved end",
            "shelltest: cp_dir begin",
            "cp: compiler.out -> apps/demo",
            "shelltest: cp_dir end",
            "shelltest: fsread_dir_copy begin",
            "fsread: apps/demo/compiler.out",
            "shelltest: fsread_dir_copy end",
            "shelltest: rm_dir begin",
            "rm: apps/demo/compiler.out",
            "shelltest: rm_dir end",
            "shelltest: fsread_dir_removed begin",
            "fat16: not found: apps/demo/compiler.out",
            "fsread: load failed",
            "shelltest: fsread_dir_removed end",
            "shelltest: mv_dir begin",
            "mv: compiler.moved -> apps/demo",
            "shelltest: mv_dir end",
        ],
        timeout=60.0,
    )
]
