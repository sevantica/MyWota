/* Custom flash region for app running with bootloader
 * Bootloader occupies first 64KB (0x10000000 - 0x1000FFFF)
 * App starts at 0x10010000
 */
FLASH(rx) : ORIGIN = 0x10010000, LENGTH = (2 * 1024 * 1024) - 64k
