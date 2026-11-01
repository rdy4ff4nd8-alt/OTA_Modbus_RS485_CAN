"""Publish an STM32 app binary and serve a fixed-length OTA manifest and image."""
import argparse
import hashlib
import http.server
import pathlib
import ssl
import struct

PRODUCT = "stm32f103c8-gateway"
# Bootloader 占前 24 KiB；应用只允许链接到 0x08006000..0x0800FFFF。
APP_BASE = 0x08006000
APP_END = 0x08010000
APP_LIMIT = APP_END - APP_BASE


def validate_image(data: bytes) -> None:
    # 发布前先检查二进制长度和向量表，避免把错链接地址的镜像发给设备。
    if not 8 <= len(data) <= APP_LIMIT:
        raise ValueError(f"image size must be 8..{APP_LIMIT} bytes")
    sp, pc = struct.unpack_from("<II", data)
    # 前两个小端 32 位值分别为初始栈指针和复位入口。
    if not (0x20000000 <= sp <= 0x20005000):
        raise ValueError("initial stack pointer is outside STM32F103C8 SRAM")
    if not (APP_BASE <= (pc & ~1) < APP_BASE + len(data) and pc & 1):
        raise ValueError("reset vector is not linked for 0x08006000 application slot")


def publish(binary: pathlib.Path, root: pathlib.Path, version: int, origin: str) -> pathlib.Path:
    # 先验证镜像、版本和下载源地址，再生成供 ESP 使用的清单。
    data = binary.read_bytes()
    validate_image(data)
    if not 1 <= version <= 99_999_999:
        raise ValueError("version must be a positive integer <= 99999999")
    origin = origin.rstrip("/")
    if not origin.startswith(("http://", "https://")):
        raise ValueError("origin must be an http:// or https:// URL")
    root.mkdir(parents=True, exist_ok=True)
    image = root / "firmware.bin"
    tmp = root / "firmware.bin.tmp"
    # 先写临时文件再替换，减少服务端读到半成品的机会。
    tmp.write_bytes(data)
    tmp.replace(image)
    sha = hashlib.sha256(data).hexdigest()
    # ESP 解析固定的 KEY=value 行；Bootloader 再核对长度和 SHA-256。
    manifest = (f"PRODUCT={PRODUCT}\nVERSION={version}\nLENGTH={len(data)}\n"
                f"SHA256={sha}\nURL={origin}/firmware.bin\n")
    tmp_manifest = root / "manifest.txt.tmp"
    tmp_manifest.write_text(manifest, encoding="ascii")
    tmp_manifest.replace(root / "manifest.txt")
    return root / "manifest.txt"


def serve(root: pathlib.Path, host: str, port: int,
          cert: pathlib.Path | None, key: pathlib.Path | None) -> None:
    # serve 只对已有发布内容开服务，不自动发布新的 ELF/BIN。
    if not (root / "manifest.txt").is_file() or not (root / "firmware.bin").is_file():
        raise ValueError("publish a firmware image before serving")

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            # 把静态文件目录固定在 --root。
            super().__init__(*args, directory=str(root), **kwargs)

        def end_headers(self):
            # 避免客户端缓存旧 manifest。
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

    httpd = http.server.ThreadingHTTPServer((host, port), Handler)
    if cert or key:
        # 提供证书和私钥时启用 HTTPS；两者必须同时给出。
        if not cert or not key:
            raise ValueError("both --cert and --key are required for HTTPS")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=cert, keyfile=key)
        httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
    print(f"Serving {root} on {'https' if cert else 'http'}://{host}:{port}")
    httpd.serve_forever()


def main() -> None:
    # publish 负责生成 firmware.bin 和 manifest.txt；serve 负责提供下载。
    p = argparse.ArgumentParser()
    commands = p.add_subparsers(dest="command", required=True)
    pub = commands.add_parser("publish")
    pub.add_argument("binary", type=pathlib.Path)
    pub.add_argument("--root", type=pathlib.Path, default=pathlib.Path("releases"))
    pub.add_argument("--version", type=int, required=True)
    pub.add_argument("--origin", required=True, help="PC address visible to ESP-01S")
    srv = commands.add_parser("serve")
    srv.add_argument("--root", type=pathlib.Path, default=pathlib.Path("releases"))
    srv.add_argument("--host", default="127.0.0.1")
    srv.add_argument("--port", type=int, default=8765)
    srv.add_argument("--cert", type=pathlib.Path)
    srv.add_argument("--key", type=pathlib.Path)
    args = p.parse_args()
    if args.command == "publish":
        print(publish(args.binary, args.root, args.version, args.origin))
    else:
        serve(args.root, args.host, args.port, args.cert, args.key)


if __name__ == "__main__":
    # 导入函数供测试使用时，不自动启动 HTTP 服务。
    main()
