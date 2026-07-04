#include "GridMap.h"
#include <queue>
#include <cmath>
#include <algorithm>
#include <iostream>

GridMap::GridMap() {
    for (int x = 0; x < WIDTH; ++x) {
        for (int z = 0; z < HEIGHT; ++z) {
            grid_[x][z] = true; // 기본적으로 모두 이동 가능
        }
    }
}

void GridMap::LoadMap() {
    // 1. 8대 주요 거점 좌표 사전 채우기 (Mundus Vivens 전체 맵 연동)
    location_coords_["Manor"] = {85.0f, 90.0f};        // 영주 저택
    location_coords_["Church"] = {20.0f, 80.0f};       // 성당
    location_coords_["Guard Post"] = {15.0f, 20.0f};   // 경비 초소
    location_coords_["Alchemy Lab"] = {80.0f, 70.0f};  // 연금술 공방
    location_coords_["Square"] = {50.0f, 50.0f};       // 마을 광장
    location_coords_["Forge"] = {70.0f, 30.0f};        // 대장간
    location_coords_["Back Alley"] = {15.0f, 50.0f};   // 뒷골목
    location_coords_["Tavern"] = {30.0f, 40.0f};       // 술집

    // 2. 간단한 장애물 배치 (예: 중앙부 수직 벽 x=45, z=30~70)
    // A*가 이 벽을 피해 가는지 검증하기 위함
    for (int z = 30; z <= 70; ++z) {
        grid_[45][z] = false;
    }
}

bool GridMap::IsWalkable(int x, int z) const {
    if (x < 0 || x >= WIDTH || z < 0 || z >= HEIGHT) return false;
    return grid_[x][z];
}

bool GridMap::GetLocationCoords(const std::string& loc_name, float& out_x, float& out_z) const {
    // C# 단에서 보내주는 "술집 (Tavern)" 등 한/영 혼용 표기 매핑을 위해 부분 문자열 비교 수행
    for (const auto& [name, coords] : location_coords_) {
        if (loc_name.find(name) != std::string::npos || name.find(loc_name) != std::string::npos) {
            out_x = coords.x;
            out_z = coords.z;
            return true;
        }
    }
    return false;
}

// A* 내부 탐색용 노드
struct AStarNode {
    int x;
    int z;
    float g;
    float f;

    bool operator>(const AStarNode& other) const {
        return f > other.f;
    }
};

std::vector<GridVector2> GridMap::FindPath(float start_x, float start_z, float end_x, float end_z) const {
    int sx = std::clamp(static_cast<int>(std::round(start_x)), 0, WIDTH - 1);
    int sz = std::clamp(static_cast<int>(std::round(start_z)), 0, HEIGHT - 1);
    int ex = std::clamp(static_cast<int>(std::round(end_x)), 0, WIDTH - 1);
    int ez = std::clamp(static_cast<int>(std::round(end_z)), 0, HEIGHT - 1);

    std::vector<GridVector2> path;

    // 예외: 시작 지점과 목표 지점이 같은 타일인 경우
    if (sx == ex && sz == ez) {
        path.push_back({static_cast<float>(ex), static_cast<float>(ez)});
        return path;
    }

    // 목표 지점이 갈 수 없는 곳인 경우 바로 리턴
    if (!IsWalkable(ex, ez)) {
        return path;
    }

    // A* 알고리즘 데이터 구조
    std::priority_queue<AStarNode, std::vector<AStarNode>, std::greater<AStarNode>> open_set;
    
    // 비용 초기화 및 부모 노드 추적 맵 캐싱 (thread_local flat vector로 힙 할당 제로화)
    thread_local std::vector<float> g_score;
    thread_local std::vector<std::pair<int, int>> parent;

    g_score.assign(WIDTH * HEIGHT, 1e9f);
    parent.assign(WIDTH * HEIGHT, {-1, -1});

    auto heuristic = [](int x1, int z1, int x2, int z2) -> float {
        // Octile distance (대각선 지원)
        int dx = std::abs(x1 - x2);
        int dz = std::abs(z1 - z2);
        return (dx + dz) + (1.41421356f - 2.0f) * std::min(dx, dz);
    };

    g_score[sx * HEIGHT + sz] = 0.0f;
    float h_start = heuristic(sx, sz, ex, ez);
    open_set.push({sx, sz, 0.0f, h_start});

    // 8방향 오프셋
    const int dx[] = { -1, 1, 0, 0, -1, -1, 1, 1 };
    const int dz[] = { 0, 0, -1, 1, -1, 1, -1, 1 };
    const float move_cost[] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.41421356f, 1.41421356f, 1.41421356f, 1.41421356f };

    bool found = false;

    while (!open_set.empty()) {
        auto curr = open_set.top();
        open_set.pop();

        if (curr.x == ex && curr.z == ez) {
            found = true;
            break;
        }

        if (curr.g > g_score[curr.x * HEIGHT + curr.z]) continue;

        for (int i = 0; i < 8; ++i) {
            int nx = curr.x + dx[i];
            int nz = curr.z + dz[i];

            if (IsWalkable(nx, nz)) {
                // 대각선 이동 시 모퉁이를 뚫고 이동하지 못하도록 체크
                if (i >= 4) {
                    if (!IsWalkable(nx, curr.z) || !IsWalkable(curr.x, nz)) {
                        continue;
                    }
                }

                float tentative_g = curr.g + move_cost[i];
                int n_idx = nx * HEIGHT + nz;
                if (tentative_g < g_score[n_idx]) {
                    g_score[n_idx] = tentative_g;
                    parent[n_idx] = {curr.x, curr.z};
                    open_set.push({nx, nz, tentative_g, tentative_g + heuristic(nx, nz, ex, ez)});
                }
            }
        }
    }

    if (found) {
        int cx = ex;
        int cz = ez;
        std::vector<GridVector2> rev_path;
        while (cx != -1 && cz != -1) {
            rev_path.push_back({static_cast<float>(cx), static_cast<float>(cz)});
            auto p = parent[cx * HEIGHT + cz];
            cx = p.first;
            cz = p.second;
        }
        std::reverse(rev_path.begin(), rev_path.end());
        path = std::move(rev_path);
    }

    return path;
}
