Import("env")
import gzip as gz, shutil, os

# The filesystem image (buildfs, uploadfs, uploadfsota) is built from a copy
# of data/ in the build directory, with every compressible top-level file
# replaced by its .gz: the web server serves the .gz, and the uncompressed
# web UI alone does not fit the littlefs partition. The release workflow
# packs the same layout. data/ itself stays as it is.

COMPRESSIBLE = {".html", ".css", ".js", ".json", ".svg", ".txt"}
FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}


def stage_fs_data(env):
    src_dir = env.subst("$PROJECT_DATA_DIR")
    stage_dir = os.path.join(env.subst("$BUILD_DIR"), "fsdata")
    shutil.rmtree(stage_dir, ignore_errors=True)
    shutil.copytree(src_dir, stage_dir)
    for fname in sorted(os.listdir(stage_dir)):
        path = os.path.join(stage_dir, fname)
        if not os.path.isfile(path) or os.path.splitext(fname)[1].lower() not in COMPRESSIBLE:
            continue
        with open(path, "rb") as f_in, gz.open(path + ".gz", "wb", compresslevel=9) as f_out:
            shutil.copyfileobj(f_in, f_out)
        orig, comp = os.path.getsize(path), os.path.getsize(path + ".gz")
        os.remove(path)
        print(f"[fs] gzip {fname}: {orig} B → {comp} B ({comp * 100 // orig}%)")
    env.Replace(PROJECT_DATA_DIR=stage_dir)


if FS_TARGETS & set(COMMAND_LINE_TARGETS) and os.path.isdir(env.subst("$PROJECT_DATA_DIR")):
    stage_fs_data(env)
