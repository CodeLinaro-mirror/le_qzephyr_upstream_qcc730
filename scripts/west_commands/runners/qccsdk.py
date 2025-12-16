# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0

from runners.core import RunnerCaps, ZephyrBinaryRunner
import argparse
from os import getenv, environ
from pathlib import Path
import sys
import os
import subprocess
import logging
import xml.etree.ElementTree as ET

# Configure module logger
logger = logging.getLogger(__name__)

# Determine output directory for generated files
# Default to ${ZEPHYR_HAL_QCOM_MODULE_DIR}/zephyr/blobs/ if set, otherwise current directory
OUTPUT_DIR = Path(os.getcwd())
try:
    zephyr_base = getenv("ZEPHYR_BASE")
    if zephyr_base:
        hal_qcom_dir = (
            Path(zephyr_base).absolute() / ".." / "modules" / "hal" / "qcom" 
        )
    if hal_qcom_dir:
        blobs_dir = Path(hal_qcom_dir) / "zephyr" / "blobs"
        if blobs_dir.exists() or blobs_dir.parent.exists():
            # Create blobs directory if it doesn't exist
            blobs_dir.mkdir(parents=True, exist_ok=True)
            OUTPUT_DIR = blobs_dir
except Exception:
    pass

print ("OUTPUT_DIR for generated files is ", str(OUTPUT_DIR))

# Construct path to firmware upgrade scripts
try:
    zephyr_base = getenv("ZEPHYR_BASE")
    if zephyr_base:
        FW_UPGRADE_SCRIPTS_PATH = (
            Path(zephyr_base).absolute() / ".." / "modules" / "hal" / "qcom" / "qfdt"
        )
        logger.debug(f"FW_UPGRADE_SCRIPTS_PATH: {FW_UPGRADE_SCRIPTS_PATH}")
        if FW_UPGRADE_SCRIPTS_PATH.exists():
            sys.path.append(str(FW_UPGRADE_SCRIPTS_PATH))
        else:
            logger.warning(f"FW_UPGRADE_SCRIPTS_PATH does not exist: {FW_UPGRADE_SCRIPTS_PATH}")
    else:
        logger.warning("ZEPHYR_BASE environment variable not set")
except Exception as e:
    logger.error(f"Failed to configure FW_UPGRADE_SCRIPTS_PATH: {e}")

# from gen_download_table import Download_Table

if sys.platform.startswith("win"):
    import winreg
else:
    winreg = None

class qccsdkRunner(ZephyrBinaryRunner):
    """qccsdk runner for flashing QCC730 with nvm_programmer.py."""
    def __init__(self, cfg, memory_type, jtag, chip_erase=False, all=False, reset=False, bdf=False, 
                 read_rram=False, read_addr=None, read_len=None, read_file=None):
        super().__init__(cfg)
        self.m = memory_type
        self.j = jtag
        self.erase = chip_erase
        self.all = all
        self.reset = reset
        self.bdf = bdf
        self.read_rram = read_rram
        self.read_addr = read_addr
        self.read_len = read_len
        self.read_file = read_file

    @classmethod
    def name(cls):
        return "qccsdk"

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
        parser.add_argument("--read-rram", action="store_true", help="Read RRAM instead of flashing (requires --read-addr, --read-len, --read-file)")
        parser.add_argument("--read-addr", type=str, help="Start address for reading RRAM (hex format, e.g., 0x208000)")
        parser.add_argument("--read-len", type=str, help="Length to read from RRAM (hex or decimal, e.g., 0x1000 or 4096)")
        parser.add_argument("--read-file", type=str, help="Output file path to save read data")
        parser.set_defaults(reset=True)

    @classmethod
    def do_create(cls, cfg, args: argparse.Namespace):
        return qccsdkRunner(cfg, memory_type=args.memory_type, jtag=args.jtag, 
                          chip_erase=args.chip_erase, all=args.all, reset=args.reset, bdf=args.bdf,
                          read_rram=args.read_rram, read_addr=args.read_addr, 
                          read_len=args.read_len, read_file=args.read_file)
    
    def do_run(self, command: str, **kwargs):
        if command == "flash" or command == "debug":
            self.flash(**kwargs)
        if command == "debug" or command == "attach":
            self.debug(**kwargs)

    def flash(self, **kwargs):
        # If read_rram flag is set, perform read operation instead of flash
        if self.read_rram:
            self.do_read_rram(**kwargs)
            return
        
        if self.j == "jlink":
            cfgpath = Path(self.cfg.board_dir) / ".." / "common" / "qcc730.JLinkScript"
        elif self.j == "ch347":
            cfgpath = Path(self.cfg.board_dir) / ".." / "common" / "qcc730_openocd_ch347.cfg"
        if not cfgpath.exists():
            raise FileNotFoundError(f"config file not found: {cfgpath}")

        module_path = (
            Path(getenv("ZEPHYR_BASE")).absolute()
            / r".."
            / "modules"
            / "hal"
            / "qcom"
        )
        nvmprogrammerpath = Path(module_path, "tools/qprgc")
        sechashpath = Path(module_path, "tools/qhash")
        blobs_path = Path(module_path, "zephyr/blobs")
        print(f"Module path: {module_path.as_posix()}")
        print(f"NVM Programmer path: {nvmprogrammerpath.as_posix()}")
        print(f"SecHash path: {sechashpath.as_posix()}")
        nvm_programmer = Path(nvmprogrammerpath, "nvm_programmer.py")
        prg_filename = self.build_conf.get("CONFIG_QCC730_PRG_FILE")
        prg_path = Path(blobs_path, prg_filename)
        sbl_filename = self.build_conf.get("CONFIG_QCC730_SBL_FILE")
        sbl_path = Path(blobs_path, sbl_filename)
        fdt_bin_name = Path(blobs_path, "frn_curr_age_with_app_bin.bin")
        fdt_default_name = Path(blobs_path, "frn_curr_age_default.bin")
        fdt_flash_name = Path(blobs_path, "firmware_table.bin")
        #build_root = os.getcwd()
        bin_name = Path(self.cfg.bin_file).as_posix()
        base_bin_name = os.path.basename(bin_name)
        name_without_ext = os.path.splitext(base_bin_name)[0]
        #print("bin_name: "+str(bin_name))
        #print("base_bin_name: "+str(base_bin_name))
        
        hashed_elf_name = Path(bin_name).parent / (name_without_ext + "_HASHED.elf")
        print("hashed_elf_name: "+str(hashed_elf_name))
        #wifi related
        regdb_path = Path(blobs_path, "regdb.bin")
        
        print("board_dir: "+str(self.cfg.board_dir))
        board_name = os.path.basename(self.cfg.board_dir)
        bdf_filename = self.build_conf.get("CONFIG_QCC730_BDF_FILE")
        bdf_path = Path(blobs_path, bdf_filename)
        cmd_pre = 'python %s -s %s -i %s --nvm-name rram --server-script %s '%(nvm_programmer, self.j, str(prg_path), str(cfgpath))
        cmd_flash_pre = 'python %s -s %s -i %s --nvm-name flash --server-script %s '%(nvm_programmer, self.j, str(prg_path), str(cfgpath))
        if self.erase:
            self.logger.info('Erasing chip')
            os.system('%s -E'%(cmd_flash_pre))
        if self.m == "rram" or self.m == "flash" :
            if self.all:
                if self.m == "rram":
                    self.logger.info(f'Flashing firmware description table: {fdt_bin_name}')
                    os.system('%s -b 0x208000 -f %s'%(cmd_pre, str(fdt_bin_name)))
                if self.m == "flash":
                    # Copy existing FDT file to OUTPUT_DIR if it exists
                    fdt_source = Path(blobs_path, "frn_curr_age_with_app_bin.bin")
                    
                    # Update download_config.xml with correct SBL path and FDT path
                    download_config_path = Path(module_path, "qfdt/download_config.xml")
                    if download_config_path.exists():
                        try:
                            tree = ET.parse(download_config_path)
                            root = tree.getroot()
                            # Update all FERMION_SBL entries with the actual sbl_path
                            for flash_elem in root.findall(".//flash[@image='FERMION_SBL']"):
                                flash_elem.set('file', str(sbl_path))
                            # Update FDT entry to use OUTPUT_DIR
                            for flash_elem in root.findall(".//flash[@image='FDT']"):
                                flash_elem.set('file', str(fdt_default_name))
                            # Save updated config to OUTPUT_DIR
                            updated_config_path = Path(blobs_path / "download_config.xml")
                            tree.write(str(updated_config_path))
                            self.logger.info(f'Updated download_config.xml with path: {blobs_path}')
                        except Exception as e:
                            self.logger.warning(f'Failed to update download_config.xml: {e}')
                    
                    self.logger.info(f'generating firmware description table: {fdt_default_name}')
                    # Generate FDT using gen_download_table.py with updated config
                    gen_download_table_script = Path(module_path, "qfdt/gen_download_table.py")
                    if gen_download_table_script.exists():
                        updated_config_path = Path(blobs_path / "download_config.xml")
                        # Set ZEPHYR_HAL_QCOM_MODULE_DIR environment variable to ensure OUTPUT_DIR is used
                        env = os.environ.copy()
                        env['ZEPHYR_HAL_QCOM_MODULE_DIR'] = str(module_path)
                        cmd_gen_fdt = f'python {gen_download_table_script} --app {hashed_elf_name} -c {updated_config_path} -A'
                        self.logger.info(f'Running: {cmd_gen_fdt}')
                        self.logger.info(f'With ZEPHYR_HAL_QCOM_MODULE_DIR={module_path}')
                        subprocess.run(cmd_gen_fdt, shell=True, env=env)
                    
                    # Use FDT from OUTPUT_DIR
                    self.logger.info(f'Flashing firmware description table: {fdt_default_name}')
                    os.system('%s -b 0x208000 -f %s'%(cmd_pre, str(fdt_default_name)))
                self.logger.info(f'Flashing SBL: {sbl_path}')
                os.system('%s -b 0x20a400 -f %s'%(cmd_pre, str(sbl_path)))
                self.logger.info(f'Flashing regdb: {regdb_path}')
                os.system('%s -b 0x373000 -f %s'%(cmd_pre, str(regdb_path)))
            if self.bdf:
                self.logger.info(f'Flashing bdf: {bdf_path}')
                os.system('%s -b 0x37a000 -f %s'%(cmd_pre, str(bdf_path)))
            self.logger.info(f'Flashing file: {bin_name}')
            if self.m == "rram":
                cmd = '%s -b 0x21a400 -f %s '%(cmd_pre, bin_name)
            if self.m == "flash" :
                self.logger.info(f'Flashing firmware description table in flash: {fdt_flash_name}')
                os.system('%s -b 0x0 -f %s'%(cmd_flash_pre, str(fdt_flash_name)))
                cmd = '%s -b 0x43000 -f %s '%(cmd_flash_pre, hashed_elf_name)
        else:
            print(f"Error: flash failed - unknown memory type {self.m}")
            raise
        if self.reset:
            cmd += " --reset "
        print(cmd)
        os.system(cmd)

    def do_read_rram(self, **kwargs):
        """Read RRAM from specified address and length, save to file."""
        if not self.read_addr or not self.read_len or not self.read_file:
            raise ValueError("--read-addr, --read-len, and --read-file are required when using --read-rram")
        
        # Parse address and length (support both hex and decimal)
        try:
            addr = int(self.read_addr, 0)  # 0 base allows auto-detection of hex/decimal
            length = int(self.read_len, 0)
        except ValueError as e:
            raise ValueError(f"Invalid address or length format: {e}")
        
        if self.j == "jlink":
            cfgpath = Path(self.cfg.board_dir) / ".." / "common" / "qcc730.JLinkScript"
        elif self.j == "ch347":
            cfgpath = Path(self.cfg.board_dir) / ".." / "common" / "qcc730_openocd_ch347.cfg"
        
        if not cfgpath.exists():
            raise FileNotFoundError(f"config file not found: {cfgpath}")

        module_path = (
            Path(getenv("ZEPHYR_BASE")).absolute()
            / r".."
            / "modules"
            / "hal"
            / "qcom"
        )
        nvmprogrammerpath = Path(module_path, "tools/qprgc")
        blobs_path = Path(module_path, "zephyr/blobs")
        nvm_programmer = Path(nvmprogrammerpath, "nvm_programmer.py")
        prg_filename = self.build_conf.get("CONFIG_QCC730_PRG_FILE")
        prg_path = Path(blobs_path, prg_filename)
        
        # Determine output file path (absolute or relative to current directory)
        output_file = Path(self.read_file)
        if not output_file.is_absolute():
            output_file = Path(os.getcwd()) / output_file
        
        # Create output directory if it doesn't exist
        output_file.parent.mkdir(parents=True, exist_ok=True)
        
        self.logger.info(f'Reading RRAM from address 0x{addr:X}, length 0x{length:X} ({length} bytes)')
        self.logger.info(f'Output file: {output_file}')
        
        # Build read command
        # nvm_programmer.py uses -d (--read) for read operation, -b for base address, -S for size, -f for output file
        cmd = f'python {nvm_programmer} -s {self.j} -i {prg_path} --nvm-name {self.m} --server-script {cfgpath} -d -b 0x{addr:X} -S 0x{length:X} -f {output_file}'
        
        print(f"Executing: {cmd}")
        result = os.system(cmd)
        
        if result == 0:
            self.logger.info(f'Successfully read {length} bytes from 0x{addr:X} to {output_file}')
        else:
            self.logger.error(f'Failed to read RRAM (exit code: {result})')
            raise RuntimeError(f'RRAM read operation failed with exit code {result}')

    def debug(self, **kwargs):
        if self.j == "jlink":
            script_path = Path(self.cfg.board_dir) / ".." / "common" / "qcc730.JLinkScript"
            if not script_path.exists():
                raise FileNotFoundError(f"JLinkScript config file not found: {script_path}")
            jlinkgdbserver_name = "JLinkGDBServer.exe" if os.name == "nt" else "JLinkGDBServer"

            jlink_dir = Path()
            jlink_root = os.getenv("JLINK_PATH")
            if jlink_root:
                jlink_dir = Path(jlink_root).expanduser().resolve()
            else:
                if os.name == "nt":
                    #jlink_dir = Path(r"C:\Program Files\SEGGER\JLink_V794f").resolve()
                    reg_paths = [
                        (winreg.HKEY_LOCAL_MACHINE,
                         r"SOFTWARE\WOW6432Node\SEGGER\J-Link"),
                        (winreg.HKEY_LOCAL_MACHINE,
                         r"SOFTWARE\SEGGER\J-Link"),
                    ]

                    for root, subkey in reg_paths:
                        try:
                            with winreg.OpenKey(root, subkey) as key:
                                install_path, _ = winreg.QueryValueEx(key, "InstallPath")
                                candidate = Path(install_path) / jlinkgdbserver_name
                                if candidate.is_file():
                                    print(candidate)
                                    jlink_dir = candidate.parent
                                    break
                        except FileNotFoundError:
                            continue

            jlinkgdbserver = jlink_dir / jlinkgdbserver_name

            if not jlinkgdbserver.is_file():
                raise FileNotFoundError(
                    f"JLink executable not found at {jlinkgdbserver!s}. "
                    "Set the JLINK_ROOT environment variable to the correct directory."
                )

            server_cmd = str(jlinkgdbserver)
            script_path  = str(Path(script_path).resolve())

            server_cmd = [
                server_cmd,
                "-select", "USB",
                "-device", "Cortex-M4",
                "-endian", "little",
                "-if", "JTAG",
                "-speed", "1000",
                "-JTAGconf", "0,0",
                "-noir",
                "-port", "3333",
                "-singlerun",
                "-silent",
                "-jlinkscriptfile", script_path,
            ]

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
        
        # Fix for Windows Ctrl+C issue: Replace gdb-py with gdb
        gdb_path = Path(self.cfg.gdb)

        if os.name == "nt" and gdb_path.name.endswith("-py.exe"):
            # Replace gdb-py.exe with gdb.exe
            gdb_no_py = str(gdb_path.parent / gdb_path.name.replace("-py.exe", ".exe"))
            if Path(gdb_no_py).exists():
                gdb_exe = gdb_no_py
                #print(f"DEBUG: Replaced with: {gdb_exe}")
            else:
                gdb_exe = self.cfg.gdb
                #print(f"DEBUG: Replacement not found, using original: {gdb_exe}")
        else:
            gdb_exe = self.cfg.gdb
        
        gdb_cmd = [gdb_exe, elf_name, '-ex', 'target extended-remote localhost:3333']
        self.run_server_and_client(server_cmd, gdb_cmd)
