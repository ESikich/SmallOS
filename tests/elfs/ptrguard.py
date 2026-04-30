from .common import case


CASES = [
    case(
        name="ptrguard",
        command="runelf ptrguard",
        must_contain=[
            "ptrguard: start",
            "sys_write invalid buf",
            "sys_read invalid buf",
            "sys_open invalid name",
            "sys_writefile invalid name",
            "sys_open hello",
            "sys_fread invalid buf",
            "sys_exec invalid argv",
            "=== ptrguard PASS ===",
        ],
        timeout=60.0,
    )
]
