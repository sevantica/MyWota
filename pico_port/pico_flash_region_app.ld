/* Custom flash region for app running with bootloader
 * Bootloader occupies first 32KB (0x10000000 - 0x10007FFF)
 * App starts at 0x10008000
 */
FLASH(rx) : ORIGIN = 0x10008000, LENGTH = (2 * 1024 * 1024) - 32k
