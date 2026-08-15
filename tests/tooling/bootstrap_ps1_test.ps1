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
foreach ($name in @('VCPKG_ROOT','VCPKG_DOWNLOADS','VCPKG_DISABLE_METRICS','GIT_TERMINAL_PROMPT','GCM_INTERACTIVE')) {
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
      'if not "%GIT_TERMINAL_PROMPT%"=="0" exit /b 96',
      'if not "%GCM_INTERACTIVE%"=="never" exit /b 96',
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
    @('@echo off','echo cmake %*>>"%FAKE_LOG%"','if "%1"=="--version" echo cmake version %FAKE_CMAKE_VERSION%','if "%FAKE_FAIL_STAGE%"=="configure" if "%1"=="--preset" exit /b 97','if "%FAKE_FAIL_STAGE%"=="build" if "%1"=="--build" exit /b 97','if not "%1"=="--preset" exit /b 0','mkdir "%FAKE_BUILD_TREE%" >nul 2>nul','>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo CMAKE_C_COMPILER:FILEPATH=%FAKE_CMAKE_COMPILER%','>>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo CMAKE_CXX_COMPILER:FILEPATH=%FAKE_CMAKE_COMPILER%','>>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo CMAKE_TOOLCHAIN_FILE:FILEPATH=%FAKE_TOOLCHAIN%','>>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo VCPKG_TARGET_TRIPLET:STRING=x64-windows','>>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo VCPKG_INSTALLED_DIR:PATH=%FAKE_INSTALLED%','>>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo VCPKG_MANIFEST_INSTALL:BOOL=OFF','>>"%FAKE_BUILD_TREE%\CMakeCache.txt" echo VCPKG_APPLOCAL_DEPS:BOOL=OFF','exit /b 0') | Set-Content (Join-Path $bin 'cmake.cmd')
    @('@echo off','echo ninja %*>>"%FAKE_LOG%"','if "%1"=="--version" echo 1.11.1','exit /b 0') | Set-Content (Join-Path $bin 'ninja.cmd')
    @('@echo off','echo ctest %*>>"%FAKE_LOG%"','if "%1"=="--version" echo ctest version 3.28.1','if "%FAKE_FAIL_STAGE%"=="test" if "%1"=="--preset" exit /b 97','exit /b 0') | Set-Content (Join-Path $bin 'ctest.cmd')
  }
  function Invoke-Wrapper([string]$Repo,[hashtable]$ExtraEnv=@{},[string]$PathOverride=$null,[bool]$InProcess=$false,[bool]$ShadowCommands=$false) {
    $shadowOriginal=@{}
    $save = @{}; foreach ($n in @('Path','FAKE_LOG','FAKE_FAIL_STAGE','FAKE_GIT_ORIGIN','FAKE_GIT_REV','FAKE_GIT_STATUS','VSCMD_ARG_TGT_ARCH','FAKE_BUILD_TREE','FAKE_CMAKE_COMPILER','FAKE_CMAKE_VERSION','FAKE_TOOLCHAIN','FAKE_INSTALLED','VCPKG_DEFAULT_TRIPLET','VCPKG_ROOT','VCPKG_DOWNLOADS','VCPKG_DISABLE_METRICS','GIT_TERMINAL_PROMPT','GCM_INTERACTIVE')) { $save[$n] = [Environment]::GetEnvironmentVariable($n) }
    try {
      $env:VSCMD_ARG_TGT_ARCH='x64'; $env:Path=if([string]::IsNullOrEmpty($PathOverride)){"$(Join-Path $Repo 'fake-native-bin');$($save['Path'])"}else{$PathOverride}; $env:FAKE_LOG=Join-Path $Repo 'fake-native.log'; $env:FAKE_CMAKE_VERSION='3.28.1'
      if (-not $InProcess) { Remove-Item Env:VCPKG_ROOT,Env:VCPKG_DOWNLOADS,Env:VCPKG_DISABLE_METRICS -ErrorAction SilentlyContinue }
      $env:FAKE_BUILD_TREE=Join-Path $Repo 'build/dev-windows'; $env:FAKE_CMAKE_COMPILER=Join-Path $Repo 'fake-native-bin/cl.cmd'; $env:FAKE_TOOLCHAIN=Join-Path $Repo '.cache/vcpkg/x64-windows/scripts/buildsystems/vcpkg.cmake'; $env:FAKE_INSTALLED=Join-Path $Repo 'build/vcpkg-installed/x64-windows'
      foreach($k in $ExtraEnv.Keys){Set-Item "Env:$k" ([string]$ExtraEnv[$k])}
      if ($InProcess) {
        if ($ShadowCommands) {
          foreach ($tool in @('git','cmake','ninja','cl','ctest')) {
            $existing=Get-Item -Path "Function:global:$tool" -ErrorAction SilentlyContinue
            $shadowOriginal[$tool]=if($null-eq$existing){$null}else{$existing.ScriptBlock}
            Set-Item -Path "Function:global:$tool" -Value ([scriptblock]::Create("throw 'shadow command invoked: $tool'"))
          }
        }
        $status = 0
        try { $script:WrapperOutput=@(& (Join-Path $Repo 'bootstrap.ps1') 2>&1|Out-String) }
        catch { $script:WrapperOutput = $_ | Out-String; $status = 1 }
        finally {
          $script:InProcessEnvironment = @{}
          foreach ($name in @('VCPKG_ROOT','VCPKG_DOWNLOADS','VCPKG_DISABLE_METRICS','GIT_TERMINAL_PROMPT','GCM_INTERACTIVE')) { $script:InProcessEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }
          $script:InProcessLocation = (Get-Location).Path
        }
        return $status
      }
      $script:WrapperOutput=@(& $PowerShellExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'bootstrap.ps1') 2>&1|Out-String); return [int]$LASTEXITCODE
    } finally { foreach($tool in $shadowOriginal.Keys){if($null-eq$shadowOriginal[$tool]){Remove-Item -Path "Function:global:$tool" -ErrorAction SilentlyContinue}else{Set-Item -Path "Function:global:$tool" -Value $shadowOriginal[$tool]}}; foreach($n in $save.Keys){if($null -eq $save[$n]){Remove-Item "Env:$n" -ErrorAction SilentlyContinue}else{Set-Item "Env:$n" $save[$n]}} }
  }
  function Expect-Reject([string]$Name,[string]$Repo,[string]$Diagnostic,[hashtable]$Env=@{}) { $status=Invoke-Wrapper $Repo $Env; if($status -eq 0){Fail "$Name unexpectedly passed"}; if($script:WrapperOutput -notmatch $Diagnostic){Fail "$Name did not report its case-specific diagnostic ($Diagnostic): $script:WrapperOutput"} }
  $canonical=Join-Path $TestRoot 'bootstrap.ps1'; if(-not(Test-Path $canonical)){Fail "bootstrap_missing_canonical_wrapper: canonical wrapper absent"}; Pass bootstrap_missing_canonical_wrapper
  if($IsWindows -ne $true){Write-Output 'SKIP: PowerShell contract matrix requires native Windows; canonical wrapper preflight passed.'; return}
  function bootstrap_missing_host_tool {
    foreach ($tool in 'cmake','ctest') {
      $r=Copy-Case "missing-host-tool-$tool";Write-Fake-Tools $r
      Remove-Item -LiteralPath (Join-Path $r "fake-native-bin/$tool.cmd") -Force
      $s=Invoke-Wrapper $r @{} (Join-Path $r 'fake-native-bin')
      if($s-eq0 -or $script:WrapperOutput-notmatch("required host tool is missing: $tool")){Fail "missing $tool failed to reject at host validation: $script:WrapperOutput"}
      $l=Get-Content (Join-Path $r 'fake-native.log') -Raw
      if($l-match'git .*clone|bootstrap-vcpkg|vcpkg install|cmake --preset|cmake --build|ctest --preset'){Fail "missing $tool began a later stage"}
    }
    Pass $MyInvocation.MyCommand.Name
  }
  function bootstrap_invalid_pin {
    $r=Copy-Case invalid-pin;Write-Pin $r invalid;Write-Fake-Tools $r;Expect-Reject $MyInvocation.MyCommand.Name $r 'tool pin must be exactly';$l=Get-Content (Join-Path $r 'fake-native.log') -Raw;if($l-match'git .*clone|bootstrap-vcpkg|vcpkg install'){Fail 'invalid pin performed checkout or provisioning work'}
    $r=Copy-Case invalid-pin-unsupported-cmake;Write-Pin $r invalid;Write-Fake-Tools $r
    $before=[Environment]::GetEnvironmentVariable('FAKE_CMAKE_VERSION')
    $s=Invoke-Wrapper $r @{FAKE_CMAKE_VERSION='3.27.9'}
    if($s-eq0 -or $script:WrapperOutput-notmatch'CMake 3.28\+ is required'){Fail "unsupported CMake did not reject before pin validation: $script:WrapperOutput"}
    if($script:WrapperOutput-match'INFO: stage: pin validation'){Fail 'unsupported CMake reached pin validation'}
    $l=Get-Content (Join-Path $r 'fake-native.log') -Raw;if($l-match'git .*clone|bootstrap-vcpkg|vcpkg install|cmake --preset|cmake --build|ctest --preset'){Fail 'unsupported CMake began a later stage'}
    if([Environment]::GetEnvironmentVariable('FAKE_CMAKE_VERSION') -cne $before){Fail 'fake CMake version override was not restored'}
    Pass $MyInvocation.MyCommand.Name
  }
  function bootstrap_untrusted_checkout {foreach($v in 'wrong-origin','wrong-revision','tracked-dirtiness','unmanaged-path','gitfile','ancestor-redirection'){$r=Copy-Case "untrusted-$v";Write-Fake-Tools $r;$c=Join-Path $r '.cache/vcpkg/x64-windows';New-Item -ItemType Directory (Join-Path $c '.git') -Force|Out-Null;if($v-eq'wrong-origin'){$env:FAKE_GIT_ORIGIN='https://example.invalid/untrusted.git'}elseif($v-eq'wrong-revision'){$env:FAKE_GIT_REV='0'*40}elseif($v-eq'tracked-dirtiness'){$env:FAKE_GIT_STATUS=' M ports/example'}elseif($v-eq'gitfile'){Remove-Item (Join-Path $c '.git') -Recurse -Force;Set-Content (Join-Path $c '.git') 'gitdir: ..\outside-git-dir'}elseif($v-eq'ancestor-redirection'){$outside=Join-Path $r '..\outside-ancestor';New-Item -ItemType Directory $outside -Force|Out-Null;Remove-Item (Join-Path $r '.cache/vcpkg') -Recurse -Force;New-Item -ItemType Junction -Path (Join-Path $r '.cache/vcpkg') -Target $outside|Out-Null}else{Remove-Item (Join-Path $c '.git') -Recurse -Force;Set-Content (Join-Path $c unmanaged) x};$diagnostic=@{'wrong-origin'='checkout origin is not the official vcpkg origin';'wrong-revision'='checkout HEAD is not the repository-owned pin';'tracked-dirtiness'='checkout has tracked/index dirtiness';'unmanaged-path'='existing checkout is not a managed Git checkout';'gitfile'='existing checkout is not a managed Git checkout';'ancestor-redirection'='managed path has a symlink/reparse ancestor'}[$v];Expect-Reject "$($MyInvocation.MyCommand.Name)/$v" $r $diagnostic;Remove-Item Env:FAKE_GIT_ORIGIN,Env:FAKE_GIT_REV,Env:FAKE_GIT_STATUS -ErrorAction SilentlyContinue};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_stage_failure {$diagnostics=@{'clone'='official clone failed with native exit code 97';'bootstrap'='vcpkg bootstrap failed with native exit code 97';'install'='manifest provisioning failed with native exit code 97';'configure'='configure failed with native exit code 97';'build'='build failed with native exit code 97';'test'='CTest failed with native exit code 97'};foreach($stage in 'clone','bootstrap','install','configure','build','test'){$r=Copy-Case "stage-$stage";Write-Fake-Tools $r;$s=Invoke-Wrapper $r @{FAKE_FAIL_STAGE=$stage};$log=Get-Content (Join-Path $r 'fake-native.log') -Raw;if($s-eq0){Fail "stage $stage passed"};if($script:WrapperOutput-notmatch [regex]::Escape($diagnostics[$stage])){Fail "stage $stage did not report its case-specific failure: $script:WrapperOutput"};if($stage-eq'clone' -and $log-match'bootstrap-vcpkg'){Fail 'clone reached bootstrap'};if($stage-eq'bootstrap' -and $log-match'vcpkg '){Fail 'bootstrap reached install'};if($stage-eq'install' -and $log-match'cmake --preset'){Fail 'install reached configure'};if($stage-eq'configure' -and $log-match'cmake --build'){Fail 'configure reached build'};if($stage-eq'build' -and $log-match'ctest --preset'){Fail 'build reached test'}};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_paths_with_spaces {$r=Copy-Case paths-with-spaces;Write-Fake-Tools $r;if((Invoke-Wrapper $r)-ne0){Fail "paths with spaces failed: $script:WrapperOutput"};if((Invoke-Wrapper $r @{} $null $true $true)-ne0){Fail "shadowed native commands were invoked instead of resolved Application paths: $script:WrapperOutput"};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_cold_run {$r=Copy-Case cold-run;Write-Fake-Tools $r;if((Invoke-Wrapper $r)-ne0){Fail 'cold run failed'};$l=Get-Content (Join-Path $r 'fake-native.log') -Raw;foreach($x in 'git .*clone','bootstrap-vcpkg','vcpkg install','cmake --preset','cmake --build','ctest --preset'){if($l-notmatch$x){Fail "cold run missed $x"}};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_warm_run_non_destructive {$r=Copy-Case warm-run;Write-Fake-Tools $r;$sentinel=Join-Path $r warm-sentinel;Set-Content $sentinel keep;if((Invoke-Wrapper $r)-ne0){Fail 'warm setup failed'};$before=Get-Content (Join-Path $r 'fake-native.log') -Raw;if((Invoke-Wrapper $r)-ne0){Fail 'warm rerun failed'};$after=Get-Content (Join-Path $r 'fake-native.log') -Raw;if(([regex]::Matches($after,'git .*clone')).Count-ne([regex]::Matches($before,'git .*clone')).Count){Fail 'warm cloned again'};foreach($x in 'vcpkg install','cmake --preset','cmake --build','ctest --preset'){if(([regex]::Matches($after,[regex]::Escape($x))).Count-ne([regex]::Matches($before,[regex]::Escape($x))).Count+1){Fail "warm omitted $x"}};if(([regex]::Matches($after,'bootstrap-vcpkg')).Count-ne([regex]::Matches($before,'bootstrap-vcpkg')).Count){Fail 'warm bootstrapped again'};if(-not(Test-Path $sentinel)){Fail 'warm run removed sentinel'};if($after-match' fetch | reset | clean | checkout --force|Remove-Item'){Fail 'warm run invoked destructive operation'};$env:VCPKG_ROOT='developer-shell-vcpkg';$env:VCPKG_DOWNLOADS='sentinel-downloads';$env:VCPKG_DISABLE_METRICS='sentinel-metrics';$env:GIT_TERMINAL_PROMPT='caller-prompt';$env:GCM_INTERACTIVE='caller-interactive';$expectedLocation=(Get-Location).Path;$s=Invoke-Wrapper $r @{} $null $true;if($s-ne0){Fail "same-process success case failed: $script:WrapperOutput"};if($script:InProcessLocation-ne$expectedLocation){Fail 'same-process success changed caller location'};foreach($pair in @{VCPKG_ROOT='developer-shell-vcpkg';VCPKG_DOWNLOADS='sentinel-downloads';VCPKG_DISABLE_METRICS='sentinel-metrics';GIT_TERMINAL_PROMPT='caller-prompt';GCM_INTERACTIVE='caller-interactive'}.GetEnumerator()){if($script:InProcessEnvironment[$pair.Key]-cne$pair.Value){Fail "same-process success did not restore $($pair.Key)"}};$afterSuccess=Get-Content (Join-Path $r 'fake-native.log') -Raw;$installCount=([regex]::Matches($afterSuccess,'vcpkg install')).Count;$s=Invoke-Wrapper $r @{FAKE_FAIL_STAGE='install'} $null $true;if($s-eq0){Fail 'same-process cleanup failure case unexpectedly passed'};if($script:WrapperOutput-notmatch'manifest provisioning' -or $script:WrapperOutput-notmatch'native exit code 97'){Fail "pre-set VCPKG_ROOT did not reach the intended install failure: $script:WrapperOutput"};$withFailure=Get-Content (Join-Path $r 'fake-native.log') -Raw;if(([regex]::Matches($withFailure,'vcpkg install')).Count-ne$installCount+1){Fail 'pre-set VCPKG_ROOT did not reach manifest provisioning'};if($script:InProcessLocation-ne$expectedLocation){Fail 'same-process failure changed caller location'};foreach($pair in @{VCPKG_ROOT='developer-shell-vcpkg';VCPKG_DOWNLOADS='sentinel-downloads';VCPKG_DISABLE_METRICS='sentinel-metrics';GIT_TERMINAL_PROMPT='caller-prompt';GCM_INTERACTIVE='caller-interactive'}.GetEnumerator()){if($script:InProcessEnvironment[$pair.Key]-cne$pair.Value){Fail "same-process failure did not restore $($pair.Key)"}};Remove-Item Env:VCPKG_ROOT,Env:VCPKG_DOWNLOADS,Env:VCPKG_DISABLE_METRICS,Env:GIT_TERMINAL_PROMPT,Env:GCM_INTERACTIVE -ErrorAction SilentlyContinue;Pass $MyInvocation.MyCommand.Name}
  function bootstrap_forbidden_environment {$r=Copy-Case forbidden-environment;Write-Fake-Tools $r;$s=Invoke-Wrapper $r @{VCPKG_DEFAULT_TRIPLET='bad'};if($s-eq0 -or $script:WrapperOutput-notmatch'selection environment VCPKG_DEFAULT_TRIPLET is set'){Fail 'forbidden environment was accepted'};$l=Get-Content (Join-Path $r 'fake-native.log') -Raw;if($l-match'git .*clone|bootstrap-vcpkg|vcpkg install|cmake --preset|cmake --build|ctest --preset'){Fail 'forbidden environment performed checkout or build work'};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_cache_mismatch {$r=Copy-Case cache-mismatch;Write-Fake-Tools $r;if((Invoke-Wrapper $r)-ne0){Fail 'cache setup failed'};Set-Content (Join-Path $r 'fake-native.log') '';Set-Content (Join-Path $r 'build/dev-windows/CMakeCache.txt') 'CMAKE_C_COMPILER:FILEPATH=C:/wrong/compiler';$s=Invoke-Wrapper $r;if($s-eq0 -or $script:WrapperOutput-notmatch'CMake cache identity mismatch'){Fail 'cache mismatch was accepted'};if((Get-Content (Join-Path $r 'fake-native.log') -Raw)-match'cmake --preset'){Fail 'cache mismatch reached configure'};$r=Copy-Case cache-redirection;Write-Fake-Tools $r;$cache=Join-Path $r 'build/dev-windows/CMakeCache.txt';$outside=Join-Path $r '..\outside-cache';New-Item -ItemType Directory (Split-Path $cache) -Force|Out-Null;New-Item -ItemType Directory $outside -Force|Out-Null;Set-Content (Join-Path $outside sentinel) keep;New-Item -ItemType Junction -Path $cache -Target $outside|Out-Null;$s=Invoke-Wrapper $r;if($s-eq0-or$script:WrapperOutput-notmatch'managed path has a symlink/reparse ancestor'){Fail 'cache redirection was accepted'};if(-not(Test-Path (Join-Path $outside sentinel))){Fail 'cache redirection target was mutated'};if((Get-Content (Join-Path $r 'fake-native.log') -Raw)-match'cmake --preset'){Fail 'cache redirection reached configure'};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_invalid_existing_executable {$r=Copy-Case invalid-existing-executable;Write-Fake-Tools $r;$c=Join-Path $r '.cache/vcpkg/x64-windows';New-Item -ItemType Directory (Join-Path $c 'scripts/buildsystems') -Force|Out-Null;New-Item -ItemType Directory (Join-Path $c '.git') -Force|Out-Null;Set-Content (Join-Path $c 'scripts/buildsystems/vcpkg.cmake') '';$source=Join-Path $c 'failure.c';$failureObj=Join-Path $c 'failure.obj';$failureExe=Join-Path $c 'vcpkg.exe';Set-Content $source 'int main(void){return 97;}';& $RealCl /nologo "/Fo:$failureObj" "/Fe:$failureExe" $source | Out-Null;if($LASTEXITCODE-ne0){Fail 'could not build failing executable fixture'};Expect-Reject $MyInvocation.MyCommand.Name $r 'vcpkg executable could not report its version';$r=Copy-Case zero-output-existing-executable;Write-Fake-Tools $r;$c=Join-Path $r '.cache/vcpkg/x64-windows';New-Item -ItemType Directory (Join-Path $c 'scripts/buildsystems') -Force|Out-Null;New-Item -ItemType Directory (Join-Path $c '.git') -Force|Out-Null;Set-Content (Join-Path $c 'scripts/buildsystems/vcpkg.cmake') ''; $source=Join-Path $c 'zero.c';$zeroObj=Join-Path $c 'zero.obj';$zeroExe=Join-Path $c 'vcpkg.exe';Set-Content $source 'int main(void){return 0;}';& $RealCl /nologo "/Fo:$zeroObj" "/Fe:$zeroExe" $source | Out-Null;if($LASTEXITCODE-ne0){Fail 'could not build zero-output executable fixture'};Expect-Reject 'bootstrap_zero_output_existing_executable' $r 'vcpkg executable returned no recognizable version';Pass $MyInvocation.MyCommand.Name}
  function bootstrap_malformed_pin {$r=Copy-Case malformed-pin;Write-Fake-Tools $r;[IO.File]::WriteAllBytes((Join-Path $r 'tools/vcpkg-tool-commit.txt'),[Text.Encoding]::ASCII.GetBytes("$Pin`0`n"));Expect-Reject $MyInvocation.MyCommand.Name $r 'tool pin must be exactly';$l=Get-Content (Join-Path $r 'fake-native.log') -Raw;if($l-match'git .*clone|bootstrap-vcpkg|vcpkg install|cmake --preset|cmake --build|ctest --preset'){Fail 'malformed pin performed checkout or build work'};Pass $MyInvocation.MyCommand.Name}
  function bootstrap_lock_contention {$r=Copy-Case lock-contention;Write-Fake-Tools $r;$d=Join-Path $r 'build/bootstrap-locks';New-Item -ItemType Directory $d -Force|Out-Null;Set-Content (Join-Path $d 'x64-windows.lock') x;Expect-Reject $MyInvocation.MyCommand.Name $r 'exclusive lock exists';if(-not(Test-Path (Join-Path $d 'x64-windows.lock'))){Fail 'lock removed'};Pass $MyInvocation.MyCommand.Name}
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
