# SPDX-License-Identifier: BSD-3-Clause
"""Framed local radio control; SSIDs remain exact bytes, never shell text."""
import json
import socket
import struct

SOCKET = "/run/a20-wifi.sock"
SCAN, STATUS, CONNECT, DISCONNECT, SCAN_JSON = 1, 2, 3, 4, 5


def receive(stream, length):
    result = bytearray()
    while len(result) < length:
        block = stream.recv(length - len(result))
        if not block:
            raise OSError("无线服务提前关闭了连接")
        result.extend(block)
    return bytes(result)


class Radio:
    def __init__(self, path=SOCKET):
        self.path = path

    def command(self, operation, ssid=b"", password=b""):
        if not isinstance(ssid, bytes) or not 0 <= len(ssid) <= 32:
            raise ValueError("热点名称最长为 32 字节")
        if len(password) > 63:
            raise ValueError("无线密码最长为 63 字节")
        request = bytearray(struct.pack("<IIII32s64s", 1, operation,
                                       len(ssid), len(password), ssid, password))
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stream:
                stream.settimeout(100)
                try:
                    stream.connect(self.path)
                except OSError as error:
                    raise OSError(f"无线服务尚未就绪（{error.errno}），点击“诊断日志”查看驱动状态") from error
                stream.sendall(request)
                result, length = struct.unpack("<iI", receive(stream, 8))
                if length > 20000:
                    raise OSError("无线服务返回的数据过长")
                body = receive(stream, length)
        finally:
            request[:] = b"\0" * len(request)
        if result:
            raise OSError(f"无线操作失败（{result}），请查看 /var/log/a20/wifi.log")
        return body

    def scan(self):
        networks = json.loads(self.command(SCAN_JSON))
        result = {}
        for network in networks:
            ssid = bytes.fromhex(network["ssid_hex"])
            if not 0 < len(ssid) <= 32:
                continue
            key = (ssid, int(network["security"]))
            if key not in result or network["signal"] > result[key]["signal"]:
                result[key] = {**network, "ssid": ssid}
        return sorted(result.values(), key=lambda n: (-n["signal"], n["ssid"]))

    def status(self):
        return dict(item.split("=", 1) for item in
                    self.command(STATUS).decode("utf-8", "replace").split() if "=" in item)

    def connect(self, ssid, password=b""):
        return self.command(CONNECT, ssid, password)

    def disconnect(self):
        self.command(DISCONNECT)
