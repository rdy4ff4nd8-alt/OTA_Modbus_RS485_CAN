"""Two logical Modbus RTU slaves (IDs 1/2, FC03, register 0) on USB-RS485."""
import argparse
import time


def crc16(data):
    # Modbus RTU CRC16 从 0xFFFF 起算，每个字节处理 8 次移位。
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def response(request, values):
    # 读保持寄存器请求固定 8 字节；尾部两个 CRC 字节按小端解释。
    if len(request) != 8 or crc16(request[:6]) != int.from_bytes(request[6:8], "little"):
        return None
    # 只模拟地址 1/2、功能码 03、起始地址 0、数量 1 的请求。
    if request[0] not in values or request[1:6] != bytes((3, 0, 0, 0, 1)):
        return None
    value = values[request[0]]
    # 应答格式：地址、功能码、数据字节数 2、寄存器高/低字节、CRC。
    packet = bytes((request[0], 3, 2, (value >> 8) & 255, value & 255))
    return packet + crc16(packet).to_bytes(2, "little")


def main():
    import serial
    # 可选择寄存器值，也能故意丢帧或破坏 CRC 来测网关容错。
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="e.g. COM5")
    parser.add_argument("--value1", type=int, default=1234)
    parser.add_argument("--value2", type=int, default=2345)
    parser.add_argument("--drop-every", type=int, default=0,
                        help="skip every Nth valid request, for timeout testing")
    parser.add_argument("--bad-crc-every", type=int, default=0,
                        help="corrupt every Nth reply, for CRC testing")
    args = parser.parse_args()
    if not 0 <= args.value1 <= 65535 or not 0 <= args.value2 <= 65535:
        parser.error("values must be 16-bit registers")
    if args.drop_every < 0 or args.bad_crc_every < 0:
        parser.error("fault intervals must be nonnegative")
    values = {1: args.value1, 2: args.value2}
    # 串口参数须与 STM32 的 Modbus 主站一致：19200、8E1。
    with serial.Serial(args.port, 19200, bytesize=serial.EIGHTBITS,
                       parity=serial.PARITY_EVEN, stopbits=serial.STOPBITS_ONE,
                       timeout=0.1) as port:
        print("Modbus slave IDs 1/2, register 0 =", values, flush=True)
        buf = bytearray()
        valid_requests = 0
        while True:
            # 一次最多凑满 8 字节；空读表示帧间隔，丢弃半帧。
            chunk = port.read(8 - len(buf))
            if chunk:
                buf.extend(chunk)
                if len(buf) == 8:
                    reply = response(bytes(buf), values)
                    if reply:
                        valid_requests += 1
                        if args.drop_every and valid_requests % args.drop_every == 0:
                            # 不应答，用来验证主站超时和状态标记。
                            print("simulated timeout", flush=True)
                            buf.clear()
                            continue
                        if args.bad_crc_every and valid_requests % args.bad_crc_every == 0:
                            # 翻转 CRC 的一位，验证主站的 CRC 检查。
                            reply = reply[:-1] + bytes((reply[-1] ^ 1,))
                        time.sleep(0.005)  # 等 USB-RS485 自动收发方向切换完成。
                        port.write(reply)
                        port.flush()
                        print("reply to slave", reply[0], flush=True)
                    buf.clear()
            else:
                buf.clear()  # 读超时形成帧间隙，未收齐的请求作废。


if __name__ == "__main__":
    # 直接运行脚本才打开测试串口。
    main()
