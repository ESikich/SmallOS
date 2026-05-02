from .common import case


CASES = [
    case(
        name="ptrguard",
        command="runelf apps/tests/ptrguard alpha beta",
        must_contain=[
            "ptrguard: start",
            "sys_write invalid buf",
            "sys_read invalid buf",
            "sys_open invalid name",
            "sys_open_mode invalid name",
            "sys_writefile invalid name",
            "sys_writefile_path invalid name",
            "sys_writefile_path invalid buf",
            "sys_open hello",
            "sys_fread invalid buf",
            "sys_exec invalid argv",
            "=== ptrguard PASS ===",
        ],
        timeout=60.0,
    )
]
