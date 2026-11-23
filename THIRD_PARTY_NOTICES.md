# Third-party notices

Raqim embeds or used the following third-party software. Full license texts are in [`licenses/`](licenses/).

| Component | Used in | License | Source |
| --- | --- | --- | --- |
| LAME 3.100 (MP3 encoder) | `raqim.html` (WebAssembly) and `Raqim.exe` (native DLLs), with Raqim's wrapper | LGPL 2.0 — `licenses/LAME-LGPL-2.0.txt` | https://lame.sourceforge.io (mirror used: https://github.com/arseneyr/lame, commit `98db548e`) |
| Fraunhofer FDK AAC (AAC encoder) | `raqim.html` (WebAssembly) and `Raqim.exe` (native DLLs, portable code path), with Raqim's wrapper | Software License for The Fraunhofer FDK AAC Codec Library — `licenses/FDK-AAC-NOTICE.txt`. No patent license is granted. | https://github.com/mstorsjo/fdk-aac (commit `ec82feb3`) |
| minimp3 (MP3 decoder) | `Raqim.exe` (`native/src/minimp3.h`) | CC0 1.0 — `licenses/minimp3-CC0.txt` | https://github.com/lieff/minimp3 |
| wasm-media-encoders 0.7.0 | stage 2 of the tool (history) | MIT — `licenses/wasm-media-encoders-MIT.txt` | https://github.com/arseneyr/wasm-media-encoders |
| lamejs 1.2.1 | stage 1 of the tool (history) | LGPL 3.0 — `licenses/lamejs-LICENSE.txt` | https://github.com/zhuker/lamejs |
| marked 12.0.2 | `tools/markdown-viewer` | MIT — `licenses/marked-LICENSE.md` | https://github.com/markedjs/marked |
| DOMPurify 3.1.6 | `tools/markdown-viewer` | Apache 2.0 / MPL 2.0 — `licenses/DOMPurify-LICENSE.txt` | https://github.com/cure53/DOMPurify |

LGPL compliance for LAME: the exact LAME sources (pinned commit), Raqim's wrapper and the build script in `encoders/` let anyone rebuild `lame.wasm` and re-embed it with `encoders/embed.py`.

LGPL compliance for `Raqim.exe`: LAME is linked into separate DLLs (`raqim_engine_x64.dll`, `raqim_engine_avx2.dll`) that the program extracts to `%LOCALAPPDATA%\Raqim` and loads at run time; they can be rebuilt from `native/` and `encoders/` with `native/build_windows.sh`.
