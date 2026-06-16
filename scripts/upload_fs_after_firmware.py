Import("env")

def upload_fs(source, target, env):
    # Only upload filesystem after USB flash, not OTA (filesystem is already on device)
    if env.get("UPLOAD_PROTOCOL", "esptool") != "esptool":
        return
    env.Execute("pio run -t uploadfs -e " + env["PIOENV"])

env.AddPostAction("upload", upload_fs)
