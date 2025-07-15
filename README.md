# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0;
This is early version for co-work. The official version will be transferred to Gerrit
# Setup Development Invironment
Install below via https://docs.zephyrproject.org/latest/develop/getting_started/index.html

- Zephyr tools, such as cmake/python by choco
- Zephyr sdk, such as zephyr-sdk-0.17.0
# Get code
Run below commands to get repos

- west init -m  https://github.qualcomm.com/QCC730-ZE-1-0/qcc730.git  qghubzephyr
- cd qghubzephyr
- west update
# Build Image
Run below commands to build image

- pre-cmds, with samples as below
	- activate.bat for python
	- zephyr-env.cmd for ZEPHYR_BASE
- do_build.bat, with default parameters as below
	- set BOARD=qcc730evbx
	- set APP_DIR=%ZEPHYR_BASE%\samples\hello_world
# Flash Image
Prepare: run below cmd to install nvm_programmer
```
cd ..\modules\hal\qualcomm\tools\zflash
cp \\dine\QCT_IOT\Projects\Fermion\tools\nvm_prg\nvm_programmer .. -r
```
Run below cmd to flash image
```
cd ..\modules\hal\qualcomm\tools\zflash
flash_elf_zephyr.bat
```