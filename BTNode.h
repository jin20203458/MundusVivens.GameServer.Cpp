#pragma once
#include <memory>
#include <vector>
#include <string>
#include <entt/entt.hpp>

// Forward declarations
namespace MundusVivens { class AsyncGrpcClient; }
class GrpcResultQueue;
class LocationRegistry;

namespace BT {

struct BTContext {
    MundusVivens::AsyncGrpcClient* client = nullptr;
    GrpcResultQueue* grpc_queue = nullptr;
    LocationRegistry* location_registry = nullptr;
    int* current_tick = nullptr;
};

enum class NodeStatus {
    Success,
    Failure,
    Running
};

class BTNode {
public:
    virtual ~BTNode() = default;
    virtual NodeStatus Tick(entt::registry& reg, entt::entity entity) = 0;
};

// Composite: Selector (Fallback / OR)
class Selector : public BTNode {
public:
    void AddChild(std::unique_ptr<BTNode> child) {
        children_.push_back(std::move(child));
    }
    
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        // Selector: 자식 중 하나가 Running이나 Success를 반환하면 즉시 반환
        // 모든 자식이 Failure를 반환하면 최종 Failure 반환
        for (auto& child : children_) {
            NodeStatus status = child->Tick(reg, entity);
            if (status != NodeStatus::Failure) {
                return status;
            }
        }
        return NodeStatus::Failure;
    }

private:
    std::vector<std::unique_ptr<BTNode>> children_;
};

// Composite: Sequence (AND)
class Sequence : public BTNode {
public:
    void AddChild(std::unique_ptr<BTNode> child) {
        children_.push_back(std::move(child));
    }
    
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        // Sequence: 자식 중 하나가 Running이나 Failure를 반환하면 즉시 반환
        // 모든 자식이 Success를 반환하면 최종 Success 반환
        for (auto& child : children_) {
            NodeStatus status = child->Tick(reg, entity);
            if (status != NodeStatus::Success) {
                return status;
            }
        }
        return NodeStatus::Success;
    }

private:
    std::vector<std::unique_ptr<BTNode>> children_;
};

// Decorator: Inverter (NOT)
class Inverter : public BTNode {
public:
    explicit Inverter(std::unique_ptr<BTNode> child) : child_(std::move(child)) {}
    
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        NodeStatus status = child_->Tick(reg, entity);
        if (status == NodeStatus::Success) return NodeStatus::Failure;
        if (status == NodeStatus::Failure) return NodeStatus::Success;
        return NodeStatus::Running;
    }

private:
    std::unique_ptr<BTNode> child_;
};

} // namespace BT
