Import("env")
import gzip as gz, shutil, os

# Extensions worth compressing. Only files the web server hands out may ship
# gzip-only: `serveStatic` and `AsyncFileResponse` fall back to a `.gz` sibling
# on their own. Anything read through a plain `LittleFS.open()` — `wifi.json`,
# scenes, playlists — has no such fallback, so `.json` is deliberately absent.
COMPRESSIBLE = {".html", ".css", ".js", ".svg", ".txt"}

# The image is packed from a staging copy under $BUILD_DIR instead of from
# `data/` itself, so the working tree keeps its readable sources and any
# untracked files a developer has there. The replacement has to happen while
# this script is imported — the platform builder turns $PROJECT_DATA_DIR into
# a node when it constructs the buildfs target, after `pre:` scripts have run.
STAGING = os.path.join(env.subst("$BUILD_DIR"), "fsdata")

os.makedirs(STAGING, exist_ok=True)
env.Replace(PROJECT_DATA_DIR=STAGING)


def stage_assets(source, target, env):
    data_dir = os.path.join(env.subst("$PROJECT_DIR"), "data")
    if not os.path.isdir(data_dir):
        return

    shutil.rmtree(STAGING, ignore_errors=True)
    shutil.copytree(data_dir, STAGING)

    for fname in sorted(os.listdir(STAGING)):
        src = os.path.join(STAGING, fname)
        if not os.path.isfile(src):
            continue
        if os.path.splitext(fname)[1].lower() not in COMPRESSIBLE:
            continue
        dst = src + ".gz"
        with open(src, "rb") as f_in, gz.open(dst, "wb", compresslevel=9) as f_out:
            shutil.copyfileobj(f_in, f_out)
        orig, comp = os.path.getsize(src), os.path.getsize(dst)
        # Ship the compressed file alone — carrying the raw sibling as well
        # costs more than the whole 320 KB partition holds.
        os.remove(src)
        print(f"[pre-uploadfs] gzip {fname}: {orig} B → {comp} B ({comp * 100 // orig}%)")

    total = sum(os.path.getsize(os.path.join(root, f))
                for root, _, files in os.walk(STAGING) for f in files)
    print(f"[pre-uploadfs] staged {total} B for the filesystem image")


env.AddPreAction("buildfs", stage_assets)
env.AddPreAction("uploadfs", stage_assets)
