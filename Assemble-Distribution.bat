@echo off
if not exist %~s1 (
	exit 1
)

xcopy /q /e /y %~dp0\base %~dp0\bin\distribution\ >nul
xcopy /q /y %~dp0\external\openvr\bin\win64\openvr_api.dll %~dp0\bin\distribution\ >nul
xcopy /q /y %~dp0\external\openvr\bin\win64\openvr_api.dll %~dp0\bin\distribution\bin\win64\ >nul
xcopy /q /e /y %~dp0\external\cloudxr\runtime %~dp0\bin\distribution\bin\win64\ >nul
xcopy /y %1\driver_cloudxr.dll %~dp0\bin\distribution\bin\win64\
xcopy /y %1\client_utility.exe %~dp0\bin\distribution\bin\win64\

xcopy /q /y %~dp0\tracing\Capture-ETL.bat %~dp0\bin\distribution\tracing\ >nul
xcopy /q /y %~dp0\tracing\DriverTracing.wprp %~dp0\bin\distribution\tracing\ >nul
