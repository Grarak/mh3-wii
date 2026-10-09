# Guest functions that address guest RAM through another alias than cached
# MEM1/MEM2 (uncached 0xC.../0xD..., physical, the locked cache 0xE...), one
# 0xADDRESS per line: the translator gives them MemoryAlias helpers, which fold
# every alias into the runtime's window (recomp.yml translation.alias_access_path).
# Functions naming such an address as a constant are found by the translator
# itself (GuestHardwareAccess.NamesAliasAddress); this list holds the ones a
# run saw with MH3_AUDIT_GUEST_ALIAS (2026-10-05: boot through the title, the
# village, Alatreon 19001, arena 9004, quest 1003 area 2).
0x804CA6B0 # uncached MEM1 globals (0xC00031xx)
0x804CAD90 # uncached MEM1
0x804CB040 # uncached MEM1
