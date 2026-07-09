#include "GridMap.h"
#include <queue>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>

GridMap::GridMap() {
    grid_.assign(WIDTH * HEIGHT, true);
}

void GridMap::LoadMap(const std::vector<MundusVivens::LocationData>& locations) {
    // 1. C#에서 넘겨준 부트스트랩 데이터로 거점 좌표 동적 구성 (하드코딩 완전 제거)
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

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    // 초간단 JSON 파서 (정규식이나 라이브러리 없이 토큰 분리)
    // 패턴: {"min_x": X, "min_z": Z, "max_x": X, "max_z": Z}
    size_t pos = 0;
    int loaded_count = 0;
    while ((pos = content.find('{', pos)) != std::string::npos) {
        size_t end_pos = content.find('}', pos);
        if (end_pos == std::string::npos) break;

        std::string entry = content.substr(pos, end_pos - pos);
        pos = end_pos + 1;

        auto get_value = [&](const std::string& key) -> int {
            size_t kpos = entry.find(key);
            if (kpos == std::string::npos) return -1;
            size_t cpos = entry.find(':', kpos);
            if (cpos == std::string::npos) return -1;
            
            // 콜론 다음 숫자 시작점 탐색
            size_t start = cpos + 1;
            while (start < entry.size() && (std::isspace(entry[start]) || entry[start] == '"')) {
                start++;
            }
            size_t end = start;
            while (end < entry.size() && (std::isdigit(entry[end]) || entry[end] == '-')) {
                end++;
            }
            if (start == end) return -1;
            return std::stoi(entry.substr(start, end - start));
        };

        int min_x = get_value("min_x");
        int min_z = get_value("min_z");
        int max_x = get_value("max_x");
        int max_z = get_value("max_z");

        if (min_x >= 0 && min_z >= 0 && max_x >= 0 && max_z >= 0) {
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
        std::cerr << "⚠️ [A* 길찾기 실패] 목표지점 타일이 갈 수 없는 곳(장애물)입니다. 목적지: (" << ex << ", " << ez << ")" << std::endl;
        std::cerr << "📌 [주변 타일 상태]:" << std::endl;
        for (int nz = ez + 1; nz >= ez - 1; --nz) {
            for (int nx = ex - 1; nx <= ex + 1; ++nx) {
                if (nx == ex && nz == ez) std::cerr << " [X]"; // target
                else std::cerr << " [" << (IsWalkable(nx, nz) ? "O" : "W") << "]";
            }
            std::cerr << std::endl;
        }
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
    } else {
        std::cerr << "⚠️ [A* 길찾기 실패] 시작지점에서 목적지로 도달하는 경로가 존재하지 않습니다." << std::endl;
        std::cerr << "   - 시작: (" << sx << ", " << sz << ") [Walkable: " << (IsWalkable(sx, sz) ? "Yes" : "No") << "]" << std::endl;
        std::cerr << "   - 목적: (" << ex << ", " << ez << ") [Walkable: " << (IsWalkable(ex, ez) ? "Yes" : "No") << "]" << std::endl;
    }

    return path;
}
