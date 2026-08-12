# Deterministic, network-free contract driver for bootstrap.ps1.
# This is a black-box driver: no production test hook, network, or real sleep is used.
$ErrorActionPreference = 'Stop'
# Negative contract cases intentionally inspect native exit codes instead of promoting them to
# NativeCommandExitException, even when the invoking CI scope enables that preference.
$PSNativeCommandUseErrorActionPreference = $false
$TestRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$PowerShellExe = (Get-Process -Id $PID).Path
$TempRoot = Join-Path ([IO.Path]::GetTempPath()) ('sitometron bootstrap ps1 test.' + [guid]::NewGuid())
$OfficialOrigin = 'https://github.com/microsoft/vcpkg.git'
$Pin = '40f3c709db80acf154ac4b17a1f83c564ebd022e'
$RealCl = [string](Get-Command cl -ErrorAction SilentlyContinue).Source
if ([string]::IsNullOrEmpty($RealCl)) { $RealCl = 'cl.exe' }
$CallerOwnedEnvironment = @{}
foreach ($name in @('VCPKG_ROOT','VCPKG_DOWNLOADS','VCPKG_DISABLE_METRICS')) {
  $CallerOwnedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
}
$CheckNames = @('bootstrap_missing_host_tool','bootstrap_invalid_pin','bootstrap_untrusted_checkout','bootstrap_stage_failure','bootstrap_paths_with_spaces','bootstrap_cold_run','bootstrap_warm_run_non_destructive','bootstrap_forbidden_environment','bootstrap_cache_mismatch','bootstrap_invalid_existing_executable','bootstrap_malformed_pin','bootstrap_lock_contention')
New-Item -ItemType Directory -Path $TempRoot -Force | Out-Null
try {
  function Fail([string]$Message) { throw "FAIL $Message" }
  function Pass([string]$Name) { Write-Output "PASS $Name" }
  function Copy-Case([string]$Name) {
    $case = Join-Path $TempRoot $Name; $repo = Join-Path $case 'repository with spaces'
    New-Item -ItemType Directory -Path $repo -Force | Out-Null
    Get-ChildItem -LiteralPath $TestRoot -Force |
      Where-Object { $_.Name -notin @('.git', '.pi-subagents', '.cache', 'build') } |
      Copy-Item -Destination $repo -Recurse -Force
    New-Item -ItemType Directory -Path (Join-Path $repo 'fake-native-bin') -Force | Out-Null
    return $repo
  }
  function Write-Pin([string]$Repo,[string]$Value=$Pin) {
    $bytes = [Text.Encoding]::ASCII.GetBytes("$Value`n"); [IO.File]::WriteAllBytes((Join-Path $Repo 'tools/vcpkg-tool-commit.txt'),$bytes)
  }
  function Write-Fake-Tools([string]$Repo) {
    $bin = Join-Path $Repo 'fake-native-bin'; $log = Join-Path $Repo 'fake-native.log'
    [IO.File]::WriteAllText($log,'')
    $gitLines = @(
      '@echo off', 'echo git %*>>"%FAKE_LOG%"',
      'if "%1"=="--version" echo git version 2.43.0',
      'if "%1"=="clone" if "%FAKE_FAIL_STAGE%"=="clone" exit /b 97',
      'if "%1"=="clone" goto cloneok',
      'if "%1"=="checkout" exit /b 0',
      ('if "%1"=="-C" if "%3"=="remote" if "%4"=="get-url" if "%FAKE_GIT_ORIGIN%"=="" echo ' + $OfficialOrigin),
      'if "%1"=="-C" if "%3"=="remote" if "%4"=="get-url" if not "%FAKE_GIT_ORIGIN%"=="" echo %FAKE_GIT_ORIGIN%',
      ('if "%1"=="-C" if "%3"=="rev-parse" if "%4"=="HEAD" if "%FAKE_GIT_REV%"=="" echo ' + $Pin),
      'if "%1"=="-C" if "%3"=="rev-parse" if "%4"=="HEAD" if not "%FAKE_GIT_REV%"=="" echo %FAKE_GIT_REV%',
      'if "%1"=="-C" if "%3"=="rev-parse" if "%4"=="--is-inside-work-tree" echo true',
      'if "%1"=="-C" if "%3"=="status" if not "%FAKE_GIT_STATUS%"=="" echo %FAKE_GIT_STATUS%',
      'exit /b 0', ':cloneok', 'set "DEST=%~3"',
      'mkdir "%DEST%\.git" >nul 2>nul',
      'mkdir "%DEST%\scripts\buildsystems" >nul 2>nul',
      'type nul > "%DEST%\scripts\buildsystems\vcpkg.cmake"',
      '>"%DEST%\fake-vcpkg.c" echo #include ^<stdio.h^>',
      '>>"%DEST%\fake-vcpkg.c" echo #include ^<stdlib.h^>',
      '>>"%DEST%\fake-vcpkg.c" echo #include ^<string.h^>',
      '>>"%DEST%\fake-vcpkg.c" echo int main(int argc,char**argv){FILE*f=fopen(getenv("FAKE_LOG"),"a");if(f){fprintf(f,"vcpkg %%s\n",argc^>1?argv[1]:"");fclose(f);}if(argc^>1^&^&strcmp(argv[1],"version")==0){printf("vcpkg package management program version fixture\n");}return(getenv("FAKE_FAIL_STAGE")^&^&strcmp(getenv("FAKE_FAIL_STAGE"),"install")==0^&^&argc^>1^&^&strcmp(argv[1],"install")==0)^?97:0;}',
      '>"%DEST%\bootstrap-vcpkg.bat" echo @echo off',
      '>>"%DEST%\bootstrap-vcpkg.bat" echo echo bootstrap-vcpkg^>^>"%%FAKE_LOG%%"',
      '>>"%DEST%\bootstrap-vcpkg.bat" echo if not exist "%%VCPKG_DOWNLOADS%%\" exit /b 98',
      '>>"%DEST%\bootstrap-vcpkg.bat" echo if "%%FAKE_FAIL_STAGE%%"=="bootstrap" exit /b 97',
      ('>>"%DEST%\bootstrap-vcpkg.bat" echo "' + $RealCl + '" /nologo /Fe:"%%~dp0vcpkg.exe" "%%~dp0fake-vcpkg.c" ^>nul'),
      '>>"%DEST%\bootstrap-vcpkg.bat" echo exit /b %%ERRORLEVEL%%', 'exit /b 0'
    )
    $gitLines | Set-Content -LiteralPath (Join-Path $bin 'git.cmd')
    @('@echo off','echo cl fixture>>"%FAKE_LOG%"','exit /b 0') | Set-Content (Join-Path $bin 'cl.cmd')
    @('@echo off','echo cmake %*>>"%FAKE_LOG%"','if "%1"=="--version" echo cmake version 3.28.1','if "%FAKE_FAIL_STAGE%"=="configure" if "%1"=="--preset" exit /b 97','if "%FAKE_FAIL_STAGE%"=="build" if "%1"=="--build" exit /b 97','if not "%1"=="--preset" exit /b 0','mkdir "%FAKE_BUILD_TREE%" >nul 2>nul','>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo CMAKE_C_COMPILER:FILEPATH=%FAKE_CMAKE_COMPILER%','>>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo CMAKE_CXX_COMPILER:FILEPATH=%FAKE_CMAKE_COMPILER%','>>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo CMAKE_TOOLCHAIN_FILE:FILEPATH=%FAKE_TOOLCHAIN%','>>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo VCPKG_TARGET_TRIPLET:STRING=x64-windows','>>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo VCPKG_INSTALLED_DIR:PATH=%FAKE_INSTALLED%','>>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo VCPKG_MANIFEST_INSTALL:BOOL=OFF','>>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo VCPKG_APPLOCAL_DEPS:BOOL=OFF','exit /b 0') | Set-Content (Join-Path $bin 'cmake.cmd')
    @('@echo off','echo ninja %*>>"%FAKE_LOG%"','if "%1"=="--version" echo 1.11.1','exit /b 0') | Set-Content (Join-Path $bin 'ninja.cmd')
    @('@echo off','echo ctest %*>>"%FAKE_LOG%"','if "%FAKE_FAIL_STAGE%"=="test" exit /b 97','exit /b 0') | Set-Content (Join-Path $bin 'ctest.cmd')
  }
  function Invoke-Wrapper([string]$Repo,[hashtable]$ExtraEnv=@{},[string]$PathOverride=$null,[bool]$InProcess=$false) {
    $save = @{}; foreach ($n in @('Path','FAKE_LOG','FAKE_FAIL_STAGE','FAKE_GIT_ORIGIN','FAKE_GIT_REV','FAKE_GIT_STATUS','VSCMD_ARG_TGT_ARCH','FAKE_BUILD_TREE','FAKE_CMAKE_COMPILER','FAKE_TOOLCHAIN','FAKE_INSTALLED','VCPKG_DEFAULT_TRIPLET','VCPKG_ROOT','VCPKG_DOWNLOADS','VCPKG_DISABLE_METRICS')) { $save[$n] = [Environment]::GetEnvironmentVariable($n) }
    try {
      $env:VSCMD_ARG_TGT_ARCH='x64'; $env:Path=if([string]::IsNullOrEmpty($PathOverride)){"$(Join-Path $Repo 'fake-native-bin');$($save['Path'])"}else{$PathOverride}; $env:FAKE_LOG=Join-Path $Repo 'fake-native.log'
      if (-not $InProcess) { Remove-Item Env:VCPKG_ROOT,Env:VCPKG_DOWNLOADS,Env:VCPKG_DISABLE_METRICS -ErrorAction SilentlyContinue }
      $env:FAKE_BUILD_TREE=Join-Path $Repo 'build/dev-windows'; $env:FAKE_CMAKE_COMPILER=Join-Path $Repo 'fake-native-bin/cl.cmd'; $env:FAKE_TOOLCHAIN=Join-Path $Repo '.cache/vcpkg/x64-windows/scripts/buildsystems/vcpkg.cmake'; $env:FAKE_INSTALLED=Join-Path $Repo 'build/vcpkg-installed/x64-windows'
      foreach($k in $ExtraEnv.Keys){Set-Item "Env:$k" ([string]$ExtraEnv[$k])}
      if ($InProcess) {
        try { $script:WrapperOutput=@(& (Join-Path $Repo 'bootstrap.ps1') 2>&1|Out-String); return 0 }
        catch { $script:WrapperOutput = $_ | Out-String; return 1 }
      }
      $script:WrapperOutput=@(& $PowerShellExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'bootstrap.ps1') 2>&1|Out-String); return [int]$LASTEXITCODE
    } finally { foreach($n in $save.Keys){if($null -eq $save[$n]){Remove-Item "Env:$n" -ErrorAction SilentlyContinue}else{Set-Item "Env:$n" $save[$n]}} }
  }
  function Expect-Reject([string]$Name,[string]$Repo,[hashtable]$Env=@{}) { $status=Invoke-Wrapper $Repo $Env; if($status -eq 0){Fail "$Name unexpectedly passed"}; if($script:WrapperOutput -notmatch '(?i)fail|error|invalid|lock|stage|tool|checkout|pin|environment'){Fail "$Name produced no actionable diagnostic"} }
  $canonical=Join-Path $TestRoot 'bootstrap.ps1'; if(-not(Test-Path $canonical)){Fail "bootstrap_missing_canonical_wrapper: canonical wrapper absent"}; Pass bootstrap_missing_canonical_wrapper
  if($IsWindows -ne $true){Write-Output 'SKIP: PowerShell contract matrix requires native Windows; canonical wrapper preflight passed.'; return}
  function bootstrap_missing_host_tool {$r=Copy-Case missing-host-tool;Write-Pin $r;Write-Fake-Tools $r;Set-Content -LiteralPath (Join-Path $r 'fake-native-bin/cmake.cmd') -Value @('@echo off','echo cmake fixture unavailable 1>&2','exit /b 127');$s=Invoke-Wrapper $r; if($s -eq 0 -or $script:WrapperOutput -notmatch 'cmake|missing|tool|version'){Fail "missing host tool failed to reject: $script:WrapperOutput"};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_invalid_pin {$r=Copy-Case invalid-pin;Write-Pin $r invalid;Write-Fake-Tools $r;Expect-Reject $MyInvocation.MyCommand.Name $r;$l=Get-Content (Join-Path $r 'fake-native.log') -Raw;if($l-match'git .*clone|bootstrap-vcpkg|vcpkg install'){Fail 'invalid pin performed checkout or provisioning work'};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_untrusted_checkout {foreach($v in 'wrong-origin','wrong-revision','tracked-dirtiness','unmanaged-path','ancestor-redirection'){$r=Copy-Case "untrusted-$v";Write-Pin $r;Write-Fake-Tools $r;$c=Join-Path $r '.cache/vcpkg/x64-windows';New-Item -ItemType Directory (Join-Path $c '.git') -Force|Out-Null;if($v-eq'wrong-origin'){$env:FAKE_GIT_ORIGIN='https://example.invalid/untrusted.git'}elseif($v-eq'wrong-revision'){$env:FAKE_GIT_REV='0'*40}elseif($v-eq'tracked-dirtiness'){$env:FAKE_GIT_STATUS=' M ports/example'}elseif($v-eq'ancestor-redirection'){$outside=Join-Path $r '..\outside-ancestor';New-Item -ItemType Directory $outside -Force|Out-Null;Remove-Item (Join-Path $r '.cache/vcpkg') -Recurse -Force;New-Item -ItemType Junction -Path (Join-Path $r '.cache/vcpkg') -Target $outside|Out-Null}else{Remove-Item (Join-Path $c '.git') -Recurse -Force;Set-Content (Join-Path $c unmanaged) x};Expect-Reject "$($MyInvocation.MyCommand.Name)/$v" $r;Remove-Item Env:FAKE_GIT_ORIGIN,Env:FAKE_GIT_REV,Env:FAKE_GIT_STATUS -ErrorAction SilentlyContinue};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_stage_failure {foreach($stage in 'clone','bootstrap','install','configure','build','test'){$r=Copy-Case "stage-$stage";Write-Pin $r;Write-Fake-Tools $r;$s=Invoke-Wrapper $r @{FAKE_FAIL_STAGE=$stage};$log=Get-Content (Join-Path $r 'fake-native.log') -Raw;if($s-eq0){Fail "stage $stage passed"};if($stage-eq'clone' -and $log-match'bootstrap-vcpkg'){Fail 'clone reached bootstrap'};if($stage-eq'bootstrap' -and $log-match'vcpkg '){Fail 'bootstrap reached install'};if($stage-eq'install' -and $log-match'cmake --preset'){Fail 'install reached configure'};if($stage-eq'configure' -and $log-match'cmake --build'){Fail 'configure reached build'};if($stage-eq'build' -and $log-match'ctest '){Fail 'build reached test'}};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_paths_with_spaces {$r=Copy-Case paths-with-spaces;Write-Pin $r;Write-Fake-Tools $r;if((Invoke-Wrapper $r)-ne0){Fail "paths with spaces failed: $script:WrapperOutput"};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_cold_run {$r=Copy-Case cold-run;Write-Pin $r;Write-Fake-Tools $r;if((Invoke-Wrapper $r)-ne0){Fail 'cold run failed'};$l=Get-Content (Join-Path $r 'fake-native.log') -Raw;foreach($x in 'git .*clone','bootstrap-vcpkg','vcpkg install','cmake --preset','cmake --build','ctest'){if($l-notmatch$x){Fail "cold run missed $x"}};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_warm_run_non_destructive {$r=Copy-Case warm-run;Write-Pin $r;Write-Fake-Tools $r;$sentinel=Join-Path $r warm-sentinel;Set-Content $sentinel keep;if((Invoke-Wrapper $r)-ne0){Fail 'warm setup failed'};$before=Get-Content (Join-Path $r 'fake-native.log') -Raw;if((Invoke-Wrapper $r)-ne0){Fail 'warm rerun failed'};$after=Get-Content (Join-Path $r 'fake-native.log') -Raw;if(([regex]::Matches($after,'git .*clone')).Count-ne([regex]::Matches($before,'git .*clone')).Count){Fail 'warm cloned again'};foreach($x in 'vcpkg install','cmake --preset','cmake --build','ctest '){if(([regex]::Matches($after,[regex]::Escape($x))).Count-ne([regex]::Matches($before,[regex]::Escape($x))).Count+1){Fail "warm omitted $x"}};if(([regex]::Matches($after,'bootstrap-vcpkg')).Count-ne([regex]::Matches($before,'bootstrap-vcpkg')).Count){Fail 'warm bootstrapped again'};if(-not(Test-Path $sentinel)){Fail 'warm run removed sentinel'};if($after-match' fetch | reset | clean | checkout --force|Remove-Item'){Fail 'warm run invoked destructive operation'};$env:VCPKG_ROOT='developer-shell-vcpkg';$env:VCPKG_DOWNLOADS='sentinel-downloads';$env:VCPKG_DISABLE_METRICS='sentinel-metrics';$installCount=([regex]::Matches($after,'vcpkg install')).Count;$s=Invoke-Wrapper $r @{FAKE_FAIL_STAGE='install'} $null $true;if($s-eq0){Fail 'same-process cleanup failure case unexpectedly passed'};if($script:WrapperOutput-notmatch'manifest provisioning' -or $script:WrapperOutput-notmatch'native exit code 97'){Fail "pre-set VCPKG_ROOT did not reach the intended install failure: $script:WrapperOutput"};$withFailure=Get-Content (Join-Path $r 'fake-native.log') -Raw;if(([regex]::Matches($withFailure,'vcpkg install')).Count-ne$installCount+1){Fail 'pre-set VCPKG_ROOT did not reach manifest provisioning'};if($env:VCPKG_ROOT-ne'developer-shell-vcpkg'-or$env:VCPKG_DOWNLOADS-ne'sentinel-downloads'-or$env:VCPKG_DISABLE_METRICS-ne'sentinel-metrics'){Fail 'same-process wrapper environment was not restored'};Remove-Item Env:VCPKG_ROOT,Env:VCPKG_DOWNLOADS,Env:VCPKG_DISABLE_METRICS -ErrorAction SilentlyContinue;Pass $MyInvocation.MyCommand.Name}
  function bootstrap_forbidden_environment {$r=Copy-Case forbidden-environment;Write-Pin $r;Write-Fake-Tools $r;$s=Invoke-Wrapper $r @{VCPKG_DEFAULT_TRIPLET='bad'};if($s-eq0 -or $script:WrapperOutput-notmatch'environment|selection'){Fail 'forbidden environment was accepted'};$l=Get-Content (Join-Path $r 'fake-native.log') -Raw;if($l-match'git .*clone|bootstrap-vcpkg|vcpkg install|cmake --preset|cmake --build|ctest '){Fail 'forbidden environment performed checkout or build work'};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_cache_mismatch {$r=Copy-Case cache-mismatch;Write-Pin $r;Write-Fake-Tools $r;if((Invoke-Wrapper $r)-ne0){Fail 'cache setup failed'};Set-Content (Join-Path $r 'fake-native.log') '';Set-Content (Join-Path $r 'build/dev-windows/CMakeCache.txt') 'CMAKE_C_COMPILER:FILEPATH=C:/wrong/compiler';$s=Invoke-Wrapper $r;if($s-eq0 -or $script:WrapperOutput-notmatch'cache|identity|mismatch'){Fail 'cache mismatch was accepted'};if((Get-Content (Join-Path $r 'fake-native.log') -Raw)-match'cmake --preset'){Fail 'cache mismatch reached configure'};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_invalid_existing_executable {$r=Copy-Case invalid-existing-executable;Write-Pin $r;Write-Fake-Tools $r;$c=Join-Path $r '.cache/vcpkg/x64-windows';New-Item -ItemType Directory (Join-Path $c 'scripts/buildsystems') -Force|Out-Null;New-Item -ItemType Directory (Join-Path $c '.git') -Force|Out-Null;Set-Content (Join-Path $c 'scripts/buildsystems/vcpkg.cmake') '';Set-Content (Join-Path $c 'vcpkg.exe') 'not executable';Expect-Reject $MyInvocation.MyCommand.Name $r;$r=Copy-Case zero-output-existing-executable;Write-Pin $r;Write-Fake-Tools $r;$c=Join-Path $r '.cache/vcpkg/x64-windows';New-Item -ItemType Directory (Join-Path $c 'scripts/buildsystems') -Force|Out-Null;New-Item -ItemType Directory (Join-Path $c '.git') -Force|Out-Null;Set-Content (Join-Path $c 'scripts/buildsystems/vcpkg.cmake') ''; $source=Join-Path $c 'zero.c';$zeroExe=Join-Path $c 'vcpkg.exe';Set-Content $source 'int main(void){return 0;}';& $RealCl /nologo "/Fe:$zeroExe" $source | Out-Null;if($LASTEXITCODE-ne0){Fail 'could not build zero-output executable fixture'};Expect-Reject 'bootstrap_zero_output_existing_executable' $r;Pass $MyInvocation.MyCommand.Name}
  function bootstrap_malformed_pin {$r=Copy-Case malformed-pin;Write-Fake-Tools $r;[IO.File]::WriteAllBytes((Join-Path $r 'tools/vcpkg-tool-commit.txt'),[Text.Encoding]::ASCII.GetBytes("$Pin`0`n"));Expect-Reject $MyInvocation.MyCommand.Name $r;$l=Get-Content (Join-Path $r 'fake-native.log') -Raw;if($l-match'git .*clone|bootstrap-vcpkg|vcpkg install|cmake --preset|cmake --build|ctest '){Fail 'malformed pin performed checkout or build work'};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_lock_contention {$r=Copy-Case lock-contention;Write-Pin $r;Write-Fake-Tools $r;$d=Join-Path $r 'build/bootstrap-locks';New-Item -ItemType Directory $d -Force|Out-Null;Set-Content (Join-Path $d 'x64-windows.lock') x;Expect-Reject $MyInvocation.MyCommand.Name $r;if(-not(Test-Path (Join-Path $d 'x64-windows.lock'))){Fail 'lock removed'};Pass $MyInvocation.MyCommand.Name}
  foreach($check in $CheckNames){& $check};Write-Output "All bootstrap.ps1 contract checks passed ($($CheckNames.Count+1) checks)."
} finally {
  try {
    if (Test-Path $TempRoot) {
      Set-Location -LiteralPath $TestRoot
      [GC]::Collect()
      [GC]::WaitForPendingFinalizers()
      Remove-Item -LiteralPath $TempRoot -Recurse -Force
    }
  } finally {
    foreach ($name in $CallerOwnedEnvironment.Keys) {
      if ($null -eq $CallerOwnedEnvironment[$name]) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
      else { Set-Item "Env:$name" $CallerOwnedEnvironment[$name] }
    }
  }
}
