#include "GridMap.h"
#include <queue>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

GridMap::GridMap() {
    grid_.assign(WIDTH * HEIGHT, true);
}

void GridMap::LoadMap(const std::vector<MundusVivens::LocationData>& locations) {
    // 1. C#에서 넘겨준 부트스트랩 데이터로 거점 좌표 동적 구성
    for (const auto& loc : locations) {
        location_coords_[loc.name] = { loc.x, loc.z };
        std::cout << "🗺️ [GridMap] 거점 로드 완료: " << loc.name << " (" 
                  << loc.x << ", " << loc.z << ")" << std::endl;
    }

    // 2. 외부 JSON 파일로부터 장애물 목록 로드
    const std::string filename = "collision_obstacles.json";
    std::ifstream file(filename);
    if (!file.is_open()) {
        // 파일이 없으면 기본 장애물(x=45, z=30~70 수직 벽) 생성
        std::cout << "⚠️ [GridMap] " << filename << "이 존재하지 않아 기본 장애물(x=45, z=30~70 장벽)을 생성합니다." << std::endl;
        std::ofstream outfile(filename);
        if (outfile.is_open()) {
            outfile << "[\n  { \"min_x\": 45, \"min_z\": 30, \"max_x\": 45, \"max_z\": 70 }\n]\n";
            outfile.close();
        }
        
        for (int z = 30; z <= 70; ++z) {
            grid_[45 * HEIGHT + z] = false;
        }
        return;
    }

    try {
        nlohmann::json j;
        file >> j;
        file.close();

        int loaded_count = 0;
        for (const auto& entry : j) {
            if (entry.contains("min_x") && entry.contains("min_z") &&
                entry.contains("max_x") && entry.contains("max_z")) {
                
                int min_x = entry["min_x"].get<int>();
                int min_z = entry["min_z"].get<int>();
                int max_x = entry["max_x"].get<int>();
                int max_z = entry["max_z"].get<int>();

                // 바운더리 클램핑
                min_x = std::clamp(min_x, 0, WIDTH - 1);
                max_x = std::clamp(max_x, 0, WIDTH - 1);
                min_z = std::clamp(min_z, 0, HEIGHT - 1);
                max_z = std::clamp(max_z, 0, HEIGHT - 1);

                for (int x = min_x; x <= max_x; ++x) {
                    for (int z = min_z; z <= max_z; ++z) {
                        grid_[x * HEIGHT + z] = false;
                    }
                }
                std::cout << "🧱 [GridMap 장애물 로드] 사각형 영역: (" << min_x << ", " << min_z 
                          << ") ~ (" << max_x << ", " << max_z << ")" << std::endl;
                loaded_count++;
            }
        }
        std::cout << "🧱 [GridMap] 총 " << loaded_count << "개의 동적 장애물 로드 완료." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "❌ [GridMap 에러] JSON 파싱 중 예외 발생: " << e.what() << std::endl;
        file.close();
    }
}

bool GridMap::IsWalkable(int x, int z) const {
    if (x < 0 || x >= WIDTH || z < 0 || z >= HEIGHT) return false;
    return grid_[x * HEIGHT + z];
}

bool GridMap::GetLocationCoords(const std::string& loc_name, float& out_x, float& out_z) const {
    auto it = location_coords_.find(loc_name);
    if (it != location_coords_.end()) {
        out_x = it->second.x;
        out_z = it->second.z;
        return true;
    }
    
    std::cerr << "❌ [GridMap 에러] 거점 좌표 조회 실패: '" << loc_name << "'을 찾을 수 없습니다." << std::endl;
    std::cerr << "📌 [GridMap 정보] 현재 등록된 거점 목록:" << std::endl;
    for (const auto& [name, coords] : location_coords_) {
        std::cerr << "   - '" << name << "' (" << coords.x << ", " << coords.z << ")" << std::endl;
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

PathResult GridMap::FindPath(float start_x, float start_z, float end_x, float end_z, int max_iterations) const {
    int sx = std::clamp(static_cast<int>(std::round(start_x)), 0, WIDTH - 1);
    int sz = std::clamp(static_cast<int>(std::round(start_z)), 0, HEIGHT - 1);
    int ex = std::clamp(static_cast<int>(std::round(end_x)), 0, WIDTH - 1);
    int ez = std::clamp(static_cast<int>(std::round(end_z)), 0, HEIGHT - 1);

    PathResult result;

    // 예외: 시작 지점과 목표 지점이 같은 타일인 경우
    if (sx == ex && sz == ez) {
        result.waypoints.push_back({static_cast<float>(ex), static_cast<float>(ez)});
        return result;
    }

    // 목표 지점이 갈 수 없는 곳인 경우 바로 리턴
    if (!IsWalkable(ex, ez)) {
        result.is_failed = true;
        return result;
    }

    // Phase 3: Destination Cluster Cache 확인 (8x8 타일 버킷)
    PathCacheKey cache_key{ sx / 8, sz / 8, ex / 8, ez / 8 };
    auto cache_it = path_cache_.find(cache_key);
    if (cache_it != path_cache_.end() && !cache_it->second.waypoints.empty()) {
        result.waypoints = cache_it->second.waypoints;
        result.is_partial = false;
        result.is_failed = false;
        return result;
    }

    // A* 알고리즘 데이터 구조
    std::priority_queue<AStarNode, std::vector<AStarNode>, std::greater<AStarNode>> open_set;
    
    // 비용 초기화 및 부모 노드 추적 맵 캐싱 (thread_local flat vector로 힙 할당 제로화)
    thread_local std::vector<float> g_score(WIDTH * HEIGHT, 1e9f);
    thread_local std::vector<std::pair<int, int>> parent(WIDTH * HEIGHT, {-1, -1});
    thread_local std::vector<int> visited_nodes;

    // 전체 배열을 초기화하는 대신 이전 탐색에서 방문했던 노드들만 O(K)로 선택적 초기화
    for (int idx : visited_nodes) {
        g_score[idx] = 1e9f;
        parent[idx] = {-1, -1};
    }
    visited_nodes.clear();

    auto heuristic = [](int x1, int z1, int x2, int z2) -> float {
        // Octile distance (대각선 지원)
        int dx = std::abs(x1 - x2);
        int dz = std::abs(z1 - z2);
        return (dx + dz) + (1.41421356f - 2.0f) * std::min(dx, dz);
    };

    g_score[sx * HEIGHT + sz] = 0.0f;
    visited_nodes.push_back(sx * HEIGHT + sz);
    float h_start = heuristic(sx, sz, ex, ez);
    open_set.push({sx, sz, 0.0f, h_start});

    // 8방향 오프셋
    const int dx[] = { -1, 1, 0, 0, -1, -1, 1, 1 };
    const int dz[] = { 0, 0, -1, 1, -1, 1, -1, 1 };
    const float move_cost[] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.41421356f, 1.41421356f, 1.41421356f, 1.41421356f };

    bool found = false;
    bool hit_cap = false;
    int iterations = 0;

    // Best-so-far 추적 (Cap 도달 시 폴백용)
    int best_x = sx;
    int best_z = sz;
    float best_h = h_start;

    while (!open_set.empty()) {
        if (++iterations > max_iterations) {
            hit_cap = true;
            break;
        }

        auto curr = open_set.top();
        open_set.pop();

        if (curr.x == ex && curr.z == ez) {
            found = true;
            best_x = curr.x;
            best_z = curr.z;
            break;
        }

        if (curr.g > g_score[curr.x * HEIGHT + curr.z]) continue;

        // Best-so-far 갱신
        float curr_h = heuristic(curr.x, curr.z, ex, ez);
        if (curr_h < best_h) {
            best_h = curr_h;
            best_x = curr.x;
            best_z = curr.z;
        }

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
                    if (g_score[n_idx] >= 1e9f) {
                        visited_nodes.push_back(n_idx);
                    }
                    g_score[n_idx] = tentative_g;
                    parent[n_idx] = {curr.x, curr.z};
                    open_set.push({nx, nz, tentative_g, tentative_g + heuristic(nx, nz, ex, ez)});
                }
            }
        }
    }

    int trace_x = found ? ex : best_x;
    int trace_z = found ? ez : best_z;

    // 전혀 전진하지 못한 경우 (시작 지점과 동일)
    if (trace_x == sx && trace_z == sz) {
        result.is_failed = true;
        return result;
    }

    std::vector<GridVector2> rev_path;
    int cx = trace_x;
    int cz = trace_z;
    while (cx != -1 && cz != -1) {
        rev_path.push_back({static_cast<float>(cx), static_cast<float>(cz)});
        auto p = parent[cx * HEIGHT + cz];
        cx = p.first;
        cz = p.second;
    }
    std::reverse(rev_path.begin(), rev_path.end());
    result.waypoints = std::move(rev_path);
    result.is_partial = hit_cap && !found;
    result.is_failed = false;

    // Phase 3: 완결 경로에 대해 Destination Cluster Cache에 저장 (최대 500개 유지)
    if (found && !result.waypoints.empty()) {
        if (path_cache_.size() > 500) {
            path_cache_.clear();
        }
        path_cache_[cache_key] = PathCacheEntry{ result.waypoints, 0 };
    }

    return result;
}

bool GridMap::IsPathBlocked(float x1, float z1, float x2, float z2) const {
    int ix1 = std::clamp(static_cast<int>(std::round(x1)), 0, WIDTH - 1);
    int iz1 = std::clamp(static_cast<int>(std::round(z1)), 0, HEIGHT - 1);
    int ix2 = std::clamp(static_cast<int>(std::round(x2)), 0, WIDTH - 1);
    int iz2 = std::clamp(static_cast<int>(std::round(z2)), 0, HEIGHT - 1);

    int dx = std::abs(ix2 - ix1);
    int dy = std::abs(iz2 - iz1);
    int sx = (ix1 < ix2) ? 1 : -1;
    int sy = (iz1 < iz2) ? 1 : -1;
    int err = dx - dy;

    int cx = ix1;
    int cz = iz1;

    while (true) {
        if (!IsWalkable(cx, cz)) {
            return true; // 장애물 있음 -> Blocked
        }
        if (cx == ix2 && cz == iz2) {
            break;
        }
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            cx += sx;
        }
        if (e2 < dx) {
            err += dx;
            cz += sy;
        }
    }
    return false; // 장애물 없음 -> Clear
}
