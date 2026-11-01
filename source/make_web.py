import sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
rows = [",".join(str(b) for b in data[i:i + 16]) for i in range(0, len(data), 16)]
out = ("/* generated from index.html - do not edit */\n"
       "static const unsigned char WEB_PAGE[%d] = {\n%s\n};\n" % (len(data), ",\n".join(rows)))
open(dst, "w").write(out)
print("ok:", len(data), "bytes")
