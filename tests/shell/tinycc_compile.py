from .common import case


CASES = [
    case(
        name="tinycc_math",
        must_contain=[
            "shelltest: tccmath_build begin",
            "shelltest: tccmath_build end",
            "shelltest: tccmath_run begin",
            "tcc math ok: add=7 fib=8 checksum=21 scratch=4 fact=120 bonus=7 total=167",
            "shelltest: tccmath_run end",
        ],
        timeout=120.0,
    ),
    case(
        name="tinycc_agg",
        must_contain=[
            "shelltest: tccagg_build begin",
            "shelltest: tccagg_build end",
            "shelltest: tccagg_run begin",
            "tcc agg ok: struct=22 pairret=22 nested=35 list=29 bundle=46 matrix=56 total=210",
            "shelltest: tccagg_run end",
        ],
        timeout=120.0,
    ),
    case(
        name="tinycc_tree",
        must_contain=[
            "shelltest: tcctree_build begin",
            "shelltest: tcctree_build end",
            "shelltest: tcctree_run begin",
            "tcc tree ok: tree=12 total=12",
            "shelltest: tcctree_run end",
        ],
        timeout=120.0,
    ),
    case(
        name="tinycc_compile",
        must_contain=[
            "shelltest: tccmini_build begin",
            "shelltest: tccmini_build end",
            "shelltest: tccmini_run begin",
            "tinycc ramp ok: add=7 fib=8 checksum=21 scratch=4 struct=22 pairret=22 nested=35 list=29 bundle=46 matrix=56 tree=12 fact=120 bonus=7 dispatch=24 total=450",
            "shelltest: tccmini_run end",
        ],
        timeout=120.0,
    ),
    case(
        name="tinycc_sysroot",
        must_contain=[
            "shelltest: tccsysroot_build begin",
            "shelltest: tccsysroot_build end",
            "shelltest: tccsysroot_run begin",
            "tcc sysroot ok: argc=1 stdout=1 name_len=18 total=20",
            "shelltest: tccsysroot_run end",
        ],
        timeout=120.0,
    ),
    case(
        name="tinycc_posix",
        must_contain=[
            "shelltest: tccposix_build begin",
            "shelltest: tccposix_build end",
            "shelltest: tccposix_run begin",
            "tcc system ok",
            "tcc posix stderr: no such file or directory",
            "tcc posix ok: io=1 stat=1 cwd=1 dir=1 time=1 headers=1 system=1 total=7",
            "shelltest: tccposix_run end",
        ],
        timeout=120.0,
    ),
]
