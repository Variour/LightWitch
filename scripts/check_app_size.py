Import("env")
import os

# Fails the build when firmware.bin is larger than the app partition
# (upload.maximum_size, which the platform sets from the partition table).
# Checks the real image size; PlatformIO's own report sums ELF sections.


def _check_app_size(target, source, env):
    image = target[0].get_abspath()
    size = os.path.getsize(image)
    limit = int(env.BoardConfig().get("upload.maximum_size"))
    if size > limit:
        print(f"[app-size] ERROR: {os.path.basename(image)} is {size} bytes, "
              f"the app partition holds {limit} bytes ({size - limit} bytes over)")
        return 1
    print(f"[app-size] {os.path.basename(image)}: {size} of {limit} bytes "
          f"({limit - size} bytes free)")
    return 0


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _check_app_size)
