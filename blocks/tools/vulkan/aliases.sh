#!/bin/sh
# Regenerates src/vk/vk_entry_alias.h from the entry-point lists in
# src/vk/vk_entry.h. Run it after adding a function to either list.
set -eu
cd "$(dirname "$0")/../.."
python3 - <<'PY'
import re, pathlib
header = pathlib.Path('src/vk/vk_entry.h').read_text()
def names(section):
    body = re.search(r"#define %s\(X\) \\\n((?:.*\\\n)*.*\n)" % section, header).group(1)
    return re.findall(r"X\((\w+)\)", body)
every = names("ENJOER_VK_INSTANCE_ENTRIES") + names("ENJOER_VK_DEVICE_ENTRIES")
path = pathlib.Path('src/vk/vk_entry_alias.h')
text = path.read_text()
start = text.index("#define vk")
end = text.rindex("\n#endif")
path.write_text(text[:start] + "".join("#define %s enjoer_vk.%s\n" % (n, n) for n in every) + text[end:])
print("wrote src/vk/vk_entry_alias.h (%d entry points)" % len(every))
PY
