# The crash report's names for the game's code (gen_symbols.py writes build/symbols.bin; symbols.cpp reads it).
    .section .rdata,"dr"
    .global _shim_symbols_blob
    .global _shim_symbols_blob_end
    .balign 4
_shim_symbols_blob:
    .incbin "build/symbols.bin"
_shim_symbols_blob_end:
