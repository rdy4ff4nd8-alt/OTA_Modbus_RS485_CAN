"""Publish the Keil App build and keep its OTA download server running."""
import pathlib
import re
import urllib.request
from urllib.parse import urlsplit

from release_server import publish, serve


ROOT = pathlib.Path(__file__).resolve().parent.parent
APP = ROOT / "stm32/app"
PROJECT = ROOT / "stm32/app/MDK-ARM/GatewayOTA.uvprojx"
IMAGE = ROOT / "stm32/app/MDK-ARM/GatewayOTA/GatewayOTA.bin"
CONFIG = ROOT / "esp01s/GatewayOTA/config.h"
RELEASES = ROOT / "releases"


def prepare():
    version_match = re.search(r"GW_APP_VERSION=(\d+)", PROJECT.read_text(encoding="utf-8"))
    url_match = re.search(r'^\s*#define\s+OTA_MANIFEST_URL\s+"([^"]+)"',
                          CONFIG.read_text(encoding="utf-8"), re.MULTILINE)
    if not version_match or not url_match:
        raise ValueError("Keil version or ESP manifest URL is missing")
    sources = [PROJECT, *(p for folder in (APP / "Core", APP / "User", APP / "FreeRTOS", APP / "Drivers")
                          for p in folder.rglob("*") if p.suffix.lower() in (".c", ".h", ".s"))]
    if not IMAGE.is_file() or IMAGE.stat().st_mtime_ns < max(p.stat().st_mtime_ns for p in sources):
        raise ValueError("Build the new App in Keil before publishing")
    url = urlsplit(url_match.group(1))
    if url.scheme != "http" or url.path != "/manifest.txt" or not url.hostname:
        raise ValueError("This launcher needs an http://host:port/manifest.txt URL")
    origin = f"{url.scheme}://{url.netloc}"
    manifest = publish(IMAGE, RELEASES, int(version_match.group(1)), origin)
    return manifest, url.hostname, url.port or 80


def main():
    manifest, host, port = prepare()
    print(f"Published V{manifest.read_text(encoding='ascii').split('VERSION=')[1].splitlines()[0]}")
    print(f"Check in a browser: http://{host}:{port}/manifest.txt")
    try:
        serve(RELEASES, "0.0.0.0", port, None, None)
    except OSError as exc:
        try:
            with urllib.request.urlopen(f"http://{host}:{port}/manifest.txt", timeout=3) as response:
                if response.read() == manifest.read_bytes():
                    print("Existing OTA server is already serving this release.")
                    return
        except OSError:
            pass
        raise RuntimeError("Port is occupied by another service; close it and retry") from exc


if __name__ == "__main__":
    main()
