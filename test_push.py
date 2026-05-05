#!/usr/bin/env python3
"""测试 PUSH 请求，验证 xrtc-server 返回的 SDP offer"""

import struct, json, socket, sys

HOST = '127.0.0.1'
PORT = 9000

def send_push(stream_name="test", audio=1, video=1):
    body = json.dumps({
        "cmdno": 1,
        "uid": 12345,
        "stream_name": stream_name,
        "audio": audio,
        "video": video,
    })
    hdr = struct.pack('<HHI16sIII', 0, 0, 1001,
                      b'\x00' * 16, 0xfb202202, 0, len(body))

    s = socket.socket()
    s.settimeout(10)
    try:
        s.connect((HOST, PORT))
        s.sendall(hdr + body.encode())

        data = s.recv(36)
        body_len = struct.unpack_from('<I', data, 32)[0]
        resp = s.recv(body_len)
        return resp.decode()
    finally:
        s.close()

if __name__ == '__main__':
    resp = send_push(*sys.argv[1:])
    print(resp)
    # 简单校验
    if '"errno":0' in resp or '"err_no":0' in resp:
        if 'a=ice-ufrag:' in resp:
            print("\n✅ ICE ufrag/pwd 存在")
        else:
            print("\n⚠️  没有 ICE 信息（Step 2 未完成）")
    else:
        print("\n❌ 请求失败")
