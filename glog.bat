FOR %%I IN (Win32 x64) DO call :cmake %%I

GOTO :EOF
:cmake
SETLOCAL
FOR %%I IN ("externals\out\gflags\%1\CMake") DO SET gflags_DIR=%%~dpfnI
MKDIR externals\out\glog\%1
MKDIR externals\build\glog\%1
PUSHD externals\build\glog\%1
SET generator=Visual Studio 14 2015
IF %1 == x64 (
  SET generator=%generator% Win64
)
cmake -G "%generator%" ../../../../glog ^
  -DCMAKE_INSTALL_PREFIX=../../../out/glog/%1 ^
  -Dgflags_DIR="%gflags_DIR%" ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON ^
  -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON ^
  -DCMAKE_FIND_PACKAGE_NO_SYSTEM_PACKAGE_REGISTRY=ON
POPD
ENDLOCAL
GOTO :EOF