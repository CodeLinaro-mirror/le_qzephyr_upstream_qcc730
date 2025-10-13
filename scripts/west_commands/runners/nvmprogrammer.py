# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0

from runners.core import RunnerCaps, ZephyrBinaryRunner
import argparse
from os import path, getenv, environ
from pathlib import Path
import sys
import os


class NVMProgrammerRunner(ZephyrBinaryRunner):
    """NVM Programmer runner for flashing QCC730 with nvm_programmer.py."""
    def __init__(self, cfg, memory_type, jtag, chip_erase=False, all=False, reset=False, bdf=False):
        super().__init__(cfg)
        self.m = memory_type
        self.j = jtag
        self.erase = chip_erase
        self.all = all
        self.reset = reset
        self.bdf = bdf

    @classmethod
    def name(cls):
        return "nvmprogrammer"

    @classmethod
    def capabilities(cls):
        return RunnerCaps(commands={'flash', 'debug', 'attach'}, reset=True)

    @classmethod
    def do_add_parser(cls, parser):
        parser.add_argument("-m", "--memory-type", choices=["rram", "flash"], action="store", help="Memory type: rram or flash", default="rram")
        parser.add_argument("-j", "--jtag", choices=["ch347", "jlink"], action="store", help="Jtag: ch347 or jlink", default="ch347")
        parser.add_argument("-e", "--chip-erase", action="store_true", help="Erase chip")
        parser.add_argument("-a", "--all", action="store_true", help="Write ftd, SBL, regdb and Zephyr app image")
        parser.add_argument("--bdf", action="store_true", help="Write bdf [WARNING: may affect WiFi RF performance]")
        parser.set_defaults(reset=True)

    @classmethod
    def do_create(cls, cfg, args: argparse.Namespace):
        return NVMProgrammerRunner(cfg, memory_type=args.memory_type, jtag=args.jtag, chip_erase=args.chip_erase, all=args.all, reset=args.reset, bdf=args.bdf)
    
    def do_run(self, command: str, **kwargs):
        if command == "flash" or command == "debug":
            self.flash(**kwargs)
        if command == "debug" or command == "attach":
            self.debug(**kwargs)

    def flash(self, **kwargs):
        openocdcfgpath = Path(self.cfg.board_dir) / ".." / "common" / "qcc730_openocd_ch347.cfg"
        if not openocdcfgpath.exists():
            raise FileNotFoundError(f"OpenOCD config file not found: {openocdcfgpath}")
        original_dir = os.getcwd()
        os.chdir(openocdcfgpath.parent)

        module_path = (
            Path(getenv("ZEPHYR_BASE")).absolute()
            / r".."
            / "modules"
            / "hal"
            / "qcom"
        )
        nvmprogrammerpath = Path(module_path, "zephyr/blobs")
        sechashpath = Path(module_path, "tools/qhash")
        blobs_path = Path(module_path, "zephyr/blobs")
        print(f"Module path: {module_path.as_posix()}")
        print(f"NVM Programmer path: {nvmprogrammerpath.as_posix()}")
        print(f"SecHash path: {sechashpath.as_posix()}")
        nvm_programmer = Path(nvmprogrammerpath, "nvm_programmer.exe")
        prg_elf_name = Path(blobs_path, "FERMION_NVM_PROGRAMMER.elf")
        sbl_elf_name = Path(blobs_path, "FERMION_SBL_HASHED.elf")
        fdt_bin_name = Path(blobs_path, "frn_curr_age_with_app_bin.bin")
        #build_root = os.getcwd()
        bin_name = Path(self.cfg.bin_file).as_posix()
        elf_name = Path(self.cfg.elf_file).as_posix()
        #print("bin_name: "+str(bin_name))
        #wifi related
        regdb_path = Path(blobs_path, "regdb.bin")
        
        print("board_dir: "+str(self.cfg.board_dir))
        board_name = os.path.basename(self.cfg.board_dir)
        bdf_filename = self.build_conf.get("CONFIG_QCC730_BDF_FILE")
        bdf_path = Path(blobs_path, bdf_filename)
        cmd_pre = '%s -s %s -i %s --nvm-name rram '%(nvm_programmer, self.j, str(prg_elf_name))
        if self.erase:
            self.logger.info('Erasing chip')
            os.system('%s -E'%cmd_pre)
        if self.m == "rram":
            if self.all:
                self.logger.info(f'Flashing firmware description table: {fdt_bin_name}')
                os.system('%s -b 0x208000 -f %s'%(cmd_pre, str(fdt_bin_name)))
                self.logger.info(f'Flashing SBL: {sbl_elf_name}')
                os.system('%s -b 0x20a400 -f %s'%(cmd_pre, str(sbl_elf_name)))
                self.logger.info(f'Flashing regdb: {regdb_path}')
                os.system('%s -b 0x373000 -f %s'%(cmd_pre, str(regdb_path)))
            if self.bdf:
                self.logger.info(f'Flashing bdf: {bdf_path}')
                os.system('%s -b 0x37a000 -f %s'%(cmd_pre, str(bdf_path)))
            self.logger.info(f'Flashing file: {bin_name}')
            cmd = '%s -b 0x21a400 -f %s '%(cmd_pre, bin_name)
        else:
            print(f"Error: flash failed - unknown memory type {self.m}")
            raise
        if self.reset:
            cmd += " --reset "
        print(cmd)
        os.system(cmd)

        os.chdir(original_dir)

    def debug(self, **kwargs):
        if self.j == "jlink":
            self.logger.error('JLink is not yet supported for debugging.')
        elif self.j == "ch347":
            openocdcfgpath = Path(self.cfg.board_dir) / ".." / "common" / "qcc730_openocd_ch347.cfg"
            if not openocdcfgpath.exists():
                raise FileNotFoundError(f"OpenOCD config file not found: {openocdcfgpath}")
            exe_name = "openocd.exe" if os.name == "nt" else "openocd"
            openocd_path = Path(environ.get("OPENOCD_PATH")) / exe_name
            server_cmd = [str(openocd_path), "-f", str(openocdcfgpath), "-l", "openocd.log"]
        
        self.require(server_cmd[0])
        self.require(self.cfg.gdb)

        elf_name = str(Path(self.cfg.elf_file))
        gdb_cmd = [self.cfg.gdb, elf_name, "-ex", "target extended-remote localhost:3333"]
        self.run_server_and_client(server_cmd, gdb_cmd)
