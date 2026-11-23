@echo off
chcp 65001 >nul
setlocal
set PYTHONUTF8=1

rem 本脚本有两个位置：
rem   ui-skin\tools\pack_skin.bat      -> 打包器在 ..\scripts
rem   <交付包>\一键打包.bat            -> 打包器在 .\scripts
set "ROOT=%~dp0"
if exist "%ROOT%scripts\pack_style.py" goto :found
set "ROOT=%~dp0.."
:found
cd /d "%ROOT%"

echo ================================================
echo   UI 素材  -^>  2MB Flash 镜像
echo ================================================
echo.

set "SRCDIR=%~1"
if not defined SRCDIR goto :ask
goto :run

:ask
echo 把「素材文件夹」拖到本窗口，再按回车（也可以直接粘贴完整路径）：
echo.
set /p SRCDIR=
echo.

:run
set "SRCDIR=%SRCDIR:"=%"
if not exist "%SRCDIR%" (
  echo 找不到素材目录：%SRCDIR%
  echo.
  pause
  exit /b 1
)

python scripts\pack_style.py "%SRCDIR%"
if errorlevel 1 (
  echo.
  echo !! 打包失败，看上面的报错 !!
  pause
  exit /b 1
)
pause
exit /b 0
