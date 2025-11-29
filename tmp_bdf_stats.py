import sys
path = r'c:\Users\prane\Documents\proOS\proos\assets\font.bdf'
max_dw = 0
max_bbx = 0
max_total = 0
ascii_dw = []
cur_dw = None
cur_enc = None
with open(path, 'r') as f:
    for raw in f:
        line = raw.strip()
        if line.startswith('ENCODING'):
            parts = line.split()
            cur_enc = int(parts[1])
        elif line.startswith('DWIDTH'):
            parts = line.split()
            cur_dw = int(parts[1])
            if cur_dw > max_dw:
                max_dw = cur_dw
            if cur_enc is not None and 32 <= cur_enc <= 126:
                ascii_dw.append(cur_dw)
        elif line.startswith('BBX'):
            parts = line.split()
            w = int(parts[1])
            xoff = int(parts[3])
            if w > max_bbx:
                max_bbx = w
            total = w + max(0, xoff)
            if total > max_total:
                max_total = total

if ascii_dw:
    avg_ascii = sum(ascii_dw) / len(ascii_dw)
else:
    avg_ascii = 0.0

print('max_dw', max_dw)
print('max_bbx', max_bbx)
print('max_total', max_total)
print('avg_ascii_dw', avg_ascii)
