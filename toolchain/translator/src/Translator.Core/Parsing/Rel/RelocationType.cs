namespace Translator.Core.Parsing.Rel;

public enum RelocationType : byte
{
    R_PPC_NONE = 0,
    R_PPC_ADDR32 = 1,
    R_PPC_ADDR24 = 2,
    R_PPC_ADDR16 = 3,
    R_PPC_ADDR16_LO = 4,
    R_PPC_ADDR16_HI = 5,
    R_PPC_ADDR16_HA = 6,
    R_PPC_ADDR14 = 7,
    R_PPC_ADDR14_BRTAKEN = 8,
    R_PPC_ADDR14_BRNTAKEN = 9,
    R_PPC_REL24 = 10,
    R_PPC_REL14 = 11,
    R_PPC_REL14_BRTAKEN = 12,
    R_PPC_REL14_BRNTAKEN = 13,
    R_RVL_NONE = 201,
    R_RVL_SECT = 202,
    R_RVL_STOP = 203,
    R_DOLPHIN_MRKREF = 204
}
