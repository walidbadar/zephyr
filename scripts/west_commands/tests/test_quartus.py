# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

import argparse
import os
import xml.etree.ElementTree as ET
from unittest.mock import call, patch

import pytest
from conftest import RC_BUILD_DIR

from runners.quartus import QuartusBinaryRunner

TEST_JIC = 'test.jic'
TEST_SOF = '/test/design.sof'
TEST_BL2 = '/test/bl2.bin'
TEST_FIP = '/test/fip.bin'
TEST_FLASH_DEVICE = 'MT25QU02G'
TEST_FLASH_LOADER = 'A5ED065BB32AE6SR0'

GEN_ARGS = [
    '--sof',
    TEST_SOF,
    '--bl2',
    TEST_BL2,
    '--flash-device',
    TEST_FLASH_DEVICE,
    '--flash-loader',
    TEST_FLASH_LOADER,
]

BL2_HEX = os.path.join(RC_BUILD_DIR, 'bl2.hex')
GEN_JIC = os.path.join(RC_BUILD_DIR, 'design.jic')


def pfg_cmd(mode='ASX4', extra=()):
    cmd = [
        'quartus_pfg',
        '-c',
        '-o',
        f'hps_path={BL2_HEX}',
        '-o',
        f'device={TEST_FLASH_DEVICE}',
        '-o',
        f'flash_loader={TEST_FLASH_LOADER}',
        '-o',
        f'mode={mode}',
    ]
    for option in extra:
        cmd.extend(['-o', option])
    return [*cmd, TEST_SOF, GEN_JIC]


def pgm_cmd(jic, cable='1'):
    return ['quartus_pgm', '-c', cable, '-m', 'jtag', '-o', f'pvi;{jic}']


def create_runner(runner_config, args):
    parser = argparse.ArgumentParser(allow_abbrev=False)
    QuartusBinaryRunner.add_parser(parser)
    return QuartusBinaryRunner.create(runner_config, parser.parse_args(args))


@patch('runners.core.ZephyrBinaryRunner.require', side_effect=lambda program: program)
@patch('runners.core.ZephyrBinaryRunner.check_call')
def test_quartus_jic(cc, req, runner_config):
    runner = create_runner(runner_config, ['--jic', TEST_JIC, '--cable', 'USB-BlasterII'])
    runner.run('flash')

    assert cc.call_args_list == [call(pgm_cmd(TEST_JIC, cable='USB-BlasterII'))]


@pytest.mark.parametrize(
    'extra_args, expected_pfg',
    [
        ([], pfg_cmd()),
        (['--mode', 'ASX1'], pfg_cmd(mode='ASX1')),
        (
            ['--pfg-option', 'hps=1', '--pfg-option', 'compression=ON'],
            pfg_cmd(extra=('hps=1', 'compression=ON')),
        ),
    ],
)
@patch('intelhex.IntelHex')
@patch('runners.core.ZephyrBinaryRunner.require', side_effect=lambda program: program)
@patch('runners.core.ZephyrBinaryRunner.check_call')
def test_quartus_generate(cc, req, ih, extra_args, expected_pfg, runner_config):
    runner = create_runner(runner_config, [*GEN_ARGS, '--bl2-address', '0x1000', *extra_args])
    runner.run('flash')

    ih.return_value.loadbin.assert_called_once_with(TEST_BL2, offset=0x1000)
    ih.return_value.write_hex_file.assert_called_once_with(BL2_HEX)
    assert cc.call_args_list == [call(expected_pfg), call(pgm_cmd(GEN_JIC))]


@patch('runners.core.ZephyrBinaryRunner.require', side_effect=lambda program: program)
@patch('runners.core.ZephyrBinaryRunner.check_call')
def test_quartus_missing_args(cc, req, runner_config):
    runner = create_runner(runner_config, ['--sof', TEST_SOF])

    with pytest.raises(RuntimeError, match='--bl2, --flash-device, --flash-loader'):
        runner.run('flash')
    cc.assert_not_called()


@patch('intelhex.IntelHex')
@patch('runners.core.ZephyrBinaryRunner.require', side_effect=lambda program: program)
@patch('runners.core.ZephyrBinaryRunner.check_call')
def test_quartus_fip(cc, req, ih, runner_config, tmp_path):
    build_dir = str(tmp_path)
    runner = create_runner(
        runner_config._replace(build_dir=build_dir),
        [*GEN_ARGS, '--fip', TEST_FIP, '--fip-offset', '0x3C00000'],
    )
    runner.run('flash')

    pfg = os.path.join(build_dir, 'design.pfg')
    jic = os.path.join(build_dir, 'design.jic')
    assert cc.call_args_list == [call(['quartus_pfg', '-c', pfg]), call(pgm_cmd(jic))]

    root = ET.parse(pfg).getroot()
    assert root.find('settings').get('mode') == 'ASX4'
    output = root.find('output_files/output_file')
    assert (output.get('name'), output.get('directory')) == ('design', build_dir)
    path = root.find('bitstreams/bitstream/path')
    assert (path.text, path.get('hps_path')) == (TEST_SOF, os.path.join(build_dir, 'bl2.hex'))
    assert root.find('raw_files/raw_file').text == TEST_FIP
    assert root.find('flash_devices/flash_device').get('type') == TEST_FLASH_DEVICE
    assert root.find('flash_devices/flash_loader').text == TEST_FLASH_LOADER
    fip = root.find("flash_devices/flash_device/partition[@id='FIP']")
    assert (fip.get('s_addr'), fip.get('fixed_s_addr')) == ('0x03c00000', '1')
    assignment = root.find("assignments/assignment[@partition_id='FIP']")
    assert assignment.find('raw_file_id').text == 'Raw_File_1'


@pytest.mark.parametrize(
    'extra_args, error',
    [
        ([], '--fip-offset required with --fip'),
        (
            ['--fip-offset', '0x3C00000', '--pfg-option', 'compression=ON'],
            '--pfg-option is not supported with --fip',
        ),
    ],
)
@patch('runners.core.ZephyrBinaryRunner.require', side_effect=lambda program: program)
@patch('runners.core.ZephyrBinaryRunner.check_call')
def test_quartus_fip_invalid_args(cc, req, extra_args, error, runner_config):
    runner = create_runner(runner_config, [*GEN_ARGS, '--fip', TEST_FIP, *extra_args])

    with pytest.raises(RuntimeError, match=error):
        runner.run('flash')
    cc.assert_not_called()
