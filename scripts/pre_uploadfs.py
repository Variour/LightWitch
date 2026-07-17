Import("env")
import gzip as gz, shutil, os

COMPRESSIBLE = {".html", ".css", ".js", ".json", ".svg", ".txt"}

def _data_dir(env):
    return os.path.join(env.subst("$PROJECT_DIR"), "data")

def gzip_assets(source, target, env):
    data_dir = _data_dir(env)
    if not os.path.isdir(data_dir):
        return
    for fname in os.listdir(data_dir):
        if os.path.splitext(fname)[1].lower() not in COMPRESSIBLE:
            continue
        src = os.path.join(data_dir, fname)
        dst = src + ".gz"
        with open(src, "rb") as f_in, gz.open(dst, "wb", compresslevel=9) as f_out:
            shutil.copyfileobj(f_in, f_out)
        orig, comp = os.path.getsize(src), os.path.getsize(dst)
        print(f"[pre-uploadfs] gzip {fname}: {orig} B → {comp} B ({comp * 100 // orig}%)")

def cleanup_gzip_assets(source, target, env):
    # Remove the .gz siblings created by gzip_assets() so they don't linger
    # in data/ and get double-packed into the next unrelated buildfs run.
    data_dir = _data_dir(env)
    if not os.path.isdir(data_dir):
        return
    for fname in os.listdir(data_dir):
        base, ext = os.path.splitext(fname)
        if ext.lower() != ".gz" or os.path.splitext(base)[1].lower() not in COMPRESSIBLE:
            continue
        os.remove(os.path.join(data_dir, fname))
        print(f"[pre-uploadfs] cleanup {fname}")

env.AddPreAction("uploadfs", gzip_assets)
env.AddPostAction("uploadfs", cleanup_gzip_assets)
