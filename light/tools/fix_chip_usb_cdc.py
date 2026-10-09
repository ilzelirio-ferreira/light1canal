"""Add USB CDC output support to the ESP-Matter bundled CHIP shell."""
import os
from pathlib import Path

source = Path(os.environ['ESP_MATTER_PATH']) / 'connectedhomeip/connectedhomeip/src/lib/shell/streamer_esp32.cpp'
text = source.read_text(encoding='utf-8')
start = text.index('ssize_t streamer_esp32_write(')
end = text.index('static streamer_t', start)
function = text[start:end]
if 'CONFIG_ESP_CONSOLE_USB_CDC' in function:
    print('CHIP shell already supports USB CDC; no patch required.')
else:
    marker = '#if CONFIG_ESP_CONSOLE_NONE'
    if function.count(marker) != 1:
        raise SystemExit('Unexpected CHIP shell implementation; USB CDC patch was not applied.')
    implementation = '''#if defined(CONFIG_ESP_CONSOLE_USB_CDC)
    // The ESP-IDF USB CDC console already supplies the stdout VFS backend.
    size_t written = fwrite(buf, 1, len, stdout);
    fflush(stdout);
    return static_cast<ssize_t>(written);
#endif
'''
    function = function.replace(marker, implementation + marker, 1)
    source.write_text(text[:start] + function + text[end:], encoding='utf-8')
    print('Added USB CDC output support to the CHIP shell.')
