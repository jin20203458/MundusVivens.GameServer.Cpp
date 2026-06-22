#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include <entt/entt.hpp>
#include "PacketProtocol.h"

// 플레이어가 보낸 패킷 데이터를 메인 게임 루프 스레드에서 처리할 수 있도록 큐잉하는 구조체
struct PlayerCommand {
    enum Type { 
        Login, 
        Move, 
        TalkToNpc, 
        PlayerMessage, 
        EndDialogue 
    };
    Type type;
    std::string player_id;
    std::string payload;     // 직렬화된 Protobuf 메시지 데이터
    uint32_t session_index;  // 발신 클라이언트의 세션 인덱스
};

class ClientSession;

class TcpServer {
public:
    TcpServer(boost::asio::io_context& io, uint16_t port);
    ~TcpServer();

    // 서버 시작 (Accept 루프 시작)
    void Start();

    // 접속 중인 모든 클라이언트에 패킷 브로드캐스트 (Thread-safe)
    void BroadcastPacket(uint16_t packet_id, const std::string& payload);

    // 특정 세션에 패킷 전송 (Thread-safe)
    void SendTo(uint32_t session_index, uint16_t packet_id, const std::string& payload);

    // 메인 루프에서 호출: 쌓여 있는 플레이어 명령어 리스트를 가져오고 큐를 비움 (Thread-safe)
    std::vector<PlayerCommand> DrainPlayerCommands();

    // 세션 등록 및 해제 (ClientSession에서 호출)
    void RegisterSession(std::shared_ptr<ClientSession> session);
    void UnregisterSession(uint32_t session_index);

    // 클라이언트로부터 수신된 명령어를 큐에 추가 (ClientSession에서 호출, Thread-safe)
    void QueueCommand(PlayerCommand cmd);

private:
    // C++20 코루틴 기반 커넥션 수락 루프
    boost::asio::awaitable<void> AcceptLoop();

    boost::asio::io_context& io_;
    boost::asio::ip::tcp::acceptor acceptor_;

    std::mutex sessions_mutex_;
    std::vector<std::shared_ptr<ClientSession>> sessions_;
    uint32_t next_session_id_ = 1;

    std::mutex commands_mutex_;
    std::vector<PlayerCommand> pending_commands_;

    bool is_running_ = false;
};
