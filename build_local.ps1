# build_local.ps1
param(
    [switch]$Clean
)

# 1. 캐시만 부분 제거 (Clean 옵션 시)
if ($Clean) {
    Write-Host "... vcpkg 의존성은 유지하고, CMake 임시 캐시 파일만 선택적 삭제합니다..." -ForegroundColor Yellow
    $Targets = @(
        "out/build/windows-default/CMakeCache.txt",
        "out/build/windows-default/CMakeFiles",
        "out/build/CMakeCache.txt",
        "out/build/CMakeFiles"
    )
    foreach ($Target in $Targets) {
        if (Test-Path -Path $Target) {
            Remove-Item -Path $Target -Recurse -Force
        }
    }
}

# 2. 설치된 Visual Studio의 vcvars64.bat 경로 자동 탐색 (vswhere 활용)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = $null

if (Test-Path $vswhere) {
    $vsInstallPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsInstallPath) {
        $candidate = Join-Path $vsInstallPath "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $candidate) {
            $vcvars = $candidate
        }
    }
}

# 자동 감지 실패 시 하드코딩된 기본값 사용 (폴백)
if (-not $vcvars) {
    $vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
}

# 3. 개발자 쉘을 로드하여 빌드 실행
cmd.exe /c "call `"$vcvars`" && cmake --preset windows-default && cmake --build out/build/windows-default --config Debug"

