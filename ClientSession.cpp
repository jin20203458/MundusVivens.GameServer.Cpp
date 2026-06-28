#include "ClientSession.h"
#include "TcpServer.h"
#include "mundus_vivens.pb.h"
#include <iostream>
#include <chrono>
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

ClientSession::ClientSession(boost::asio::ip::tcp::socket socket, TcpServer& server, uint32_t index)
    : socket_(std::move(socket)), server_(server), index_(index) {
}

ClientSession::~ClientSession() {
    try {
        if (socket_.is_open()) {
            socket_.close();
        }
    } catch (...) {}
    std::cout << "[ClientSession] 세션 객체 소멸 (인덱스: " << index_ << ")" << std::endl;
}

boost::asio::awaitable<void> ClientSession::Run() {
    auto self = shared_from_this(); // 비동기 작업 중 객체 수명 보장
    try {
        // Nagle 알고리즘 비활성화 (저지연 최적화 - Task F-1)
        socket_.set_option(boost::asio::ip::tcp::no_delay(true));

        while (true) {
            // 백프레셔 흐름 제어 활성화 시 코루틴 일시 정지 (Task F-2)
            while (is_reading_suspended_) {
                boost::asio::steady_timer timer(socket_.get_executor(), std::chrono::milliseconds(50));
                co_await timer.async_wait(boost::asio::use_awaitable);
            }

            // 1. 헤더 4바이트 수신
            uint8_t header_buf[HEADER_SIZE];
            co_await boost::asio::async_read(
                socket_, 
                boost::asio::buffer(header_buf, HEADER_SIZE), 
                boost::asio::use_awaitable
            );

            uint16_t length = ReadU16BE(header_buf);
            uint16_t packet_id = ReadU16BE(header_buf + 2);

            if (length < HEADER_SIZE || length > MAX_PACKET_SIZE) {
                std::cerr << "[ClientSession] 잘못된 패킷 크기 수신 (크기: " << length << "). 연결을 종료합니다." << std::endl;
                break;
            }

            size_t payload_size = length - HEADER_SIZE;
            
            // Small Buffer Optimization (SBO): 256바이트 이하는 스택 버퍼 활용하여 힙 할당 Zero화 (Task F-3)
            static constexpr size_t SBO_SIZE = 256;
            if (payload_size <= SBO_SIZE) {
                alignas(16) uint8_t stack_buf[SBO_SIZE];
                if (payload_size > 0) {
                    co_await boost::asio::async_read(
                        socket_, 
                        boost::asio::buffer(stack_buf, payload_size), 
                        boost::asio::use_awaitable
                    );
                }
                HandlePacket(packet_id, stack_buf, payload_size);
            } else {
                std::vector<uint8_t> heap_buf(payload_size);
                co_await boost::asio::async_read(
                    socket_, 
                    boost::asio::buffer(heap_buf.data(), payload_size), 
                    boost::asio::use_awaitable
                );
                HandlePacket(packet_id, heap_buf.data(), payload_size);
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[ClientSession] 세션 수신 루프 예외 발생 (인덱스: " << index_ 
                  << ", 원인: " << e.what() << ")" << std::endl;
    }

    // 소켓 정리 및 서버 언레지스터
    try {
        if (socket_.is_open()) {
            socket_.close();
        }
    } catch (...) {}

    server_.UnregisterSession(index_);
}

void ClientSession::Send(uint16_t packet_id, const std::string& payload) {
    // 패킷 데이터 조립 (헤더 4바이트 + 페이로드)
    uint16_t total_length = static_cast<uint16_t>(HEADER_SIZE + payload.size());
    std::vector<uint8_t> send_buf(total_length);

    WriteU16BE(send_buf.data(), total_length);
    WriteU16BE(send_buf.data() + 2, packet_id);
    if (!payload.empty()) {
        std::memcpy(send_buf.data() + HEADER_SIZE, payload.data(), payload.size());
    }

    // 스레드 안전하게 큐에 넣고 비동기 쓰기 구동
    bool start_write = false;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_queue_.push_back(std::move(send_buf));
        if (!is_writing_) {
            is_writing_ = true;
            start_write = true;
        }
    }

    if (start_write) {
        // WriteLoop를 io_context의 스레드에서 구동
        boost::asio::co_spawn(
            socket_.get_executor(),
            WriteLoop(),
            boost::asio::detached
        );
    }
}

boost::asio::awaitable<void> ClientSession::WriteLoop() {
    auto self = shared_from_this();
    try {
        while (true) {
            std::vector<uint8_t> current_data;
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                if (write_queue_.empty()) {
                    is_writing_ = false;
                    co_return;
                }
                current_data = std::move(write_queue_.front());
                write_queue_.pop_front();
            }

            // 비동기로 데이터 전송
            co_await boost::asio::async_write(
                socket_,
                boost::asio::buffer(current_data.data(), current_data.size()),
                boost::asio::use_awaitable
            );
        }
    } catch (const std::exception& e) {
        std::cerr << "[ClientSession] 쓰기 루프 예외 발생 (인덱스: " << index_ 
                  << ", 원인: " << e.what() << ")" << std::endl;
        
        // 에러 시 소켓 닫음
        try {
            if (socket_.is_open()) {
                socket_.close();
            }
        } catch (...) {}
    }
}

void ClientSession::HandlePacket(uint16_t packet_id, const uint8_t* payload, size_t size) {
    std::string payload_str(payload, payload + size);

    PlayerCommand cmd;
    cmd.session_index = index_;
    cmd.payload = std::move(payload_str);
    cmd.player_id = player_id_; // 아직 로그인 전이면 비어있음

    switch (packet_id) {
        case PacketId::CS_LOGIN: {
            mundusvivens::LoginRequest req;
            if (req.ParseFromString(cmd.payload)) {
                cmd.type = PlayerCommand::Login;
                cmd.player_id = req.player_id();
                // 세션에 플레이어 ID 기록
                player_id_ = req.player_id();
                server_.QueueCommand(std::move(cmd));
            } else {
                std::cerr << "[ClientSession] CS_LOGIN 파싱 실패 (세션: " << index_ << ")" << std::endl;
            }
            break;
        }
        case PacketId::CS_PLAYER_MOVE: {
            cmd.type = PlayerCommand::Move;
            server_.QueueCommand(std::move(cmd));
            break;
        }
        case PacketId::CS_TALK_TO_NPC: {
            cmd.type = PlayerCommand::TalkToNpc;
            server_.QueueCommand(std::move(cmd));
            break;
        }
        case PacketId::CS_PLAYER_MESSAGE: {
            cmd.type = PlayerCommand::PlayerMessage;
            server_.QueueCommand(std::move(cmd));
            break;
        }
        case PacketId::CS_END_DIALOGUE: {
            cmd.type = PlayerCommand::EndDialogue;
            server_.QueueCommand(std::move(cmd));
            break;
        }
        case PacketId::CS_HEARTBEAT: {
            // 하트비트 수신 시 즉시 ACK 전송
            Send(PacketId::SC_HEARTBEAT_ACK, "");
            break;
        }
        default:
            std::cout << "[ClientSession] 정의되지 않은 패킷 아이디 수신: " << packet_id << std::endl;
            break;
    }
}
