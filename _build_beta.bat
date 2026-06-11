@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cmake --build "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo" --target nhllegacy
echo BUILD_EXIT=%ERRORLEVEL%
