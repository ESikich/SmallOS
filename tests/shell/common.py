def case(name, must_contain=None, timeout=30.0):
    return {
        "name": name,
        "must_contain": list(must_contain or []),
        "timeout": timeout,
    }
