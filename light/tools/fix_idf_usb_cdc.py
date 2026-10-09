"""Backport the ESP32-S2 USB descriptor lifetime fix when missing in ESP-IDF."""
import os
from pathlib import Path


def patch_console(text):
    marker = 'static void esp_usb_console_rom_cleanup(void)'
    start = text.index(marker)
    opening = text.index('{', start)
    first_cleanup = text.index('usb_dev_deinit();', opening)
    if 'rom_usb_cdc_set_descriptor_patch();' in text[opening:first_cleanup]:
        return text
    insertion = '''
#ifdef CONFIG_IDF_TARGET_ESP32S2
    // Bootloader descriptors point into RAM reclaimed by the application heap.
    rom_usb_cdc_set_descriptor_patch();
#endif
'''
    text = text[:opening + 1] + insertion + text[opening + 1:]
    header = '#include "esp32s2/rom/usb/usb_common.h"'
    if header not in text and '#include "rom/usb/usb_common.h"' not in text:
        include_marker = '#include "esp_rom_caps.h"'
        if include_marker not in text:
            raise ValueError('Unexpected USB console includes; refusing to patch.')
        text = text.replace(include_marker, include_marker + '\n#ifdef CONFIG_IDF_TARGET_ESP32S2\n' + header + '\n#endif', 1)
    return text


def main():
    path = Path(os.environ['IDF_PATH']) / 'components/esp_usb_cdc_rom_console/usb_console.c'
    original = path.read_text(encoding='utf-8')
    updated = patch_console(original)
    if updated == original:
        print('ESP-IDF already restores USB descriptors; no patch required.')
    else:
        path.write_text(updated, encoding='utf-8')
        print('Applied ESP32-S2 USB descriptor lifetime fix.')


if __name__ == '__main__':
    main()
