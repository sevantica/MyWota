#!/usr/bin/env python3
"""
Bundle bootloader and application into a single .uf2 file for development.

This creates a combined binary that can be flashed with picotool in one step.
- Bootloader at 0x10000000
- Application (with header) at 0x10008000
"""

import argparse
import os
import struct
import sys

# UF2 constants
UF2_MAGIC_START0 = 0x0A324655  # "UF2\n"
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56

# Flash addresses
BOOTLOADER_ADDRESS = 0x10000000
APP_ADDRESS = 0x10008000
FLASH_PAGE_SIZE = 256


def create_uf2_block(data: bytes, block_no: int, num_blocks: int, target_addr: int) -> bytes:
    """Create a single UF2 block (512 bytes)."""
    # Pad data to 256 bytes
    if len(data) < FLASH_PAGE_SIZE:
        data = data + b'\x00' * (FLASH_PAGE_SIZE - len(data))
    
    # UF2 block structure
    block = struct.pack(
        '<IIIIIIII',
        UF2_MAGIC_START0,
        UF2_MAGIC_START1,
        UF2_FLAG_FAMILY_ID,
        target_addr,
        FLASH_PAGE_SIZE,
        block_no,
        num_blocks,
        RP2040_FAMILY_ID
    )
    block += data[:FLASH_PAGE_SIZE]
    block += b'\x00' * (512 - 32 - FLASH_PAGE_SIZE - 4)  # Padding
    block += struct.pack('<I', UF2_MAGIC_END)
    
    return block


def binary_to_uf2(binary_data: bytes, start_address: int) -> bytes:
    """Convert binary data to UF2 format."""
    num_blocks = (len(binary_data) + FLASH_PAGE_SIZE - 1) // FLASH_PAGE_SIZE
    uf2_data = b''
    
    for i in range(num_blocks):
        offset = i * FLASH_PAGE_SIZE
        chunk = binary_data[offset:offset + FLASH_PAGE_SIZE]
        addr = start_address + offset
        uf2_data += create_uf2_block(chunk, i, num_blocks, addr)
    
    return uf2_data


def create_combined_uf2(bootloader_bin: bytes, app_bin: bytes) -> bytes:
    """Create a combined UF2 with bootloader and app."""
    # Calculate total blocks
    bl_blocks = (len(bootloader_bin) + FLASH_PAGE_SIZE - 1) // FLASH_PAGE_SIZE
    app_blocks = (len(app_bin) + FLASH_PAGE_SIZE - 1) // FLASH_PAGE_SIZE
    total_blocks = bl_blocks + app_blocks
    
    uf2_data = b''
    block_no = 0
    
    # Add bootloader blocks
    for i in range(bl_blocks):
        offset = i * FLASH_PAGE_SIZE
        chunk = bootloader_bin[offset:offset + FLASH_PAGE_SIZE]
        addr = BOOTLOADER_ADDRESS + offset
        uf2_data += create_uf2_block(chunk, block_no, total_blocks, addr)
        block_no += 1
    
    # Add app blocks
    for i in range(app_blocks):
        offset = i * FLASH_PAGE_SIZE
        chunk = app_bin[offset:offset + FLASH_PAGE_SIZE]
        addr = APP_ADDRESS + offset
        uf2_data += create_uf2_block(chunk, block_no, total_blocks, addr)
        block_no += 1
    
    return uf2_data


# Firmware header constants (must match bootloader)
FIRMWARE_HEADER_SIZE = 16
FIRMWARE_MAGIC = 0x52503255  # "RP2U"


def main():
    parser = argparse.ArgumentParser(description='Bundle bootloader and app into combined .uf2')
    parser.add_argument('--bootloader', '-b', required=True, help='Path to bootloader.bin')
    parser.add_argument('--app', '-a', required=True, help='Path to app binary (firmware.bin with header, or raw .bin)')
    parser.add_argument('--output', '-o', required=True, help='Output combined.uf2 path')
    
    args = parser.parse_args()
    
    # Read bootloader
    if not os.path.exists(args.bootloader):
        print(f"Error: Bootloader not found: {args.bootloader}")
        sys.exit(1)
    
    with open(args.bootloader, 'rb') as f:
        bootloader_bin = f.read()
    
    # Check bootloader size
    if len(bootloader_bin) > 0x8000:  # 32KB max
        print(f"Error: Bootloader too large ({len(bootloader_bin)} bytes, max 32KB)")
        sys.exit(1)
    
    # Read app
    if not os.path.exists(args.app):
        print(f"Error: App binary not found: {args.app}")
        sys.exit(1)
    
    with open(args.app, 'rb') as f:
        app_bin = f.read()
    
    # Check if app has firmware header and strip it for direct flash
    # When flashing combined bootloader+app, the app must be raw binary at 0x10008000
    # The header is only used for SD card OTA updates
    if len(app_bin) > FIRMWARE_HEADER_SIZE:
        magic = struct.unpack('<I', app_bin[0:4])[0]
        if magic == FIRMWARE_MAGIC:
            print(f"  Detected firmware header (magic 0x{magic:08X}), stripping {FIRMWARE_HEADER_SIZE} bytes for direct flash")
            app_bin = app_bin[FIRMWARE_HEADER_SIZE:]
    
    # Create combined UF2
    combined_uf2 = create_combined_uf2(bootloader_bin, app_bin)
    
    # Write output
    with open(args.output, 'wb') as f:
        f.write(combined_uf2)
    
    print(f"Combined firmware created successfully!")
    print(f"  Bootloader: {len(bootloader_bin):,} bytes @ 0x{BOOTLOADER_ADDRESS:08X}")
    print(f"  Application: {len(app_bin):,} bytes @ 0x{APP_ADDRESS:08X}")
    print(f"  Output: {args.output} ({len(combined_uf2):,} bytes)")


if __name__ == '__main__':
    main()
