import socket
import struct
import sys
import threading
import time
import random

try:
    import mundus_vivens_pb2 as pb
except ImportError:
    print("⚠️  mundus_vivens_pb2.py를 찾을 수 없습니다. 프로토콜 컴파일이 선행되어야 합니다.")
    sys.exit(1)

LOCATIONS = ["광장", "여관", "대장간", "시장"]
NPC_IDS = ["npc_eva", "npc_bart", "npc_maya", "npc_jake", "npc_sophia"]

class DummyClient:
    def __init__(self, bot_id):
        self.bot_id = bot_id
        self.player_id = f"bot_{bot_id}"
        self.player_name = f"더미봇_{bot_id}"
        self.sock = None
        self.running = False
        self.rx_thread = None
        self.logic_thread = None

    def connect(self, host, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.sock.connect((host, port))
            self.running = True
            
            # Start receiver
            self.rx_thread = threading.Thread(target=self.receive_loop, daemon=True)
            self.rx_thread.start()
            
            # Start logic
            self.logic_thread = threading.Thread(target=self.logic_loop, daemon=True)
            self.logic_thread.start()
            return True
        except Exception as e:
            print(f"[Bot {self.bot_id}] Connect failed: {e}")
            return False

    def disconnect(self):
        self.running = False
        if self.sock:
            self.sock.close()

    def send_packet(self, packet_id, protobuf_msg):
        if not self.running: return
        try:
            payload = protobuf_msg.SerializeToString()
            length = 4 + len(payload)
            header = struct.pack('>HH', length, packet_id)
            self.sock.sendall(header + payload)
        except Exception as e:
            self.running = False
            # print(f"[Bot {self.bot_id}] Send failed: {e}")

    def receive_loop(self):
        try:
            while self.running:
                header = self.sock.recv(4)
                if not header or len(header) < 4:
                    self.running = False
                    break
                
                length, packet_id = struct.unpack('>HH', header)
                payload_len = length - 4
                
                payload = b''
                while len(payload) < payload_len:
                    chunk = self.sock.recv(payload_len - len(payload))
                    if not chunk:
                        break
                    payload += chunk
                    
                # Just discard or parse specific if needed
                pass
        except Exception as e:
            self.running = False

    def logic_loop(self):
        # 1. Login
        login_req = pb.LoginRequest()
        login_req.player_id = self.player_id
        login_req.player_name = self.player_name
        self.send_packet(0x0001, login_req)
        
        time.sleep(2)
        
        while self.running:
            # Random action: Move or Talk or Idle
            action = random.random()
            if action < 0.3:
                # Move
                move_req = pb.PlayerMoveRequest()
                move_req.target_location = random.choice(LOCATIONS)
                self.send_packet(0x0002, move_req)
            elif action < 0.4:
                # Talk
                talk_req = pb.TalkToNpcRequest()
                talk_req.npc_id = random.choice(NPC_IDS)
                self.send_packet(0x0003, talk_req)
            
            # Send Heartbeat
            hb = pb.LoginRequest()
            self.send_packet(0x00FF, hb)
            
            time.sleep(random.uniform(2.0, 5.0))

def main():
    num_bots = 50
    if len(sys.argv) > 1:
        num_bots = int(sys.argv[1])
        
    print(f"🚀 Starting {num_bots} dummy bots...")
    
    bots = []
    for i in range(num_bots):
        bot = DummyClient(i+1)
        if bot.connect('localhost', 7777):
            bots.append(bot)
        time.sleep(0.05) # Stagger connections
        
    print(f"✅ {len(bots)} bots connected and running. Press Ctrl+C to stop.")
    
    try:
        while True:
            time.sleep(1)
            active = sum(1 for b in bots if b.running)
            print(f"\rActive bots: {active}/{len(bots)}", end="")
            if active == 0:
                break
    except KeyboardInterrupt:
        print("\nStopping bots...")
    
    for bot in bots:
        bot.disconnect()

if __name__ == '__main__':
    main()
