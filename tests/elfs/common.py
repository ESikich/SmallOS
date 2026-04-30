def case(name, command, must_contain=None, interactive=None, timeout=30.0):
    return {
        "name": name,
        "command": command,
        "must_contain": list(must_contain or []),
        "interactive": list(interactive or []),
        "timeout": timeout,
    }
