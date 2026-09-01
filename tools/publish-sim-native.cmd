@ECHO OFF
REM Publishes Sim.Net.Client's NativeAOT shared library.
REM
REM This exists for one reason: PATH. The link step runs findvcvarsall.bat, which calls vcvarsall.bat
REM with its stdout sent to NUL but its stderr left alone; vcvarsall invokes a bare "vswhere.exe", and
REM when that is not on PATH the resulting error text is captured by MSBuild and spliced into the
REM linker path. So the VS Installer directory has to be on PATH before dotnet is invoked.
REM
REM Doing that from CMake directly does not work. A Windows PATH is semicolon-separated and CMake
REM treats semicolons as list separators, so a "PATH=..." argument is torn into one argument per
REM entry. Setting it here also PREPENDS to the live %PATH% rather than replacing it, which matters:
REM MSBuild hands custom build steps the MSVC environment, and overwriting PATH would throw it away.
REM
REM   %1 = VS Installer directory, or "" if it was not found
REM   %2 = the Sim.Net.Client project directory
REM   %3 = configuration (Debug or Release)
REM   %4 = runtime identifier
SETLOCAL

IF NOT "%~1"=="" SET "PATH=%~1;%PATH%"

dotnet publish "%~2" -c %~3 -r %~4 --nologo -v quiet
EXIT /B %ERRORLEVEL%
