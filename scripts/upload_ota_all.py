Import("env")
import gzip as gz, shutil, subprocess, sys, os, socket, time
from concurrent.futures import ThreadPoolExecutor, as_completed

HOSTS = [
    "batterylight1.local",
    "batterylight2.local",
    "batterylight3.local",
]

# Hosts that run on ESP32-C3 (RISC-V) — need a separate firmware build
C3_HOSTS = {"batterylight1.local"}
C3_ENV   = "esp32c3"

REBOOT_WAIT   = 10
RETRY_ATTEMPTS = 1   # total attempts per device per phase
RETRY_DELAY    = 6   # seconds between attempts
COMPRESSIBLE  = {".html", ".css", ".js", ".json", ".svg", ".txt"}

# ── filesystem builder ─────────────────────────────────────────────────────────

def _gzip_assets(env):
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
        print(f"[ota-all] gzip {fname}: {orig} B → {comp} B ({comp * 100 // orig}%)")

def _spiffs_size(env):
    raw = env.subst("$SPIFFS_SIZE").strip()
    if raw and raw != "$SPIFFS_SIZE":
        try:
            return int(raw, 0)
        except ValueError:
            pass
    proj  = env.subst("$PROJECT_DIR")
    fwdir = env.PioPlatform().get_package_dir("framework-arduinoespressif32") or ""
    for csv_path in [
        os.path.join(proj, "partitions.csv"),
        os.path.join(proj, env.subst("$PARTITIONS_TABLE_CSV") or ""),
        os.path.join(fwdir, "tools", "partitions", "default.csv"),
    ]:
        if not os.path.isfile(csv_path):
            continue
        with open(csv_path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = [p.strip() for p in line.split(",")]
                if len(parts) >= 5 and parts[2].lower() in ("spiffs", "littlefs", "fat"):
                    try:
                        return int(parts[4], 0)
                    except (ValueError, IndexError):
                        pass
    print("[ota-all] WARNING: partition size unknown, falling back to 0x170000 (1.5 MB)")
    return 0x170000

def _build_fs(env):
    _gzip_assets(env)
    pkg = env.PioPlatform().get_package_dir("tool-mklittlefs")
    if not pkg:
        print("[ota-all] ERROR: tool-mklittlefs package not found")
        return 1
    tool = os.path.join(pkg, "mklittlefs" + (".exe" if sys.platform.startswith("win") else ""))
    if not os.path.isfile(tool):
        print(f"[ota-all] ERROR: mklittlefs not found at {tool}")
        return 1
    data_dir = os.path.join(env.subst("$PROJECT_DIR"), "data")
    out_file  = os.path.join(env.subst("$BUILD_DIR"), "littlefs.bin")
    page = int(env.subst("$SPIFFS_PAGE").strip() or 256)
    block = int(env.subst("$SPIFFS_BLOCK").strip() or 4096)
    size  = _spiffs_size(env)
    r = subprocess.run(
        [tool, "-c", data_dir, "-p", str(page), "-b", str(block), "-s", str(size),
         "--name_max", "64", out_file],
        capture_output=True,
    )
    if r.returncode != 0:
        print(r.stderr.decode(errors="replace"))
    return r.returncode

# ── OTA helpers ────────────────────────────────────────────────────────────────

def _find_espota(env):
    for p in [
        os.path.join(
            env.PioPlatform().get_package_dir("framework-arduinoespressif32") or "",
            "tools", "espota.py",
        ),
        os.path.join(
            env.PioPlatform().get_package_dir("tool-esptoolpy") or "",
            "espota.py",
        ),
    ]:
        if os.path.isfile(p):
            return p
    return None

def _resolve(host):
    try:
        return socket.getaddrinfo(host, None, socket.AF_INET)[0][4][0]
    except socket.gaierror:
        return None

def _filter_output(text):
    """Return lines worth showing: skip blank lines and raw progress percentages."""
    out = []
    for line in text.strip().splitlines():
        s = line.strip()
        if not s:
            continue
        # espota emits "XX%" progress lines — drop them
        if s.rstrip("%").isdigit():
            continue
        out.append(s)
    return out

def _upload_one(espota, host, image_path, port, extra_flags):
    """Upload to one host with retries. Returns (host, ip_or_None, ok, elapsed_s, output_lines)."""
    ip      = None
    elapsed = 0.0
    lines   = []

    for attempt in range(1, RETRY_ATTEMPTS + 1):
        # Re-resolve each attempt — IP may change after a reboot
        ip = _resolve(host)
        if not ip:
            msg = "mDNS lookup failed — device offline or not on this network"
            if attempt < RETRY_ATTEMPTS:
                lines.append(f"attempt {attempt}: {msg} — retrying in {RETRY_DELAY}s ...")
                time.sleep(RETRY_DELAY)
                continue
            lines.append(msg)
            return host, None, False, elapsed, lines

        t0 = time.monotonic()
        r  = subprocess.run(
            [sys.executable, espota, "-i", ip, "-p", str(port), "-f", image_path] + extra_flags,
            capture_output=True, text=True,
        )
        elapsed += time.monotonic() - t0
        attempt_lines = _filter_output(r.stderr + "\n" + r.stdout)

        if r.returncode == 0:
            if attempt > 1:
                lines.append(f"succeeded on attempt {attempt}")
            return host, ip, True, elapsed, lines

        prefix = f"attempt {attempt}: " if RETRY_ATTEMPTS > 1 else ""
        lines.extend(f"{prefix}{l}" for l in attempt_lines)
        if attempt < RETRY_ATTEMPTS:
            lines.append(f"retrying in {RETRY_DELAY}s ...")
            time.sleep(RETRY_DELAY)

    return host, ip, False, elapsed, lines

def _upload_parallel(espota, image_path, extra_flags, hosts=None, host_to_image=None):
    """
    Upload to all hosts in parallel.
    host_to_image overrides image_path on a per-host basis when provided.
    Prints a result line per host and returns the list of failed hostnames.
    """
    if hosts is None:
        hosts = HOSTS

    results = {}
    with ThreadPoolExecutor(max_workers=len(hosts)) as pool:
        futures = {
            pool.submit(
                _upload_one, espota, h,
                host_to_image[h] if host_to_image and h in host_to_image else image_path,
                3232, extra_flags,
            ): h
            for h in hosts
        }
        for f in as_completed(futures):
            host, ip, ok, elapsed, lines = f.result()
            results[host] = (ip, ok, elapsed, lines)

    failed = []
    for host in hosts:          # print in original order
        ip, ok, elapsed, lines = results[host]
        ip_tag  = f"  ({ip}, {elapsed:.1f}s)" if ip else ""
        if ok:
            print(f"[ota-all]   ✓  {host}{ip_tag}")
        else:
            print(f"[ota-all]   ✗  {host}{ip_tag}")
            for line in lines:
                print(f"[ota-all]      {line}")
            failed.append(host)
    return failed

# ── targets ────────────────────────────────────────────────────────────────────

def do_upload_all(source, target, env):
    espota = _find_espota(env)
    if not espota:
        print("[ota-all] ERROR: espota.py not found")
        return 1

    dev_firmware = str(source[0])
    proj_dir     = env.subst("$PROJECT_DIR")

    # Build ESP32-C3 firmware for hosts in C3_HOSTS (different CPU arch — can't share binary)
    c3_firmware = os.path.join(proj_dir, ".pio", "build", C3_ENV, "firmware.bin")
    if C3_HOSTS:
        print(f"\n[ota-all] Building ESP32-C3 firmware (env:{C3_ENV}) ...")
        r = subprocess.run(
            ["pio", "run", "-e", C3_ENV],
            cwd=proj_dir, capture_output=True, text=True,
        )
        if r.returncode != 0:
            print(r.stdout[-3000:] + r.stderr[-3000:])
            print(f"[ota-all] ERROR: ESP32-C3 firmware build failed")
            return 1
        if not os.path.isfile(c3_firmware):
            print(f"[ota-all] ERROR: {c3_firmware} not found after build")
            return 1

    host_to_fw = {h: (c3_firmware if h in C3_HOSTS else dev_firmware) for h in HOSTS}

    print("\n[ota-all] Building filesystem image ...")
    if _build_fs(env) != 0:
        print("[ota-all] ERROR: buildfs failed")
        return 1
    fs_image = os.path.join(env.subst("$BUILD_DIR"), "littlefs.bin")
    if not os.path.isfile(fs_image):
        print(f"[ota-all] ERROR: {fs_image} not found after build")
        return 1

    # ── Phase 1: firmware ──────────────────────────────────────────────────────
    print(f"\n[ota-all] ═══ Phase 1/2: firmware ({len(HOSTS)} devices in parallel, up to {RETRY_ATTEMPTS} attempts) ═══")
    failed_fw = _upload_parallel(espota, dev_firmware, extra_flags=[], host_to_image=host_to_fw)

    successful = [h for h in HOSTS if h not in failed_fw]
    if not successful:
        print(f"\n[ota-all] ✗  All firmware uploads failed — aborting.")
        return 1
    if failed_fw:
        print(f"[ota-all]    Continuing with {len(successful)}/{len(HOSTS)} device(s).")

    # ── Wait for reboots ───────────────────────────────────────────────────────
    print(f"\n[ota-all] Waiting {REBOOT_WAIT}s for devices to reboot", end="", flush=True)
    for _ in range(REBOOT_WAIT):
        time.sleep(1)
        print(".", end="", flush=True)
    print()

    # ── Phase 2: filesystem ────────────────────────────────────────────────────
    print(f"\n[ota-all] ═══ Phase 2/2: filesystem ({len(successful)} devices in parallel, up to {RETRY_ATTEMPTS} attempts) ═══")
    failed_fs = _upload_parallel(espota, fs_image, extra_flags=["-s"], hosts=successful)

    all_failed = sorted(set(failed_fw + failed_fs))
    if all_failed:
        print(f"\n[ota-all] ✗  Failed: {', '.join(all_failed)}")
        return 1

    print(f"\n[ota-all] ✓  All {len(HOSTS)} devices updated (firmware + filesystem). Config preserved.")
    return 0


def do_upload_all_fs(source, target, env):
    espota = _find_espota(env)
    if not espota:
        print("[ota-all] ERROR: espota.py not found")
        return 1

    print("\n[ota-all] Building filesystem image ...")
    if _build_fs(env) != 0:
        print("[ota-all] ERROR: buildfs failed")
        return 1
    fs_image = os.path.join(env.subst("$BUILD_DIR"), "littlefs.bin")
    if not os.path.isfile(fs_image):
        print(f"[ota-all] ERROR: {fs_image} not found after build")
        return 1

    print(f"\n[ota-all] ═══ Filesystem update ({len(HOSTS)} devices in parallel, up to {RETRY_ATTEMPTS} attempts) ═══")
    failed = _upload_parallel(espota, fs_image, extra_flags=["-s"])

    if failed:
        print(f"\n[ota-all] ✗  Failed: {', '.join(failed)}")
        return 1

    print(f"\n[ota-all] ✓  All {len(HOSTS)} devices updated. Config preserved.")
    return 0


env.AddCustomTarget(
    name="upload_all",
    dependencies=["$BUILD_DIR/${PROGNAME}.bin"],
    actions=[do_upload_all],
    title="Upload firmware + filesystem to all OTA targets",
    description="Push firmware then filesystem to " + ", ".join(HOSTS) +
                " in parallel. config.json is preserved.",
)

env.AddCustomTarget(
    name="upload_all_fs",
    dependencies=[],
    actions=[do_upload_all_fs],
    title="Upload filesystem to all OTA targets",
    description="Push filesystem image to " + ", ".join(HOSTS) +
                " in parallel. config.json is preserved.",
)
