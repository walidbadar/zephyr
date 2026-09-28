# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Runner for the Altera Quartus Prime Programmer.

Programs a JTAG Indirect Configuration File (.jic) into the configuration
flash of an Altera SoC FPGA with quartus_pgm. The .jic is either given
directly, or generated with quartus_pfg from the FPGA design (.sof) and the
TF-A BL2 image, which the SDM loads into the HPS. Optionally, the TF-A FIP
image is added to the .jic as raw data at the offset BL2 loads it from.

quartus_pgm and quartus_pfg are distributed with Quartus Prime and the
Quartus Prime Programmer; they are not part of the Zephyr SDK.
"""

import os
import xml.etree.ElementTree as ET

from runners.core import RunnerCaps, ZephyrBinaryRunner

DEFAULT_CABLE = '1'
DEFAULT_MODE = 'ASX4'
DEFAULT_BL2_ADDRESS = 0x0
# Flash region reserved for the SDM boot information
BOOT_INFO_END = 0x1FFFFF


class QuartusBinaryRunner(ZephyrBinaryRunner):
    """Runner front-end for quartus_pgm and quartus_pfg."""

    def __init__(
        self,
        cfg,
        cable=DEFAULT_CABLE,
        jic=None,
        sof=None,
        bl2=None,
        bl2_address=DEFAULT_BL2_ADDRESS,
        fip=None,
        fip_offset=None,
        flash_device=None,
        flash_loader=None,
        mode=DEFAULT_MODE,
        pfg_options=None,
    ):
        super().__init__(cfg)
        self.cable = cable
        self.jic = jic
        self.sof = sof
        self.bl2 = bl2
        self.bl2_address = bl2_address
        self.fip = fip
        self.fip_offset = fip_offset
        self.flash_device = flash_device
        self.flash_loader = flash_loader
        self.mode = mode
        self.pfg_options = pfg_options or []

    @classmethod
    def name(cls):
        return 'quartus'

    @classmethod
    def capabilities(cls):
        return RunnerCaps(commands={'flash'})

    @classmethod
    def do_add_parser(cls, parser):
        parser.add_argument(
            '--cable',
            default=DEFAULT_CABLE,
            help=f'JTAG cable name or index for quartus_pgm -c (default: {DEFAULT_CABLE})',
        )
        parser.add_argument('--jic', help='program this .jic file as is, instead of generating one')
        parser.add_argument('--sof', help='FPGA design .sof file to generate the .jic from')
        parser.add_argument('--bl2', help='TF-A BL2 binary (bl2.bin) loaded by the SDM')
        parser.add_argument(
            '--bl2-address',
            type=lambda value: int(value, 0),
            default=DEFAULT_BL2_ADDRESS,
            help=f'BL2 load address in the HPS bootloader hex file '
            f'(default: {DEFAULT_BL2_ADDRESS:#x})',
        )
        parser.add_argument('--fip', help='TF-A FIP image (fip.bin) to add to the .jic as raw data')
        parser.add_argument(
            '--fip-offset',
            type=lambda value: int(value, 0),
            help='offset of the FIP image in the configuration flash; required with --fip',
        )
        parser.add_argument(
            '--flash-device', help='configuration flash device, quartus_pfg device option'
        )
        parser.add_argument(
            '--flash-loader', help='FPGA part number, quartus_pfg flash_loader option'
        )
        parser.add_argument(
            '--mode',
            default=DEFAULT_MODE,
            help=f'configuration mode, quartus_pfg mode option (default: {DEFAULT_MODE})',
        )
        parser.add_argument(
            '--pfg-option',
            dest='pfg_options',
            action='append',
            metavar='NAME=VALUE',
            help='additional quartus_pfg option; may be repeated; not supported with --fip',
        )

    @classmethod
    def do_create(cls, cfg, args):
        return QuartusBinaryRunner(
            cfg,
            cable=args.cable,
            jic=args.jic,
            sof=args.sof,
            bl2=args.bl2,
            bl2_address=args.bl2_address,
            fip=args.fip,
            fip_offset=args.fip_offset,
            flash_device=args.flash_device,
            flash_loader=args.flash_loader,
            mode=args.mode,
            pfg_options=args.pfg_options,
        )

    def do_run(self, command, **kwargs):
        self.require('quartus_pgm')

        jic = self.jic if self.jic else self._generate_jic()

        self.logger.info(f'Programming {jic}')
        self.check_call(['quartus_pgm', '-c', self.cable, '-m', 'jtag', '-o', f'pvi;{jic}'])

    def _generate_jic(self):
        missing = [
            option
            for option, value in (
                ('--sof', self.sof),
                ('--bl2', self.bl2),
                ('--flash-device', self.flash_device),
                ('--flash-loader', self.flash_loader),
            )
            if not value
        ]
        if missing:
            raise RuntimeError(
                f'quartus: {", ".join(missing)} required to generate a .jic; '
                'or pass --jic to program an existing file'
            )
        if self.fip and self.fip_offset is None:
            raise RuntimeError('quartus: --fip-offset required with --fip')
        if self.fip and self.pfg_options:
            raise RuntimeError('quartus: --pfg-option is not supported with --fip')

        self.require('quartus_pfg')

        from intelhex import IntelHex

        bl2_hex = os.path.join(self.cfg.build_dir, 'bl2.hex')
        ih = IntelHex()
        ih.loadbin(self.bl2, offset=self.bl2_address)
        ih.write_hex_file(bl2_hex)

        name = os.path.splitext(os.path.basename(self.sof))[0]
        jic = os.path.join(self.cfg.build_dir, name + '.jic')

        if self.fip:
            pfg = os.path.join(self.cfg.build_dir, name + '.pfg')
            self._write_pfg(pfg, name, bl2_hex)
            cmd = ['quartus_pfg', '-c', pfg]
        else:
            cmd = self._pfg_cmd(bl2_hex, jic)

        self.logger.info(f'Generating {jic}')
        self.check_call(cmd)

        return jic

    def _pfg_cmd(self, bl2_hex, jic):
        cmd = [
            'quartus_pfg',
            '-c',
            '-o',
            f'hps_path={bl2_hex}',
            '-o',
            f'device={self.flash_device}',
            '-o',
            f'flash_loader={self.flash_loader}',
            '-o',
            f'mode={self.mode}',
        ]
        for option in self.pfg_options:
            cmd.extend(['-o', option])
        cmd.extend([self.sof, jic])
        return cmd

    def _write_pfg(self, pfg, name, bl2_hex):
        """Write a quartus_pfg settings file placing the FIP at fip_offset."""
        root = ET.Element('pfg', version='1')
        ET.SubElement(root, 'settings', custom_db_dir='./', mode=self.mode)

        output_files = ET.SubElement(root, 'output_files')
        output = ET.SubElement(
            output_files, 'output_file', name=name, directory=self.cfg.build_dir, type='JIC'
        )
        ET.SubElement(output, 'file_options')
        secondary = ET.SubElement(output, 'secondary_file', type='MAP', name=name + '_jic')
        ET.SubElement(secondary, 'file_options')
        ET.SubElement(output, 'flash_device_id').text = 'Flash_Device_1'

        bitstreams = ET.SubElement(root, 'bitstreams')
        bitstream = ET.SubElement(bitstreams, 'bitstream', id='Bitstream_1')
        ET.SubElement(bitstream, 'path', hps_path=bl2_hex).text = self.sof

        raw_files = ET.SubElement(root, 'raw_files')
        ET.SubElement(
            raw_files, 'raw_file', bitswap='1', type='RBF', id='Raw_File_1'
        ).text = self.fip

        flash_devices = ET.SubElement(root, 'flash_devices')
        flash_device = ET.SubElement(
            flash_devices, 'flash_device', type=self.flash_device, id='Flash_Device_1'
        )
        for part_id, reserved, start, end in (
            ('BOOT_INFO', '1', 0, BOOT_INFO_END),
            ('P1', '0', None, None),
            ('FIP', '0', self.fip_offset, None),
        ):
            ET.SubElement(
                flash_device,
                'partition',
                reserved=reserved,
                fixed_s_addr='0' if start is None else '1',
                s_addr='auto' if start is None else f'{start:#010x}',
                e_addr='auto' if end is None else f'{end:#010x}',
                fixed_e_addr='0' if end is None else '1',
                id=part_id,
                size='0',
            )
        ET.SubElement(flash_devices, 'flash_loader').text = self.flash_loader

        assignments = ET.SubElement(root, 'assignments')
        for part_id, tag, ref in (
            ('P1', 'bitstream_id', 'Bitstream_1'),
            ('FIP', 'raw_file_id', 'Raw_File_1'),
        ):
            assignment = ET.SubElement(assignments, 'assignment', page='0', partition_id=part_id)
            ET.SubElement(assignment, tag).text = ref

        ET.indent(root)
        ET.ElementTree(root).write(pfg, encoding='utf-8')
