#!/usr/bin/env python3
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
    return f"peer-{random.randint(10000, 99999)}"

# 连接管理
peers = {}  # {peer_id: websocket}
rooms = {}  # {room: [peer_ids]}

async def broadcast_to_room(room, message, exclude=None):
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
    if to_peer_id in peers:
        try:
            await peers[to_peer_id].send(message)
        except Exception as e:
            print(f"发送失败: {e}")

async def handle_websocket(websocket):
    peer_id = generate_id()
    print(f"[新连接] {peer_id}")
    peers[peer_id] = websocket
    current_room = None

    try:
        async for message in websocket:
            try:
                msg = json.loads(message)
                msg_type = msg.get("type")

                if msg_type == "join":
                    current_room = msg.get("room", "default")
                    if current_room not in rooms:
                        rooms[current_room] = []
                    rooms[current_room].append(peer_id)

                    # 通知房间内其他人（新 peer 加入）
                    await broadcast_to_room(
                        current_room,
                        json.dumps({"type": "peer_joined", "peerId": peer_id}),
                        exclude=peer_id
                    )

                    # 告诉新 peer 它的 ID
                    await websocket.send(json.dumps({"type": "joined", "peerId": peer_id}))
                    print(f"[加入房间] {peer_id} → {current_room}")

                elif msg_type in ["offer", "answer", "candidate"]:
                    msg["from"] = peer_id
                    to_peer = msg.get("to")
                    if to_peer:
                        print(f"[转发] {peer_id} → {to_peer}: {msg_type}")
                        await send_to_peer(to_peer, json.dumps(msg))

                elif msg_type == "leave":
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
    """启动简单的 HTTP 服务器，提供静态文件"""
    os.chdir(WEB_DIR)
    Handler = SimpleHTTPRequestHandler
    httpd = HTTPServer(("0.0.0.0", 8764), Handler)
    print(f"HTTP 服务器运行中: http://localhost:8764/")
    httpd.serve_forever()

async def start_websocket_server():
    """启动 WebSocket 信令服务器"""
    async with serve(handle_websocket, "0.0.0.0", 8765):
        print(f"WebSocket 信令服务器运行中: ws://localhost:8765/")
        await asyncio.Future()

def main():
    print("=" * 50)
    print("  CrystalRTC 信令服务器")
    print("=" * 50)

    # 启动 HTTP 服务器（后台线程）
    http_thread = threading.Thread(target=start_http_server, daemon=True)
    http_thread.start()

    # 启动 WebSocket 服务器
    asyncio.run(start_websocket_server())

if __name__ == "__main__":
    main()
