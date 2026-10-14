# miniz source provenance

Upstream: <https://github.com/richgel999/miniz>

Pinned release tag: **3.1.0**. The files listed below were downloaded unmodified
from `https://raw.githubusercontent.com/richgel999/miniz/3.1.0/<filename>` on
2026-10-13. The MIT license is included in `LICENSE`, and the original upstream
README is retained in `readme.md`.

`miniz_export.h` is the only project-provided header: it defines `MINIZ_EXPORT`
as empty for static linking, equivalent to the generated static-build export
header. No system miniz install or additional DLL is required.

Compile `miniz.c`, `miniz_tdef.c`, `miniz_tinfl.c`, and `miniz_zip.c` with:

```text
MINIZ_NO_STDIO
MINIZ_NO_TIME
MINIZ_NO_ZLIB_APIS
MINIZ_NO_ZLIB_COMPATIBLE_NAMES
```

The same definitions must be visible to code consuming the headers. Qt handles
all filesystem I/O through miniz callbacks; unused stdio and zlib compatibility
APIs are omitted. The C sources may be compiled as C++ in this C++-only project;
upstream declarations include `extern "C"` guards.

SHA-256 of upstream files:

```text
0115478D567121238CF6CC1C0C361926CF07A49D9E4C9E66DA97FAC6A01646B3  LICENSE
6320FA10A5D21DB9F78594D92745BC27593C1E6BED657104C22DA0A9BD687B49  miniz_common.h
30AA8BF68EBCD6D15F03E1DB3C7E72D70A038D4A44E74E69193185EA9187ECD0  miniz_tdef.c
D62DA02953C3834DFCBDE4DB026C6D9345013EF9B0E53185131016A3A4E48C33  miniz_tdef.h
FF9CC5426DF23A78070B5B8FA2DCB7AEA68FAAC466169A883B4FFB8727532350  miniz_tinfl.c
DA4850920FDF09F8877D9AFFABE1EBBA1852CE4B1EBBD121F2BA7A7EA8E57E5D  miniz_tinfl.h
3C1F90797C02DDC9D6BFCDF7079F5EA6F6212F257ED8745F2334DD021D697669  miniz_zip.c
B062A5DEF545445C73FD1D664E35A89165DFB0D996AB848F507244E819CFCF04  miniz_zip.h
F23204EFEE2BFAFFB72A974D2E28C64C3C1D9787144B5C5890F129C465E467BD  miniz.c
232ABB0B807FD4962A8E61822459DD49BAECB61D0E557F23497732978640F6A7  miniz.h
B4EA367BE28A36E5386F65118B73962BC659A67936D248BE68B7A7C03B8D359B  readme.md
```
