Import("env")

def force_gnu17(flags):
    result = []
    for flag in flags:
        if isinstance(flag, str) and flag.startswith("-std="):
            continue
        result.append(flag)
    if "-std=gnu++17" not in result:
        result.append("-std=gnu++17")
    return result

env["CCFLAGS"] = force_gnu17(env.get("CCFLAGS", []))
env["CXXFLAGS"] = force_gnu17(env.get("CXXFLAGS", []))
