# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0;
@echo off

set BUILD_MSG_LEVEL=-vvv
REM -p={auto,always,never}, no value is the same as "always"
set BUILD_PRISTINE_LEVEL=auto
set EXTRA_MODULES_DIR=%~dp0
set _EXTRA_MODULES_DIR=%EXTRA_MODULES_DIR:\=/%
REM set APP_NAME=hello_world
REM set APP_DIR=%ZEPHYR_BASE%\samples\%APP_NAME%
REM set APP_NAME=power_app
set APP_NAME=wifi_app
set APP_DIR=..\qapp\%APP_NAME%
set BUILD_DIR=build\%APP_NAME%
set BOARD=qcc730evbx

echo             BOARD :: %STR_BOOT% %BOARD%
echo   APPLICATION DIR :: %APP_DIR%
echo EXTRA_MODULES_DIR :: %EXTRA_MODULES_DIR%

set CMD=west %BUILD_MSG_LEVEL% build -p=%BUILD_PRISTINE_LEVEL% -d %BUILD_DIR% -b %BOARD% -s %APP_DIR% -- -DZEPHYR_EXTRA_MODULES=%_EXTRA_MODULES_DIR%

if not exist %BUILD_DIR% (
	mkdir %BUILD_DIR%
)

echo CMD=%CMD%
%CMD% > %BUILD_DIR%\build.log
REM %CMD%

echo "install images"
set zephyr_image_dir=%BUILD_DIR%\zephyr
if not exist %zephyr_image_dir%\zephyr.elf (
	echo "no elf generated"
	exit
)
set tools_path=..\modules\hal\qualcomm\tools
set zflash_path=%tools_path%\zflash
rm -f %zflash_path%\zephyr.bin
rm -f %zflash_path%\zephyr.elf
rm -f %zflash_path%\zephyr.map
cp %zephyr_image_dir%\zephyr.bin %zflash_path%\zephyr.bin
cp %zephyr_image_dir%\zephyr.elf %zflash_path%\zephyr.elf
cp %zephyr_image_dir%\zephyr.map %zflash_path%\zephyr.map
