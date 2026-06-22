import socket
import struct
import sys
import threading
import time

# protobuf 임포트 예외 처리
try:
    import mundus_vivens_pb2 as pb
except ImportError:
    print("⚠️  mundus_vivens_pb2.py를 찾을 수 없습니다. 프로토콜 컴파일이 선행되어야 합니다.")
    print("명령어: protoc -I=Protos --python_out=. Protos/mundus_vivens.proto")
    sys.exit(1)

def send_packet(sock, packet_id, protobuf_msg):
    payload = protobuf_msg.SerializeToString()
    length = 4 + len(payload)
    # Big-Endian unsigned short (H) 2개로 length(2B)와 packet_id(2B) 패킹
    header = struct.pack('>HH', length, packet_id)
    sock.sendall(header + payload)
    print(f"📤 [보냄] Packet ID: 0x{packet_id:04X}, Length: {length}")

def receive_loop(sock):
    try:
        while True:
            # 헤더 4바이트 수신
            header = sock.recv(4)
            if not header or len(header) < 4:
                print("❌ 서버 연결이 끊겼습니다 (헤더 수신 실패).")
                break
                
            length, packet_id = struct.unpack('>HH', header)
            payload_len = length - 4
            
            payload = b''
            if payload_len > 0:
                # 페이로드 루프 수신
                while len(payload) < payload_len:
                    chunk = sock.recv(payload_len - len(payload))
                    if not chunk:
                        break
                    payload += chunk
            
            handle_packet(packet_id, payload)
    except Exception as e:
        print(f"❌ 수신 루프 오류: {e}")

def handle_packet(packet_id, payload):
    if packet_id == 0x1001:  # SC_LOGIN_ACK
        resp = pb.LoginResponse()
        resp.ParseFromString(payload)
        print(f"\n📥 [수신 SC_LOGIN_ACK] 결과: {resp.success}, 메시지: {resp.message}")
        print(f"🗺️  구역 목록: {list(resp.locations)}")
        print(f"🤖 NPC 목록 ({len(resp.npcs)}명):")
        for npc in resp.npcs:
            print(f"  - {npc.display_name} ({npc.npc_id}): 위치=[{npc.location}], 감정=[{npc.emotion}], 활동=[{npc.activity}]")
        print("\n📝 입력: ", end="", flush=True)
            
    elif packet_id == 0x1002:  # SC_WORLD_SNAPSHOT
        snapshot = pb.WorldSnapshotPayload()
        snapshot.ParseFromString(payload)
        print(f"\n📥 [수신 SC_WORLD_SNAPSHOT] 논리 틱: {snapshot.tick}")
        for npc in snapshot.npcs:
            print(f"  - {npc.display_name} ({npc.npc_id}): 위치=[{npc.location}], 감정=[{npc.emotion}], 활동=[{npc.activity}]")
        print("\n📝 입력: ", end="", flush=True)
            
    elif packet_id == 0x1005:  # SC_NPC_REPLY
        reply = pb.NpcReplyPayload()
        reply.ParseFromString(payload)
        print(f"\n📥 [수신 SC_NPC_REPLY] 대화 ID: {reply.session_id}, {reply.npc_name} 대답: \"{reply.reply_text}\"")
        print("\n📝 입력: ", end="", flush=True)
        
    elif packet_id == 0x10FF:  # SC_HEARTBEAT_ACK
        # 하트비트 수신은 주기적이므로 화면 출력 공해 방지를 위해 주석 처리하거나 조용히 처리
        pass
        
    else:
        print(f"\n📥 [수신 알 수 없음] Packet ID: 0x{packet_id:04X}, Length: {len(payload)}")
        print("\n📝 입력: ", end="", flush=True)

def heartbeat_loop(sock):
    while True:
        try:
            time.sleep(10)  # 10초마다 하트비트 발송
            hb = pb.LoginRequest() # 빈 요청
            send_packet(sock, 0x00FF, hb)
        except:
            break

def main():
    host = 'localhost'
    port = 7777
    
    print(f"🚀 C++ TCP 게이트웨이 서버({host}:{port})로 연결을 시도합니다...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect((host, port))
        print("✅ 서버 연결 성공!")
    except Exception as e:
        print(f"❌ 연결 실패: {e}")
        return

    # 수신 스레드 시작
    rx_thread = threading.Thread(target=receive_loop, args=(sock,), daemon=True)
    rx_thread.start()

    # 하트비트 스레드 시작
    hb_thread = threading.Thread(target=heartbeat_loop, args=(sock,), daemon=True)
    hb_thread.start()

    # 1. 로그인 요청 전송
    login_req = pb.LoginRequest()
    login_req.player_id = "player_lucas"
    login_req.player_name = "루카스"
    send_packet(sock, 0x0001, login_req)

    time.sleep(1.5)

    # 2. 이동 요청 전송
    move_req = pb.PlayerMoveRequest()
    move_req.target_location = "광장"
    send_packet(sock, 0x0002, move_req)

    # 3. 사용자 대화 입력 루프
    print("\n[명령어 가이드]")
    print("- talk <npc_id> (예: talk npc_eva)")
    print("- msg <session_id> <메시지> (예: msg session_123 안녕)")
    print("- end <session_id> (예: end session_123)")
    print("- move <location_name> (예: move 여관)")
    print("- quit (종료)")

    while True:
        try:
            user_input = input("\n📝 입력: ")
            if not user_input:
                continue
                
            parts = user_input.split(maxsplit=2)
            cmd = parts[0].lower()
            
            if cmd == 'quit':
                break
            elif cmd == 'talk' and len(parts) >= 2:
                req = pb.TalkToNpcRequest()
                req.npc_id = parts[1]
                send_packet(sock, 0x0003, req)
            elif cmd == 'msg' and len(parts) >= 3:
                req = pb.PlayerMessageRequest()
                req.session_id = parts[1]
                req.message = parts[2]
                send_packet(sock, 0x0004, req)
            elif cmd == 'end' and len(parts) >= 2:
                req = pb.EndDialogueRequest()
                req.session_id = parts[1]
                send_packet(sock, 0x0005, req)
            elif cmd == 'move' and len(parts) >= 2:
                req = pb.PlayerMoveRequest()
                req.target_location = parts[1]
                send_packet(sock, 0x0002, req)
            else:
                print("❌ 잘못된 명령어 형식입니다.")
        except KeyboardInterrupt:
            break

    sock.close()
    print("🔌 연결을 종료하고 클라이언트를 마칩니다.")

if __name__ == '__main__':
    main()
