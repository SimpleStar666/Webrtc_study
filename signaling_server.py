#!/usr/bin/env python3
# ============================================================================
# CrystalRTC 信令服务器 (Signaling Server)
# ============================================================================
#
# 【什么是信令服务器？】
# 在 WebRTC 中，两个浏览器（称为 Peer）需要交换一些控制信息才能建立
# 直接的 P2P 连接，这个过程叫做"信令"（Signaling）。WebRTC 标准本身
# 并没有规定信令如何传输，开发者可以自由选择——最常见的方案就是使用
# WebSocket 作为信令通道。
#
# 【信令服务器的作用】
# 1. 帮助双方交换 SDP（Session Description Protocol）——描述各自的媒体能力
#    - Offer SDP：发起方创建的会话描述，包含自己的编码格式、分辨率等
#    - Answer SDP：应答方创建的会话描述，回应并协商媒体参数
# 2. 帮助双方交换 ICE Candidate——网络连通性候选地址
#    - 每个 Candidate 是一个可能的网络路径（IP:端口/协议）
#    - 双方收集各自的 Candidate 后通过信令转发给对方
#    - 对方收到后尝试连通，最终选择最优路径建立 P2P 通道
# 3. 管理房间（Room），让同一房间的用户能发现彼此
#
# 【本服务器的信令协议（消息类型）】
# 客户端 → 服务器：
#   { type: "join",    room: "房间名" }          — 加入房间
#   { type: "offer",   sdp: {...}, to: "peerId" } — 发送 Offer
#   { type: "answer",  sdp: {...}, to: "peerId" } — 发送 Answer
#   { type: "candidate", candidate: {...}, to: "peerId" } — 发送 ICE Candidate
#   { type: "leave" }                             — 离开房间
#
# 服务器 → 客户端：
#   { type: "joined",      peerId: "..." } — 确认加入，告知分配的 ID
#   { type: "peer_joined", peerId: "..." } — 通知房间内其他人有新用户加入
#   { type: "peer_left",   peerId: "..." } — 通知房间内有人离开
#   { type: "offer/answer/candidate", from: "...", ... } — 转发信令消息
#
# 【架构说明】
# 本服务器同时运行两个服务：
# - HTTP 服务器（端口 8764）：提供静态文件，让浏览器加载 index.html
# - WebSocket 服务器（端口 8765）：处理信令消息的实时转发
# ============================================================================
"""CrystalRTC 简单信令服务器（HTTP + WebSocket）
提供：
1. http://localhost:8765/ → 加载 web/index.html
2. ws://localhost:8765/  → WebSocket 信令
"""
import asyncio
import websockets
from websockets.server import serve
from http.server import HTTPServer, SimpleHTTPRequestHandler
from http.server import BaseHTTPRequestHandler
import threading
import json
import random
import os
from pathlib import Path

SERVER_DIR = Path(__file__).parent
WEB_DIR = SERVER_DIR / "web"


def generate_id():
    """生成唯一的 Peer ID

    每个 WebSocket 连接都会被分配一个唯一的 peer_id，
    用于在信令消息中标识发送者和接收者。
    格式为 "peer-XXXXX"，其中 XXXXX 是 10000~99999 的随机数。
    """
    return f"peer-{random.randint(10000, 99999)}"


# ---- 连接管理数据结构 ----
# peers: 记录所有当前在线的 WebSocket 连接
#   key = peer_id（字符串），value = websocket 对象
#   当用户连接时添加，断开时删除
peers = {}  # {peer_id: websocket}

# rooms: 记录每个房间中的用户列表
#   key = room（房间名字符串），value = 该房间内所有 peer_id 的列表
#   同一房间内的用户可以互相发现并建立 WebRTC 连接
rooms = {}  # {room: [peer_ids]}


async def broadcast_to_room(room, message, exclude=None):
    """向房间内所有用户广播消息（可排除指定用户）

    在信令流程中，当新用户加入房间时，需要通知房间内已有的用户，
    让他们知道有新的 Peer 可以建立连接。

    参数:
        room: 房间名称
        message: 要广播的消息（JSON 字符串）
        exclude: 要排除的 peer_id（通常是消息的触发者本人，避免收到自己的通知）
    """
    if room not in rooms:
        return
    for peer_id in rooms[room]:
        if peer_id == exclude:
            continue
        if peer_id in peers:
            try:
                await peers[peer_id].send(message)
            except Exception as e:
                print(f"发送失败: {e}")


async def send_to_peer(to_peer_id, message):
    """向指定用户发送点对点信令消息

    这是 SDP/ICE 消息转发的核心函数。信令服务器不解析 SDP 或 ICE
    Candidate 的内容，只是根据 "to" 字段将消息路由到目标用户。
    这种"透明转发"是信令服务器的典型设计——它不需要理解媒体协商细节。

    参数:
        to_peer_id: 目标用户的 peer_id
        message: 要发送的消息（JSON 字符串）
    """
    if to_peer_id in peers:
        try:
            await peers[to_peer_id].send(message)
        except Exception as e:
            print(f"发送失败: {e}")


async def handle_websocket(websocket):
    """处理单个 WebSocket 连接的生命周期

    这是信令服务器的核心处理函数，每个 WebSocket 连接都会创建一个
    对应的协程实例。函数负责：
    1. 为新连接分配 peer_id
    2. 循环接收并处理客户端发来的信令消息
    3. 连接断开时清理资源并通知房间内其他用户

    【信令流程详解】
    典型的双人通话信令流程如下：

    用户A（发起方）                     信令服务器                     用户B（应答方）
        |                                  |                              |
        |-- join(room) ------------------>|                              |
        |<-- joined(peerId_A) ------------|                              |
        |                                  |<-- join(room) --------------|
        |                                  |--- joined(peerId_B) ------->|
        |<-- peer_joined(peerId_B) -------|                              |
        |                                  |                              |
        |  (A 发现 B 加入，创建 Offer)     |                              |
        |-- offer(sdp, to=B) ------------>|--- offer(sdp, from=A) ----->|
        |                                  |                              |
        |                                  |  (B 收到 Offer，创建 Answer) |
        |<--- answer(sdp, from=B) --------|<-- answer(sdp, to=A) -------|
        |                                  |                              |
        |  (双方同时交换 ICE Candidates)    |                              |
        |<--- candidate(from=B) ----------|<-- candidate(to=A) ---------|
        |--- candidate(to=B) ------------>|--- candidate(from=A) ------>|
        |                                  |                              |
        |========= P2P 直连建立 ===========|                              |
    """
    # 为新连接分配唯一 ID
    peer_id = generate_id()
    print(f"[新连接] {peer_id}")

    # 将新连接注册到 peers 字典中
    peers[peer_id] = websocket

    # 记录当前用户所在的房间，断开连接时需要从此房间移除
    current_room = None

    try:
        # 持续监听客户端发来的消息
        async for message in websocket:
            try:
                msg = json.loads(message)
                msg_type = msg.get("type")

                if msg_type == "join":
                    # ---- 加入房间 ----
                    # 用户发送 join 消息加入指定房间，服务器需要：
                    # 1. 将用户添加到房间的成员列表
                    # 2. 通知房间内已有的其他用户（peer_joined）
                    # 3. 告知新用户其分配到的 peer_id（joined）
                    current_room = msg.get("room", "default")
                    if current_room not in rooms:
                        rooms[current_room] = []
                    rooms[current_room].append(peer_id)

                    # 通知房间内其他人：有新用户加入
                    # 收到 peer_joined 的用户会作为"发起方"创建 Offer
                    await broadcast_to_room(
                        current_room,
                        json.dumps({"type": "peer_joined", "peerId": peer_id}),
                        exclude=peer_id
                    )

                    # 告诉新用户它被分配的 peer_id
                    await websocket.send(json.dumps({"type": "joined", "peerId": peer_id}))
                    print(f"[加入房间] {peer_id} → {current_room}")

                elif msg_type in ["offer", "answer", "candidate"]:
                    # ---- 转发 SDP/ICE 消息 ----
                    # 这三种消息是 WebRTC 建立连接的关键：
                    #
                    # offer:     发起方的会话描述，包含媒体格式、带宽等信息
                    # answer:    应答方的会话描述，回应并协商媒体参数
                    # candidate: ICE 候选地址，包含 IP、端口、协议等信息
                    #
                    # 信令服务器不解析这些消息的内容，只负责根据 "to" 字段
                    # 将消息路由到目标用户，并附加 "from" 字段标识来源。
                    msg["from"] = peer_id
                    to_peer = msg.get("to")
                    if to_peer:
                        print(f"[转发] {peer_id} → {to_peer}: {msg_type}")
                        await send_to_peer(to_peer, json.dumps(msg))

                elif msg_type == "leave":
                    # ---- 主动离开房间 ----
                    # 用户主动离开时，从房间列表中移除，并通知房间内其他人
                    if current_room:
                        if current_room in rooms and peer_id in rooms[current_room]:
                            rooms[current_room].remove(peer_id)
                        await broadcast_to_room(
                            current_room,
                            json.dumps({"type": "peer_left", "peerId": peer_id})
                        )

            except json.JSONDecodeError:
                pass

    finally:
        # ---- 连接断开时的清理工作 ----
        # 无论连接是正常关闭还是异常断开，都需要：
        # 1. 从 peers 字典中移除
        # 2. 从所在房间的成员列表中移除
        # 3. 通知房间内其他用户该用户已离开
        print(f"[断开连接] {peer_id}")
        if peer_id in peers:
            del peers[peer_id]
        if current_room:
            if current_room in rooms and peer_id in rooms[current_room]:
                rooms[current_room].remove(peer_id)
            await broadcast_to_room(
                current_room,
                json.dumps({"type": "peer_left", "peerId": peer_id})
            )


def start_http_server():
    """启动简单的 HTTP 服务器，提供静态文件

    HTTP 服务器用于向浏览器提供 index.html 等静态资源，
    让用户可以通过浏览器访问 WebRTC 客户端页面。
    运行在 8764 端口，与 WebSocket 服务器（8765 端口）分离。
    """
    os.chdir(WEB_DIR)
    Handler = SimpleHTTPRequestHandler
    httpd = HTTPServer(("0.0.0.0", 8764), Handler)
    print(f"HTTP 服务器运行中: http://localhost:8764/")
    httpd.serve_forever()


async def start_websocket_server():
    """启动 WebSocket 信令服务器

    WebSocket 服务器监听 8765 端口，处理客户端的信令消息。
    每个新的 WebSocket 连接都会调用 handle_websocket 协程。
    asyncio.Future() 使服务器永远运行，不会自动退出。
    """
    async with serve(handle_websocket, "0.0.0.0", 8765):
        print(f"WebSocket 信令服务器运行中: ws://localhost:8765/")
        await asyncio.Future()


def main():
    """主函数：同时启动 HTTP 和 WebSocket 两个服务器

    由于 HTTP 服务器是同步的（基于 threading），而 WebSocket 服务器
    是异步的（基于 asyncio），所以将 HTTP 服务器放在后台线程中运行，
    WebSocket 服务器在主线程的事件循环中运行。
    """
    print("=" * 50)
    print("  CrystalRTC 信令服务器")
    print("=" * 50)

    # 启动 HTTP 服务器（后台线程）
    # daemon=True 表示主线程退出时该线程也会自动终止
    http_thread = threading.Thread(target=start_http_server, daemon=True)
    http_thread.start()

    # 启动 WebSocket 服务器（主线程）
    # asyncio.run() 创建事件循环并运行协程
    asyncio.run(start_websocket_server())


if __name__ == "__main__":
    main()
