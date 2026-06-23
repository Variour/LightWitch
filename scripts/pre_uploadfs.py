Import("env")
import gzip as gz, shutil, os

COMPRESSIBLE = {".html", ".css", ".js", ".json", ".svg", ".txt"}

def gzip_assets(source, target, env):
    data_dir = os.path.join(env.subst("$PROJECT_DIR"), "data")
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

env.AddPreAction("uploadfs", gzip_assets)
