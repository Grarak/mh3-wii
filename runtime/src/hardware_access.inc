# Translated functions that access hardware (MMIO at 0xCC000000..0xCDFFFFFF,
# the EFB at 0xC8000000). Their loads and stores go through MemoryHw, which
# sends a hardware address to the device models; every other function uses
# plain flat guest memory, and a hardware access there faults with a report
# naming the function (runtime/src/guest_flat_memory.cpp). Add it here then.
# Sources: the slow-path trace sweep (docs/optimizations/cpu/flat-guest-memory.md) and a static
# scan for hardware base loads.
# A callee that the C++ compiler may inline into a listed function is listed
# too: the trace attributes its accesses to the caller.
0x80047DD0  # GX FIFO
0x800531C4  # GX FIFO
0x800545E4  # GX FIFO
0x80055FBC  # GX FIFO
0x80058870  # GX FIFO
0x80085758  # GX FIFO
0x800BC48C  # GX FIFO
0x800BD298  # GX FIFO
0x800BE424  # GX FIFO
0x800BF87C  # GX FIFO
0x800C3DEC  # GX FIFO
0x800D10B0  # GX FIFO DrawHud
0x801F9D38  # static C800
0x801FA32C  # static C800
0x8046DC00  # AI
0x8046DD80  # static CC00
0x8046DEA0  # static CD00
0x804777E0  # SDK callee of 0x804CE0F0, inlined into it
0x804A5890  # static CC00
0x804B0640  # static CD00 EXIImm
0x804B0970  # static CD00 EXIDma
0x804B0A70  # static CD00 EXISync
0x804B0F00  # static CD00
0x804B10E0  # static CD00 EXISelect
0x804B1210  # static CD00 EXIDeselect
0x804B1310  # static CD00 EXIIntrruptHandler
0x804B13D0  # static CD00 TCIntrruptHandler
0x804B3D10  # CP GXInit
0x804B4220  # GX FIFO __GXInitGX; PE __GXInitGX
0x804B4DD0  # CP GXSetCPUFifo; PI GXSetCPUFifo
0x804B4FB0  # CP GXSetGPFifo
0x804B5AD0  # SDK callee of 0x80058870, inlined into it
0x804B6400  # SDK callee of 0x80058870, inlined into it
0x804B64B0  # static CC00
0x804B6620  # static CC00, CC01, GX_HLE_FIFO calls
0x804B68E0  # SDK callee of 0x804B4220, inlined into it
0x804B68F0  # SDK callee of 0x804B4220, inlined into it
0x804B6910  # PE
0x804B6930  # PE
0x804B6990  # PE
0x804B69B0  # SDK callee of 0x804B4220, inlined into it
0x804B69D0  # PE
0x804B69F0  # PE
0x804B6A10  # static C800
0x804B6A30  # static C800
0x804B6C00  # PE __GXPEInit
0x804B71E0  # SDK callee of 0x804B4220, inlined into it
0x804B7210  # SDK callee of 0x804B4220, inlined into it
0x804B75A0  # SDK callee of 0x804B4220, inlined into it
0x804B75C0  # SDK callee of 0x804B4220, inlined into it
0x804B7B60  # SDK callee of 0x804B4220, inlined into it
0x804B7E20  # SDK callee of 0x804B4220, inlined into it
0x804B82C0  # GX FIFO
0x804B8500  # SDK callee of 0x804B4220, inlined into it
0x804B8FF0  # SDK callee of 0x804B4220, inlined into it
0x804B9010  # SDK callee of 0x804B4220, inlined into it
0x804B9900  # SDK callee of 0x804B4220, inlined into it
0x804BA5A0  # SDK callee of 0x804B4220, inlined into it
0x804BA5D0  # SDK callee of 0x804B4220, inlined into it
0x804BA720  # SDK callee of 0x804B4220, inlined into it
0x804BA780  # SDK callee of 0x804B4220, inlined into it
0x804BAA40  # GX FIFO
0x804BAB90  # GX FIFO
0x804BAC10  # GX FIFO
0x804BACB0  # SDK callee of 0x80058870, inlined into it
0x804BACD0  # GX FIFO
0x804BAFB0  # SDK callee of 0x804B4220, inlined into it
0x804BB890  # CP
0x804BB940  # static CD00 IPCReadReg
0x804BB9C0  # static CD00
0x804BBC20  # static CD00 IPCInterruptHandler
0x804BBF10  # static CD00 __ios_Ipc2
0x804BC150  # static CD00
0x804BD3F0  # static CD00
0x804CB9F0  # static CD00
0x804CC7B0  # static CD80 __AIClockInit
0x804CCBA0  # static CC00 __OSStopAudioSystem
0x804CE0F0  # static CC00, CD00 __OSUnhandledException
0x804CF090  # static CC00
0x804CFD00  # static CC00
0x804CFE20  # static CD00 OSFatal
0x804D08F0  # static CC00 OSSetFontEncode
0x804D0C80  # static CC00
0x804D1060  # static CC00
0x804D17C0  # static CC00, CD00
0x804D1DF0  # static CC00
0x804D2320  # static CC00 BATConfig
0x804D6030  # static CC00
0x804D60B0  # static CC00 __OSHotReset
0x804D6320  # static CC00 __OSStateEventHandler
0x804D8AD0  # static CD00
0x804D8C60  # static CD00
0x804D8D40  # static CD00
0x804DE0A0  # static CD00
0x804DE3A0  # static CD00 SIInterruptHandler
0x804DE790  # static CD00
0x804DE9C0  # static CD00
0x804DEB70  # static CD00
0x804DEBF0  # static CD00
0x804DEC70  # static CD00
0x804DED00  # static CD00
0x804DED70  # static CD00
0x804DF630  # VI SISetSamplingRate
0x804E6F30  # VI
0x804E7900  # static CC00
0x804E7B00  # VI
0x804E9230  # static CC00
0x804E92D0  # static CC00
0x804E93C0  # VI
0x804E9420  # VI
0x804E98D0  # static CD80
0x804ED950  # Hollywood
0x805229D8  # static CC00
0x80522CE8  # static CD00
0x80522FD0  # static CD00
0x8052308C  # static CD00
0x805231B0  # static CD00
0x805232B4  # static CD00
0x80523394  # static CD00
0x80987E9C  # static C800
