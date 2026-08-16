# Canonical deterministic developer bootstrap for native Windows x64 MSVC.
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

if ($args.Count -eq 1 -and $args[0] -ceq '-Help') {
  Write-Output 'Usage: .\bootstrap.ps1 [-Help]'
  Write-Output 'Provision x64-windows, configure, build, and run CTest.'
  exit 0
}
if ($args.Count -ne 0) { throw 'only the exact -Help token is supported; this command has no repair, offline, or alternate-path mode' }

$Root = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$CallerLocation = Get-Location
$LocationChanged = $false
$OfficialOrigin = 'https://github.com/microsoft/vcpkg.git'
$Triplet = 'x64-windows'; $Preset = 'dev-windows'
$Checkout = Join-Path $Root '.cache/vcpkg/x64-windows'
$Installed = Join-Path $Root 'build/vcpkg-installed/x64-windows'
$BuildTree = Join-Path $Root 'build/dev-windows'
$Lock = Join-Path $Root 'build/bootstrap-locks/x64-windows.lock'
$Stage = 'initialization'; $LockHeld = $false
$LockToken = "$PID-$([guid]::NewGuid().ToString('N'))"
function Stop-Bootstrap([string]$Message) { throw "ERROR [$Stage]: $Message" }
function Invoke-Checked([string]$Label, [scriptblock]$Command) {
  try { & $Command; $code = $LASTEXITCODE } catch { Stop-Bootstrap "$Label failed: $($_.Exception.Message); native exit code unavailable; retry: .\bootstrap.ps1" }
  if ($code -ne 0) { Stop-Bootstrap "$Label failed with native exit code $code; retry: .\bootstrap.ps1" }
}
function Set-Stage([string]$Name) {
  $script:Stage = $Name
  Write-Output "INFO: stage: $Name"
  Write-Output "INFO: paths: checkout=$Checkout installed=$Installed build=$BuildTree lock=$Lock"
}
function Test-ManagedPath([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path).TrimEnd('\','/')
  $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\','/')
  if (-not $full.StartsWith("$rootFull\", [StringComparison]::OrdinalIgnoreCase) -and $full -ne $rootFull) { Stop-Bootstrap "managed path is outside the repository root: $Path" }
  $current = $rootFull
  $relative = $full.Substring($rootFull.Length).TrimStart('\','/')
  $components = if ([string]::IsNullOrEmpty($relative)) { @() } else { $relative -split '[\\/]' }
  foreach ($component in $components) {
    $current = Join-Path $current $component
    if (Test-Path -LiteralPath $current) {
      $item = Get-Item -LiteralPath $current -Force
      if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { Stop-Bootstrap "managed path has a symlink/reparse ancestor: $current" }
    }
  }
}
function Normalize-PathValue([string]$Value) {
  if ([string]::IsNullOrWhiteSpace($Value)) { return '' }
  $full = [IO.Path]::GetFullPath($Value).Replace('\','/').TrimEnd('/')
  return $full.ToLowerInvariant()
}
function Cache-Value([string]$Cache, [string]$Key) {
  $line = Get-Content -LiteralPath $Cache | Where-Object { $_ -match "^$([regex]::Escape($Key))(:[^=]*)?=" } | Select-Object -First 1
  if ($null -eq $line) { return '' }
  return ($line -replace '^[^=]*=', '')
}
function Resolve-NativeApplication([string]$Name) {
  $command = Get-Command -Name $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($null -eq $command -or [string]::IsNullOrWhiteSpace($command.Path)) { Stop-Bootstrap "required host tool is missing: $Name" }
  return [IO.Path]::GetFullPath($command.Path)
}
function Invoke-Git([string[]]$Arguments) {
  $output = & $GitExe @Arguments 2>&1; $code = $LASTEXITCODE
  if ($code -ne 0) { Stop-Bootstrap "Git query failed with native exit code $code; retry: .\bootstrap.ps1" }
  return (($output | Out-String).Trim())
}
$OwnedEnvironment = @{}
foreach ($name in @('VCPKG_ROOT','VCPKG_DOWNLOADS','VCPKG_DISABLE_METRICS','GIT_TERMINAL_PROMPT','GCM_INTERACTIVE')) {
  $OwnedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
}
try {
  Set-Location -LiteralPath $Root
  $LocationChanged = $true
  Set-Stage 'host validation'
  if ($IsWindows -ne $true) { Stop-Bootstrap 'bootstrap.ps1 supports native Windows only' }
  if ($PSVersionTable.PSVersion -lt [version]'7.3') { Stop-Bootstrap 'PowerShell 7.3 or newer is required' }
  $GitExe = Resolve-NativeApplication 'git'
  $CmakeExe = Resolve-NativeApplication 'cmake'
  $NinjaExe = Resolve-NativeApplication 'ninja'
  $ClExe = Resolve-NativeApplication 'cl'
  $CtestExe = Resolve-NativeApplication 'ctest'
  $cmakeVersion = [version]((& $CmakeExe --version | Select-Object -First 1) -replace '^cmake version ', '')
  if ($cmakeVersion -lt [version]'3.28') { Stop-Bootstrap "CMake 3.28+ is required (found $cmakeVersion)" }
  Write-Output "INFO: CMake $cmakeVersion"; Write-Output "INFO: Ninja $(& $NinjaExe --version)"
  Invoke-Checked 'CTest qualification' { & $CtestExe --version }
  Write-Output "INFO: MSVC $((& $ClExe 2>&1 | Select-Object -First 1))"
  if ([string]::IsNullOrWhiteSpace($env:VSCMD_ARG_TGT_ARCH) -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64') { Stop-Bootstrap 'an initialized x64 MSVC Developer PowerShell is required (VSCMD_ARG_TGT_ARCH=x64)' }
  # An initialized MSVC Developer PowerShell may set VCPKG_ROOT to Visual Studio's bundled
  # checkout. This wrapper owns and later restores VCPKG_ROOT, so only other selection inputs
  # remain forbidden.
  foreach ($name in @('VCPKG_OVERLAY_PORTS','VCPKG_OVERLAY_TRIPLETS','VCPKG_CHAINLOAD_TOOLCHAIN_FILE','VCPKG_DEFAULT_TRIPLET','VCPKG_DEFAULT_HOST_TRIPLET','VCPKG_FEATURE_FLAGS')) { if (-not [string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) { Stop-Bootstrap "selection environment $name is set; unset it before retrying" } }
  foreach ($name in @('GIT_DIR','GIT_WORK_TREE','GIT_INDEX_FILE','GIT_OBJECT_DIRECTORY','GIT_ALTERNATE_OBJECT_DIRECTORIES','GIT_COMMON_DIR','GIT_NAMESPACE')) { if (-not [string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) { Stop-Bootstrap "unsafe Git repository environment $name is set; unset it before retrying" } }
  foreach ($name in @('VCPKG_BINARY_SOURCES','VCPKG_ASSET_SOURCES','HTTPS_PROXY','HTTP_PROXY','ALL_PROXY')) { if (-not [string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) { Write-Output "INFO: optional cache/transport override $name is active (value hidden)" } }
  $env:GIT_TERMINAL_PROMPT = '0'
  $env:GCM_INTERACTIVE = 'never'

  Set-Stage 'pin validation'; $PinFile = Join-Path $Root 'tools/vcpkg-tool-commit.txt'
  if (-not (Test-Path -LiteralPath $PinFile -PathType Leaf)) { Stop-Bootstrap "repository-owned pin is missing: $PinFile" }
  $bytes = [IO.File]::ReadAllBytes($PinFile)
  if ($bytes.Length -ne 41 -or $bytes[40] -ne 10) { Stop-Bootstrap 'tool pin must be exactly 41 bytes: 40 lowercase hex bytes and LF' }
  $Pin = [Text.Encoding]::ASCII.GetString($bytes, 0, 40)
  if ($Pin -cnotmatch '\A[0-9a-f]{40}\z') { Stop-Bootstrap 'tool pin must be exactly one lowercase full 40-hex commit' }

  Set-Stage 'lock acquisition'
  foreach ($managed in @($Checkout, $Installed, $BuildTree, $Lock, (Join-Path $Checkout 'downloads'), (Join-Path $Checkout 'scripts/buildsystems/vcpkg.cmake'))) { Test-ManagedPath $managed }
  New-Item -ItemType Directory -Path (Split-Path $Lock) -Force | Out-Null
  try { New-Item -ItemType File -Path $Lock -ErrorAction Stop | Out-Null; $LockHeld = $true } catch { Stop-Bootstrap "exclusive lock exists: $Lock; inspect that no bootstrap is active, then remove it and retry .\bootstrap.ps1" }
  Set-Content -LiteralPath $Lock -Value $LockToken -NoNewline
  $env:VCPKG_ROOT = $Checkout; $env:VCPKG_DOWNLOADS = Join-Path $Checkout 'downloads'; $env:VCPKG_DISABLE_METRICS = '1'
  Write-Output "INFO: repository root: $Root"; Write-Output "INFO: checkout: $Checkout"; Write-Output "INFO: installed tree: $Installed"; Write-Output "INFO: build tree: $BuildTree"; Write-Output "INFO: triplet/preset: $Triplet/$Preset"; Write-Output "INFO: pinned revision: $Pin"
  Write-Output "INFO: network-capable stages may include clone, vcpkg bootstrap, pinned port sources, configured caches, and proxies; official clone origin: $OfficialOrigin"

  Set-Stage 'checkout validation'
  if (-not (Test-Path -LiteralPath $Checkout)) {
    New-Item -ItemType Directory -Path (Split-Path $Checkout) -Force | Out-Null
    Invoke-Checked 'official clone' { & $GitExe clone $OfficialOrigin $Checkout }
    Invoke-Checked 'exact checkout' { & $GitExe -C $Checkout checkout --detach $Pin }
  } elseif (-not (Test-Path -LiteralPath (Join-Path $Checkout '.git') -PathType Container) -or (Get-Item -LiteralPath (Join-Path $Checkout '.git') -Force).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) { Stop-Bootstrap "existing checkout is not a managed Git checkout: $Checkout; move it manually and retry" }
  if ((Invoke-Git @('-C',$Checkout,'rev-parse','--is-inside-work-tree')) -ne 'true') { Stop-Bootstrap "invalid Git checkout: $Checkout" }
  if ((Invoke-Git @('-C',$Checkout,'remote','get-url','origin')) -ne $OfficialOrigin) { Stop-Bootstrap "checkout origin is not the official vcpkg origin: $Checkout" }
  if ((Invoke-Git @('-C',$Checkout,'rev-parse','HEAD')) -ne $Pin) { Stop-Bootstrap "checkout HEAD is not the repository-owned pin: $Checkout" }
  if (-not [string]::IsNullOrWhiteSpace((Invoke-Git @('-C',$Checkout,'status','--porcelain','--untracked-files=no'))) ) { Stop-Bootstrap "checkout has tracked/index dirtiness: $Checkout" }
  $Toolchain = Join-Path $Checkout 'scripts/buildsystems/vcpkg.cmake'; if (-not (Test-Path -LiteralPath $Toolchain -PathType Leaf)) { Stop-Bootstrap "vcpkg checkout is incomplete (toolchain missing): $Toolchain" }
  Test-ManagedPath $env:VCPKG_DOWNLOADS
  New-Item -ItemType Directory -Path $env:VCPKG_DOWNLOADS -Force | Out-Null

  Set-Stage 'vcpkg bootstrap'; $Vcpkg = Join-Path $Checkout 'vcpkg.exe'
  if (-not (Test-Path -LiteralPath $Vcpkg -PathType Leaf)) { if (-not (Test-Path -LiteralPath (Join-Path $Checkout 'bootstrap-vcpkg.bat'))) { Stop-Bootstrap 'vcpkg bootstrap script is missing' }; Invoke-Checked 'vcpkg bootstrap' { & (Join-Path $Checkout 'bootstrap-vcpkg.bat') -disableMetrics } }
  Test-ManagedPath $Vcpkg
  if (-not (Test-Path -LiteralPath $Vcpkg -PathType Leaf)) { Stop-Bootstrap "vcpkg bootstrap did not create an executable: $Vcpkg" }
  Test-ManagedPath $Toolchain
  try { $versionResult = @(& $Vcpkg version 2>&1); $versionCode = $LASTEXITCODE }
  catch { Stop-Bootstrap "vcpkg executable could not report its version: $Vcpkg" }
  $versionOutput = ($versionResult | Out-String).Trim()
  if ($versionCode -ne 0) { Stop-Bootstrap "vcpkg executable could not report its version: $Vcpkg" }
  if ([string]::IsNullOrWhiteSpace($versionOutput) -or $versionOutput -notmatch '(?i)vcpkg.*version') { Stop-Bootstrap "vcpkg executable returned no recognizable version: $Vcpkg" }
  Write-Output "INFO: vcpkg qualification: $versionOutput"

  Set-Stage 'manifest provisioning'; New-Item -ItemType Directory -Path $Installed -Force | Out-Null
  Invoke-Checked 'manifest provisioning' { & $Vcpkg install "--triplet=$Triplet" "--x-manifest-root=$Root" "--x-install-root=$Installed" }
  function Check-Cache {
    $cache = Join-Path $BuildTree 'CMakeCache.txt'
    Test-ManagedPath $cache
    if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) { Stop-Bootstrap "configured cache is missing or not a regular file: $cache" }
    $compiler = $ClExe
    $expected = @{ CMAKE_C_COMPILER=$compiler; CMAKE_CXX_COMPILER=$compiler; CMAKE_TOOLCHAIN_FILE=$Toolchain; VCPKG_TARGET_TRIPLET=$Triplet; VCPKG_INSTALLED_DIR=$Installed; VCPKG_MANIFEST_INSTALL='OFF'; VCPKG_APPLOCAL_DEPS='OFF' }
    foreach ($key in $expected.Keys) {
      $actual = Cache-Value $cache $key
      if ($key -in @('CMAKE_C_COMPILER','CMAKE_CXX_COMPILER','CMAKE_TOOLCHAIN_FILE','VCPKG_INSTALLED_DIR')) { if ((Normalize-PathValue $actual) -ne (Normalize-PathValue $expected[$key])) { Stop-Bootstrap "CMake cache identity mismatch for $key (expected $($expected[$key]))" } }
      elseif ($actual -cne $expected[$key]) { Stop-Bootstrap "CMake cache identity mismatch for $key (expected $($expected[$key]))" }
    }
  }
  if (Test-Path -LiteralPath (Join-Path $BuildTree 'CMakeCache.txt')) { Set-Stage 'pre-configure cache validation'; Check-Cache }
  $Compiler = $ClExe
  Set-Stage 'configure'; Invoke-Checked 'configure' { & $CmakeExe --preset $Preset "-DCMAKE_C_COMPILER=$Compiler" "-DCMAKE_CXX_COMPILER=$Compiler" "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" "-DVCPKG_TARGET_TRIPLET=$Triplet" "-DVCPKG_INSTALLED_DIR=$Installed" '-DVCPKG_MANIFEST_INSTALL=OFF' '-DVCPKG_APPLOCAL_DEPS=OFF' }
  Set-Stage 'post-configure cache validation'; Check-Cache
  Set-Stage 'build'; Invoke-Checked 'build' { & $CmakeExe --build --preset $Preset }
  Set-Stage 'test'; Invoke-Checked 'CTest' { & $CtestExe --preset $Preset }
  Write-Output "SUCCESS: bootstrap completed for $Triplet at $Root"
} catch { Write-Error $_.Exception.Message; throw } finally {
  try {
    if ($LockHeld -and (Test-Path -LiteralPath $Lock) -and ((Get-Content -LiteralPath $Lock -Raw) -ceq "$LockToken`r`n" -or (Get-Content -LiteralPath $Lock -Raw) -ceq $LockToken)) { Remove-Item -LiteralPath $Lock -Force }
  } finally {
    try {
      if ($LocationChanged) { Set-Location -LiteralPath $CallerLocation }
    } finally {
      foreach ($name in $OwnedEnvironment.Keys) {
        if ($null -eq $OwnedEnvironment[$name]) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
        else { Set-Item "Env:$name" $OwnedEnvironment[$name] }
      }
    }
  }
}
