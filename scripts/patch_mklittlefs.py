Import("env")
import os, sys

# The espressif32 platform hardcodes the mklittlefs command with no flags variable,
# so we wrap the binary to inject --name_max 64.  This matches the arduino-esp32
# runtime default (LFS_NAME_MAX=64), allowing filenames up to 64 chars
# (e.g. 32-char UUID + ".json" = 37 chars).
real = env.subst("$MKFSTOOL")
if not real or "mklittlefs" not in real.lower():
    print("[patch-mklittlefs] WARNING: MKFSTOOL not set or not mklittlefs — skipping patch")
else:
    build_dir = env.subst("$BUILD_DIR")
    os.makedirs(build_dir, exist_ok=True)
    wrap = os.path.join(build_dir, "mklittlefs_wrap.sh")
    with open(wrap, "w") as f:
        f.write(f'#!/bin/sh\nexec "{real}" --name_max 64 "$@"\n')
    os.chmod(wrap, 0o755)
    env.Replace(MKFSTOOL=wrap)
    print(f"[patch-mklittlefs] mklittlefs will use --name_max 64")
