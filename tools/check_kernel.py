import re
import subprocess
import sys

LIMIT = 0x9FC00
elf = sys.argv[1]
nm = sys.argv[2]
out = subprocess.check_output([nm, elf]).decode()
end = None
for line in out.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[2] == "_kernel_end":
        end = int(parts[0], 16)
if end is None:
    sys.exit("check_kernel: _kernel_end not found")
print("    kernel end   0x%05x  (limit 0x%05x, %d KiB free)" % (end, LIMIT, (LIMIT - end) // 1024))
if end >= LIMIT:
    sys.exit("ERROR: kernel image reaches 0x%x, above the 0x9FC00 legacy hole" % end)
