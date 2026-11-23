"""Put freshly built encoders (encoders/dist/lame.wasm, aac.wasm) into raqim.html.

Usage:  python3 encoders/embed.py raqim.html
The tool keeps each encoder as base64 text inside it (LAME_WASM_B64 / AAC_WASM_B64),
so it stays a single file that works offline.
"""
import base64, pathlib, re, sys

html_path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "raqim.html")
dist = pathlib.Path(__file__).parent / "dist"
s = html_path.read_text(encoding="utf-8")
for var, name in (("LAME_WASM_B64", "lame.wasm"), ("AAC_WASM_B64", "aac.wasm")):
    b64 = base64.b64encode((dist / name).read_bytes()).decode()
    s, n = re.subn(r'const %s="[A-Za-z0-9+/=]*";' % var, 'const %s="%s";' % (var, b64), s, count=1)
    if n != 1:
        sys.exit("could not find %s in %s" % (var, html_path))
    print("updated", var, len(b64), "chars")
html_path.write_text(s, encoding="utf-8")
