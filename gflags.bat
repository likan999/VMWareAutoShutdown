FOR %%I IN (Win32 x64) DO call :cmake %%I

GOTO :EOF
:cmake
SETLOCAL
MKDIR externals\out\gflags\%1
MKDIR externals\build\gflags\%1
PUSHD externals\build\gflags\%1
SET generator=Visual Studio 14 2015
IF %1 == x64 (
  SET generator=%generator% Win64
)
cmake -G "%generator%" ../../../../gflags ^
  -DCMAKE_INSTALL_PREFIX=../../../out/gflags/%1 ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DGFLAGS_BUILD_SHARED_LIBS=ON ^
  -DGFLAGS_BUILD_STATIC_LIBS=ON ^
  -DGFLAGS_REGISTER_BUILD_DIR=OFF ^
  -DGFLAGS_REGISTER_INSTALL_PREFIX=OFF
POPD
ENDLOCAL
GOTO :EOF