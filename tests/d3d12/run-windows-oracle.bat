@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "ORACLE_D3D9_EXE=%~dp0dxmt-wine-d3d9-tests.exe"
set "ORACLE_D3D10_EXE=%~dp0dxmt-wine-d3d10-tests.exe"
set "ORACLE_D3D11_EXE=%~dp0dxmt-wine-d3d11-tests.exe"
set "ORACLE_D3D12_EXE=%~dp0dxmt-wine-d3d12-tests.exe"
set "ORACLE_SUITE_SCHEMA=public-api-v1"
set "ORACLE_OUTPUT=%~dp0windows-oracle-results"
set "ORACLE_ADAPTER=warp"
set "ORACLE_DEBUG=0"
set "ORACLE_D3D12_ONLY=0"
set "ORACLE_CASES=*"
set "ORACLE_WORKER_ARG="
set "ORACLE_D3D12_JOBS=4"

if defined DXMT_CI_D3D12_CASE_FILTER (
  set "ORACLE_CASES=%DXMT_CI_D3D12_CASE_FILTER%"
  set "ORACLE_D3D12_ONLY=1"
)
if defined DXMT_CI_D3D12_JOBS set "ORACLE_D3D12_JOBS=%DXMT_CI_D3D12_JOBS%"
if /I "%DXMT_CI_D3D12_DEBUG%"=="1" (
  set "ORACLE_DEBUG=1"
  set "ORACLE_D3D12_ONLY=1"
)

:parse_args
if "%~1"=="" goto run_oracle
if /I "%~1"=="--debug" (
  set "ORACLE_DEBUG=1"
  set "ORACLE_D3D12_ONLY=1"
  shift
  goto parse_args
)
if /I "%~1"=="--hardware" (
  set "ORACLE_ADAPTER=default"
  shift
  goto parse_args
)
if /I "%~1"=="--warp" (
  set "ORACLE_ADAPTER=warp"
  shift
  goto parse_args
)
if /I "%~1"=="--case" (
  if "%~2"=="" (
    echo ERROR: --case requires a D3D12 CaseId.
    exit /b 2
  )
  set "ORACLE_CASES=%~2"
  set "ORACLE_D3D12_ONLY=1"
  shift
  shift
  goto parse_args
)
if /I "%~1"=="--output" (
  if "%~2"=="" (
    echo ERROR: --output requires a directory.
    exit /b 2
  )
  set "ORACLE_OUTPUT=%~2"
  shift
  shift
  goto parse_args
)
if /I "%~1"=="--help" goto usage
if /I "%~1"=="/?" goto usage
echo ERROR: Unknown argument: %~1
goto usage_error

:run_oracle
for %%F in ("%ORACLE_D3D9_EXE%" "%ORACLE_D3D10_EXE%" "%ORACLE_D3D11_EXE%" "%ORACLE_D3D12_EXE%") do (
  if not exist "%%~F" (
    echo ERROR: Oracle executable was not found:
    echo   %%~F
    exit /b 2
  )
)

if not exist "%ORACLE_OUTPUT%" (
  mkdir "%ORACLE_OUTPUT%"
  if errorlevel 1 (
    echo ERROR: Failed to create output directory:
    echo   %ORACLE_OUTPUT%
    exit /b 2
  )
)
del /q "%ORACLE_OUTPUT%\d3d9-oracle-*.log" >nul 2>&1
del /q "%ORACLE_OUTPUT%\d3d10-oracle-*.log" >nul 2>&1
del /q "%ORACLE_OUTPUT%\d3d11-oracle-*.log" >nul 2>&1
del /q "%ORACLE_OUTPUT%\d3d12-oracle-*.log" >nul 2>&1
del /q "%ORACLE_OUTPUT%\windows-oracle-*.txt" >nul 2>&1

if /I "%ORACLE_ADAPTER%"=="warp" (
  set "DXMT_TEST_WINDOWS_ADAPTER=warp"
) else (
  set "DXMT_TEST_WINDOWS_ADAPTER="
)

if "%ORACLE_DEBUG%"=="1" (
  set "DXMT_TEST_WINDOWS_DEBUG_LAYER=1"
  set "ORACLE_MODE=%ORACLE_ADAPTER%-debug"
  set "ORACLE_WORKER_ARG=--dxmt-test-worker"
) else (
  set "DXMT_TEST_WINDOWS_DEBUG_LAYER="
  set "ORACLE_MODE=%ORACLE_ADAPTER%"
)

set "ORACLE_STAMP=%RANDOM%-%RANDOM%"
set "ORACLE_D3D9_LOG=%ORACLE_OUTPUT%\d3d9-oracle-%ORACLE_MODE%-%ORACLE_STAMP%.log"
set "ORACLE_D3D10_LOG=%ORACLE_OUTPUT%\d3d10-oracle-%ORACLE_MODE%-%ORACLE_STAMP%.log"
set "ORACLE_D3D11_LOG=%ORACLE_OUTPUT%\d3d11-oracle-%ORACLE_MODE%-%ORACLE_STAMP%.log"
set "ORACLE_D3D12_LOG=%ORACLE_OUTPUT%\d3d12-oracle-%ORACLE_MODE%-%ORACLE_STAMP%.log"
set "ORACLE_METADATA=%ORACLE_OUTPUT%\windows-oracle-%ORACLE_MODE%-%ORACLE_STAMP%.txt"
set "ORACLE_EXIT=0"
set "ORACLE_D3D9_EXIT=not-run"
set "ORACLE_D3D10_EXIT=not-run"
set "ORACLE_D3D11_EXIT=not-run"
set "ORACLE_D3D12_EXIT=not-run"

echo DXMT Windows behavior oracle
echo   Suite schema: %ORACLE_SUITE_SCHEMA%
echo   D3D9/10/11: default Windows hardware adapter
echo   D3D12:      %ORACLE_ADAPTER%
echo   Debug layer: %ORACLE_DEBUG%
echo   Output:      %ORACLE_OUTPUT%
echo.

(
  echo timestamp=%DATE% %TIME%
  echo suite_schema=%ORACLE_SUITE_SCHEMA%
  echo computer=%COMPUTERNAME%
  echo d3d10_d3d11_adapter=default-hardware
  echo d3d12_adapter=%ORACLE_ADAPTER%
  echo d3d12_debug_layer=%ORACLE_DEBUG%
  echo d3d12_case_filter=%ORACLE_CASES%
  echo d3d12_jobs=%ORACLE_D3D12_JOBS%
  ver
  certutil -hashfile "%ORACLE_D3D9_EXE%" SHA256
  certutil -hashfile "%ORACLE_D3D10_EXE%" SHA256
  certutil -hashfile "%ORACLE_D3D11_EXE%" SHA256
  certutil -hashfile "%ORACLE_D3D12_EXE%" SHA256
) > "%ORACLE_METADATA%" 2>&1

if "%ORACLE_D3D12_ONLY%"=="1" goto run_d3d12

echo [ D3D9 ] Running complete suite...
"%ORACLE_D3D9_EXE%" --dxmt-test-jobs=1 > "%ORACLE_D3D9_LOG%" 2>&1
set "ORACLE_D3D9_EXIT=%ERRORLEVEL%"
type "%ORACLE_D3D9_LOG%"
if not "%ORACLE_D3D9_EXIT%"=="0" set "ORACLE_EXIT=1"
echo.

echo [ D3D10 ] Running complete suite...
"%ORACLE_D3D10_EXE%" --dxmt-test-jobs=1 > "%ORACLE_D3D10_LOG%" 2>&1
set "ORACLE_D3D10_EXIT=%ERRORLEVEL%"
type "%ORACLE_D3D10_LOG%"
if not "%ORACLE_D3D10_EXIT%"=="0" set "ORACLE_EXIT=1"
echo.

echo [ D3D11 ] Running complete suite...
"%ORACLE_D3D11_EXE%" --dxmt-test-jobs=1 > "%ORACLE_D3D11_LOG%" 2>&1
set "ORACLE_D3D11_EXIT=%ERRORLEVEL%"
type "%ORACLE_D3D11_LOG%"
if not "%ORACLE_D3D11_EXIT%"=="0" set "ORACLE_EXIT=1"
echo.

:run_d3d12
echo [ D3D12 ] Running complete suite...
"%ORACLE_D3D12_EXE%" %ORACLE_WORKER_ARG% --dxmt-test-jobs=%ORACLE_D3D12_JOBS% "--dxmt-case-id=%ORACLE_CASES%" > "%ORACLE_D3D12_LOG%" 2>&1
set "ORACLE_D3D12_EXIT=%ERRORLEVEL%"
type "%ORACLE_D3D12_LOG%"
if not "%ORACLE_D3D12_EXIT%"=="0" set "ORACLE_EXIT=1"

(
  echo d3d9_exit_code=%ORACLE_D3D9_EXIT%
  echo d3d10_exit_code=%ORACLE_D3D10_EXIT%
  echo d3d11_exit_code=%ORACLE_D3D11_EXIT%
  echo d3d12_exit_code=%ORACLE_D3D12_EXIT%
  echo overall_exit_code=%ORACLE_EXIT%
) >> "%ORACLE_METADATA%"

echo.
echo Overall exit code: %ORACLE_EXIT%
echo Metadata: %ORACLE_METADATA%
if not "%ORACLE_D3D9_EXIT%"=="not-run" echo D3D9 log: %ORACLE_D3D9_LOG%
if not "%ORACLE_D3D10_EXIT%"=="not-run" echo D3D10 log: %ORACLE_D3D10_LOG%
if not "%ORACLE_D3D11_EXIT%"=="not-run" echo D3D11 log: %ORACLE_D3D11_LOG%
echo D3D12 log: %ORACLE_D3D12_LOG%
echo A failing assertion is behavior evidence, not an automatic DXMT defect verdict.
exit /b %ORACLE_EXIT%

:usage
echo Usage: %~nx0 [--warp ^| --hardware] [--debug] [--case D3D12CaseId] [--output Directory]
echo.
echo   default      Run the complete D3D10, D3D11, and D3D12 suites.
echo   --warp       Run D3D12 on Microsoft WARP. This is the default.
echo   --hardware   Run D3D12 on the default hardware adapter.
echo   --debug      Run the complete D3D12 suite with the Debug Layer.
echo   --case       Run one D3D12 CaseId and skip D3D10/D3D11.
echo   --output     Change the result directory.
exit /b 0

:usage_error
call :usage
exit /b 2
