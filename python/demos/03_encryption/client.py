#!/usr/bin/env python3
"""Demo 03 — 传输密码层（ChaCha20-Poly1305-IETF AEAD）剖析与在线验证。

演示内容
--------
Part A（离线，确定性）——不连服务器，直接调用 lib.ork_cipher 展示：
  1. 加密前后的帧结构：15B OrkFrame 帧头 + AEAD 密文(明文长度) + 16B Poly1305 tag
  2. 15B 帧头逐字段拆解：ver(1) + seq(8 LE) + payload_len(2 LE) + packet_type(4 LE)
  3. nonce / AAD 的构造（域分隔，绑定 direction·channel·seq·conn·token·type·len）
  4. seal → open 往返一致；重放（同一帧二次 open）被拒绝

Part B（在线）——连接真实服务端：
  5. 报告本次会话协商出的加密模式（Bootstrap flags → encryption_required）
  6. 双通道发送 N 个 25B echo 帧；每个回显都经 open_frame AEAD 解密+完整性校验，
     统计"解密并回显成功"的次数，从而验证端到端加密通信可用。

线上帧布局（加密模式）：
    ver(1)=0x01 | seq(8,LE) | payload_len(2,LE,明文长) | packet_type(4,LE)
    | AEAD(plaintext) = ciphertext(payload_len) || tag(16)
  → 帧总长 = 15 + payload_len + 16

用法
----
    python demos/03_encryption/client.py
    python demos/03_encryption/client.py --offline-only      # 只跑 Part A，不连服务器
    python demos/03_encryption/client.py --rounds 5

退出码：0 = 通过；1 = 失败。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from lib import ork_cipher as oc  # noqa: E402
from lib.demo_helpers import (  # noqa: E402
    RELIABLE_MARKER,
    UNRELIABLE_MARKER,
    add_common_args,
    banner,
    connect_verbose,
    echo_once_reliable,
    echo_once_unreliable,
    fail,
    info,
    marker_int,
    ok,
    rule,
)
from lib.ork_client_session import CLIENT_COMMAND, OrkClientSession  # noqa: E402


def _hex_wrapped(data: bytes, width: int = 32) -> str:
    h = data.hex()
    lines = [h[i:i + width] for i in range(0, len(h), width)]
    return "\n".join("      " + ln for ln in lines)


def part_a_offline() -> bool:
    rule("Part A · 离线：帧结构 / nonce / AAD / seal-open / 重放")

    c2s_key = bytes(range(0x11, 0x11 + 32))
    s2c_key = bytes(range(0x22, 0x22 + 32))
    conn_id = 0x12345678
    token = 0xAABBCCDD
    plaintext = b"ORKBENCH1" + (12345).to_bytes(8, "little") + (0x0F0F).to_bytes(8, "little")

    state = oc.CipherState.from_keys(
        conn_id, token, c2s_key, s2c_key, expire_at=0, encryption_required=True
    )

    info(f"明文 payload ({len(plaintext)}B):")
    print(_hex_wrapped(plaintext))

    frame = oc.seal(state, oc.CHANNEL_RUDP, oc.C2S, CLIENT_COMMAND, plaintext)
    assert frame is not None, "seal 返回 None"

    # 15B 帧头拆解
    ver = frame[0]
    seq = int.from_bytes(frame[1:9], "little")
    payload_len = int.from_bytes(frame[9:11], "little")
    packet_type = int.from_bytes(frame[11:15], "little")
    body_len = len(frame) - oc.FRAME_HEADER_SIZE

    print()
    info(f"加密后 OrkFrame（总长 {len(frame)}B = 15B 帧头 + {payload_len}B 密文 + 16B tag）:")
    print(_hex_wrapped(frame))
    print()
    print("      15B 帧头逐字段:")
    print(f"        ver          = 0x{ver:02x}            (offset 0, 1B)")
    print(f"        seq          = {seq}            (offset 1, 8B LE)")
    print(f"        payload_len  = {payload_len}           (offset 9, 2B LE, 明文长)")
    print(f"        packet_type  = {packet_type}            (offset 11, 4B LE, CLIENT_COMMAND)")
    print(f"        AEAD body    = {body_len}B         (= 密文 {payload_len}B + tag 16B)")

    # nonce / AAD 展示
    nonce = oc.make_nonce(oc.CIPHER_VERSION, oc.C2S, oc.CHANNEL_RUDP, seq)
    aad = oc.make_aad(conn_id, token, packet_type, oc.CHANNEL_RUDP, seq, payload_len)
    print()
    info(f"nonce (12B) = ver|dir|channel|0x00 + seq(8B BE):")
    print(_hex_wrapped(nonce))
    info(f"AAD ({len(aad)}B) = 'ORK1' + ver|channel + connId + token + type + seq + len:")
    print(_hex_wrapped(aad))

    # seal → open 往返
    opened = oc.open_frame(state, oc.CHANNEL_RUDP, oc.C2S, 0, frame)
    if opened != plaintext:
        fail("open_frame 往返不一致")
        return False
    ok("seal → open 往返一致（AEAD 解密 + 完整性校验通过）")

    # 重放：同一帧二次 open（RUDP seq 未严格递增）→ 拒绝
    replay = oc.open_frame(state, oc.CHANNEL_RUDP, oc.C2S, 0, frame)
    if replay is not None:
        fail("重放未被拒绝")
        return False
    ok("重放检测：同一帧二次 open 被拒绝（RUDP seq 未递增）")

    # 篡改密文一字节 → 完整性校验失败
    tampered = bytearray(frame)
    tampered[-1] ^= 0x01
    st2 = oc.CipherState.from_keys(conn_id, token, c2s_key, s2c_key, encryption_required=True)
    if oc.open_frame(st2, oc.CHANNEL_RUDP, oc.C2S, 0, bytes(tampered)) is not None:
        fail("篡改未被检出")
        return False
    ok("完整性：密文尾字节被篡改 → open 返回 None（Poly1305 tag 校验失败）")
    return True


def part_b_online(host: str, port: int, rounds: int) -> bool:
    rule("Part B · 在线：真实会话的加密 echo")
    session: OrkClientSession | None = None
    try:
        session = connect_verbose(host, port, "demo-03-enc")
        mode = "AEAD ChaCha20-Poly1305-IETF (加密)" if session.encryption_required else "plaintext (15B 帧头保留)"
        info(f"本会话协商加密模式: {mode}")
        if not session.encryption_required:
            info("提示：当前服务端未强制加密，线上为明文帧（仍带 15B 帧头 + replay 防护）")

        rel_ok = udp_ok = 0
        rel_seq = marker_int(RELIABLE_MARKER)
        udp_seq = marker_int(UNRELIABLE_MARKER)
        for i in range(1, rounds + 1):
            r = echo_once_reliable(session, rel_seq + i)
            u = echo_once_unreliable(session, udp_seq + i, attempts=3)
            rel_ok += 1 if r is not None else 0
            udp_ok += 1 if u is not None else 0
            info(
                f"round {i}/{rounds}: Reliable {'✓' if r is not None else '✗'}"
                f"{' RTT=%.2fms' % r if r is not None else ''} | "
                f"Unreliable {'✓' if u is not None else '✗'}"
                f"{' RTT=%.2fms' % u if u is not None else ''}"
            )
        print()
        ok(f"加密 echo 解密成功: Reliable {rel_ok}/{rounds} · Unreliable {udp_ok}/{rounds}")
        return rel_ok == rounds and udp_ok >= 1
    except Exception as exc:  # noqa: BLE001
        fail(f"在线部分失败: {exc}")
        return False
    finally:
        if session is not None:
            session.close()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Demo 03 — 传输密码层剖析（帧结构/nonce/AAD/seal-open/重放）+ 在线加密 echo",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    add_common_args(parser)
    parser.add_argument("--rounds", type=int, default=3, help="Part B 在线 echo 轮次")
    parser.add_argument("--offline-only", action="store_true", help="只运行 Part A（离线，不连服务器）")
    args = parser.parse_args()

    banner("DEMO 03 · Encryption Layer", "ChaCha20-Poly1305-IETF AEAD · 帧结构剖析 + 在线加密 echo")

    passed = part_a_offline()
    if args.offline_only:
        print("\n\033[32m✅ Demo 03 (offline) 完成\033[0m" if passed else "\n\033[31m❌ Part A 失败\033[0m")
        return 0 if passed else 1

    passed = part_b_online(args.host, args.port, args.rounds) and passed
    if passed:
        print("\n\033[32m✅ Demo 03 完成\033[0m")
        return 0
    print("\n\033[31m❌ Demo 03 存在失败\033[0m")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
