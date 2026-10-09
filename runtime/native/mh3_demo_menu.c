/* Ordinary C character-creation menu, reconstructed from80987E9C.
 * The ten-way display switch was verified from8099FEBC and80988488.
 * Scratch addresses hold typed CPU records shared with pure guest helpers. */
#include "mh3_scene_feedback.h"
#include <math.h>
#include <string.h>
static float as_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, 4);
    return value;
}

static double dbits(uint64_t bits) {
    double value;
    memcpy(&value, &bits, 8);
    return value;
}
#define R8(a) o->read(o->context, (a), 1)
#define S8(a) ((int8_t)R8(a))
#define R16(a) o->read(o->context, (a), 2)
#define S16(a) ((int16_t)R16(a))
#define R32(a) o->read(o->context, (a), 4)
#define S32(a) ((int32_t)R32(a))
#define F(a) as_float(R32(a))
#define D(a) dbits((uint64_t)R32(a) << 32 | R32((a) + 4))
#define CONCAT44(a, b) ((uint64_t)(uint32_t)(a) << 32 | (uint32_t)(b))
#define W8(a, v) o->write(o->context, (a), (v), 1)
#define W16(a, v) o->write(o->context, (a), (v), 2)
#define W32(a, v) o->write(o->context, (a), (v), 4)
#define C(fn, ...) o->call(o->context, (fn), (const uint32_t[]){__VA_ARGS__}, sizeof((const uint32_t[]){__VA_ARGS__}) / 4)
#define CP20(dst, src) o->copy(o->context, (dst), (src), 20)
#define FC(fn, value) o->float_call(o->context, (fn), 0, 0, (const float[]){(value)}, 1)
#define FP(fn, pointer, value) o->float_call(o->context, (fn), (const uint32_t[]){(pointer)}, 1, (const float[]){(value)}, 1)
/* Sampling begins before the authored wheel's draw, not after it. */
#define END_PICK(address)                                                                                                                                                                              \
    do {                                                                                                                                                                                               \
        uint8_t rgba[4];                                                                                                                                                                               \
        if (!o->sample_shape(o->context, 0, 0, rgba))                                                                                                                                                  \
            return 0;                                                                                                                                                                                  \
        W32((address), ((uint32_t)rgba[0] << 16) | ((uint32_t)rgba[1] << 8) | rgba[2]);                                                                                                                \
    } while (0)
int mh3_scene_demo_character_menu(const mh3_scene_cpu_ops *o, uint32_t scratch) {
    if (!o || !o->read || !o->write || !o->call || !o->copy || !o->float_call || !o->begin_shape_sample || !o->sample_shape)
        return 0;

    uint8_t bVar1, bVar2;
    uint32_t uVar3, uVar4, uVar9, uVar18;
    uint16_t uVar17;
    int16_t sVar6, sVar7, sVar19, sVar21, sVar24;
    int8_t cVar8;
    int32_t iVar10, iVar14, iVar20, iVar22, iVar23;
    uint32_t puVar11, pbVar12, puVar13, pbVar15, psVar16;
    size_t sVar5;
    double dVar25, dVar26;
    W32((scratch + 0x70), 0x43300000);
    W32((scratch + 0x78), 0x43300000);
    W32((scratch + 0x10), 0xffffffff);
    W32((scratch + 0x94), 0);
    C(0x80040354, R32(0x809d3568) + 0x101c);
    iVar14 = (int)((F(R32(0x809d3568) + 0x101c) - F(0x80993970)) * F(0x8099393c));
    C(0x80984a8c);
    sVar24 = (short)iVar14;

    if (S8(R32(0x809d3568) + 0x10fd) != '\0') {
        C(0x80984e10, 0x8099d990, R32(0x8099faf0 + (R8(0x8099d990)) * 4) + S16(0x8099d998) * 8, R32(0x8099d994), (int)sVar24, 0);
        sVar6 = C(0x8005b738);
        sVar7 = C(0x8005b75c);
        puVar13 = R32(0x809a7980 + (R8(0x8065aa69)) * 4);
        C(0x8005b714, 0x14, 0x14);
        C(0x8005b780, (int)(short)(sVar24 + 0xaa), 0xbe);
        C(0x8005b7ec, 0x202020ff);
        C(0x8005ba04, R32(puVar13));
        C(0x8005b714, (int)sVar6, (int)sVar7);
    }

    if (((S8(R32(0x809d3568) + 0x10fb) != '\x01') || (R8(R32(0x809d3568) + 0x10fa) == 2)) || (0xb < R8(R32(0x809d3568) + 0x10fa))) {
        if (S8(R32(0x809d3568) + 0x1150) == '\x01') {
            pbVar12 = 0x8099e710;
            iVar14 = 0;

            do {
                C(0x80984fe4, pbVar12, R32(0x8099fb20 + (R8(pbVar12)) * 4) + S16(pbVar12 + 8) * 8, R32(pbVar12 + 4), (int)sVar24, 0);
                iVar14 = iVar14 + 1;
                pbVar12 = pbVar12 + 0x14;
            } while (iVar14 < 2);
            iVar10 = (uint32_t)R8(R32(0x809d3568) + 0x10d7) * 0x14;
            puVar11 = R32(0x8099fb20 + ((uint8_t)R8(0x8099ecf4 + (iVar10) * 1)) * 4);
            sVar6 = S16(0x8099ecfc + iVar10);
            W32((scratch + 0x10), 0xfaa01400);
            W32((scratch + 0x7c), R32(R32(0x809d3568) + 0x104c));
            dVar26 = (double)FC(0x80985644, (double)((F(0x80993978) * (float)(dbits(CONCAT44(R32((scratch + 0x78)), R32((scratch + 0x7c)))) - D(0x80993960))) / F(0x8099397c)));
            iVar14 = (uint32_t)((double)(float)((double)F(0x80993974) * dVar26));
            W32((scratch + 0x10), R32((scratch + 0x10)) | iVar14 + 0x3cU & 0xff);
            C(0x80984fe4, 0x8099ecf4 + iVar10, puVar11 + sVar6 * 8, R32((scratch + 0x10)), (int)sVar24, 0);
            iVar22 = 0;
            psVar16 = 0x8099ece8;
            iVar14 = 0;

            do {
                if (S8(R32(0x809d3568) + iVar22 + 0x10d4) == '\x01') {
                    CP20((scratch + 0x30), 0x8099ecc0);
                } else {
                    CP20((scratch + 0x30), 0x8099ecd4);
                }

                W16((scratch + 0x3a), S16((scratch + 0x3a)) + S16(psVar16));
                W16((scratch + 0x3c), S16((scratch + 0x3c)) + S16(psVar16 + (1) * 2));
                C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, R32(0x8099ecf8 + iVar10), (int)sVar24, 0);
                sVar6 = S16(psVar16);
                sVar7 = S16(psVar16 + (1) * 2);

                if (S8(R32(0x809d3568) + iVar22 + 0x10d4) == '\x01') {
                    pbVar12 = R32(R32(0x809d3568) + iVar14 + 0x10c8);
                    iVar20 = 0;
                    puVar11 = 0x8099ed30;

                    do {
                        if ((iVar20 != 3) || (S16(pbVar12 + 0x3de4) != 0)) {
                            CP20((scratch + 0x30), puVar11);
                            W16((scratch + 0x3a), S16((scratch + 0x3a)) + sVar6);
                            W16((scratch + 0x3c), S16((scratch + 0x3c)) + sVar7);
                            C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, R32((scratch + 0x34)), (int)sVar24, 0);
                        }

                        puVar11 = puVar11 + 0x14;
                        iVar20 = iVar20 + 1;
                    } while (iVar20 < 4);
                    sVar6 = sVar6 + sVar24;
                    sVar5 = C(0x80456c40, (pbVar12 + 3));
                    sVar19 = sVar6 + 0x52 + (short)((sVar5 * -10 + 0x86) >> 1);
                    C(0x8005b714, 0x14, 0x14);
                    C(0x8005b780, sVar19 + 2, (short)(sVar7 + 0xb) + 2);
                    C(0x8005b7ec, 0xff);
                    C(0x8005bab4, pbVar12 + 3);
                    C(0x8005b780, (int)sVar19, (int)(short)(sVar7 + 0xb));
                    C(0x8005b7ec, R32(0x8099eda0));
                    C(0x8005bab4, pbVar12 + 3);
                    C(0x8005b714, 0x12, 0x12);
                    sVar21 = S16(0x8099ed88);
                    sVar19 = S16(0x8099ed86);
                    iVar23 = (int)S16(0x8099ed8a);
                    puVar11 = R32(0x809a7b00 + (R8(0x8065aa69)) * 4);
                    iVar20 = C(0x8005b8d8, R32(puVar11 + (uint32_t)R8(pbVar12) * 4));
                    sVar19 = sVar19 + (short)((iVar23 + iVar20 * -9) / 2) + sVar6;
                    C(0x8005b780, sVar19 + 2, (short)(sVar21 + sVar7) + 2);
                    C(0x8005b7ec, 0xff);
                    C(0x8005ba04, R32(puVar11 + (uint32_t)R8(pbVar12) * 4));
                    C(0x8005b780, (int)sVar19, (int)(short)(sVar21 + sVar7));
                    C(0x8005b7ec, R32(0x8099eda4 + (uint32_t)R8(pbVar12) * 4));
                    C(0x8005ba04, R32(puVar11 + (uint32_t)R8(pbVar12) * 4));
                    iVar20 = S32(pbVar12 + 0x1c);
                    iVar23 = iVar20 / 0xe10 + (iVar20 >> 0x1f);
                    iVar23 = iVar23 - (iVar23 >> 0x1f);
                    iVar20 = iVar20 + iVar23 * -0xe10;
                    iVar20 = iVar20 / 0x3c + (iVar20 >> 0x1f);
                    C(0x8045e54c, (scratch + 0x20), R32(0x8099edd8), iVar23, iVar20 - (iVar20 >> 0x1f));
                    sVar19 = S16(0x8099ed8c) + sVar6;
                    sVar21 = S16(0x8099ed8e) + sVar7;
                    C(0x8005b780, sVar19 + 2, sVar21 + 2);
                    C(0x8005b7ec, 0xff);
                    C(0x8005ba04, (scratch + 0x20));
                    C(0x8005b780, (int)sVar19, (int)sVar21);
                    C(0x8005b7ec, R32(0x8099eda0));
                    C(0x8005ba04, (scratch + 0x20));
                    C(0x8045e54c, (scratch + 0x20), R32(0x8099eddc), R32(pbVar12 + 0x18));
                    sVar19 = S16(0x8099ed92) + sVar6;
                    sVar21 = S16(0x8099ed94) + sVar7;
                    C(0x8005b780, sVar19 + 2, sVar21 + 2);
                    C(0x8005b7ec, 0xff);
                    C(0x8005ba04, (scratch + 0x20));
                    C(0x8005b780, (int)sVar19, (int)sVar21);
                    C(0x8005b7ec, R32(0x8099eda0));
                    C(0x8005ba04, (scratch + 0x20));

                    if (S16(pbVar12 + 0x3de4) != 0) {
                        C(0x8045e54c, (scratch + 0x20), 0x8099fe90);
                        sVar6 = S16(0x8099ed98) + sVar6;
                        sVar7 = S16(0x8099ed9a) + sVar7;
                        C(0x8005b780, sVar6 + 2, sVar7 + 2);
                        C(0x8005b7ec, 0xff);
                        C(0x8005ba04, (scratch + 0x20));
                        C(0x8005b780, (int)sVar6, (int)sVar7);
                        C(0x8005b7ec, R32(0x8099edac));
                        C(0x8005ba04, (scratch + 0x20));
                    }
                } else {
                    puVar13 = R32(0x809a7ae8 + (R8(0x8065aa69)) * 4);
                    C(0x8005b714, 0x16, 0x16);

                    if (S8(R32(0x809d3568) + iVar22 + 0x10d4) == -1) {
                        iVar20 = C(0x8005b8d8, R32(puVar13));
                        C(0x8005b780, (int)sVar6 + (int)sVar24 + (iVar20 * -0xb + 0x9a) / 2 + 0x48, sVar7 + 0x28);
                        C(0x8005b7ec, 0x5a1400ff);
                        C(0x8005ba04, R32(puVar13));
                    } else {
                        iVar20 = C(0x8005b8d8, R32(puVar13 + (1) * 4));
                        C(0x8005b780, (int)sVar6 + (int)sVar24 + (iVar20 * -0xb + 0x9a) / 2 + 0x48, sVar7 + 0x28);
                        C(0x8005b7ec, 0xa0a0aff);
                        C(0x8005ba04, R32(puVar13 + (1) * 4));
                    }
                }

                psVar16 = psVar16 + 4;
                iVar14 = iVar14 + 4;
                iVar22 = iVar22 + 1;
            } while (iVar22 < 3);
            puVar11 = R32(0x809a7968 + (R8(0x8065aa69)) * 4);
            C(0x8005b714, 0x12, 0x12);
            C(0x8005b780, (short)(sVar24 + 0x5e) + 2, 0x186);
            C(0x8005b7ec, 0x202020ff);
            C(0x8005ba04, R32(puVar11 + (uint32_t)R8(R32(0x809d3568) + 2) * 4));
            C(0x8005b780, (int)(short)(sVar24 + 0x5e), 0x184);
            C(0x8005b7ec, 0xffffffff);
            C(0x8005ba04, R32(puVar11 + (uint32_t)R8(R32(0x809d3568) + 2) * 4));

            if (S8(R32(0x809d3568) + 2) == '\x01') {
                C(0x80987c8c, R8(R32(0x809d3568) + 0x105a), (int)sVar24);
            }
        } else {
            if (S8(R32(0x809d3568) + 0x10fb) == '\x03') {
                C(0x80984fe4, 0x8099e724, R32(0x8099fb20 + (R8(0x8099e724)) * 4) + S16(0x8099e72c) * 8, R32(0x8099e728), (int)sVar24, 0);
            } else {
                pbVar12 = 0x8099e710;
                iVar14 = 0;

                do {
                    if (((iVar14 == 3) && (S8(R32(0x809d3568) + 0x10fa) == '\f')) || ((iVar14 == 2 && (S8(R32(0x809d3568) + 0x10fa) == '\x01')))) {
                        CP20((scratch + 0x30), pbVar12);
                        W16((scratch + 0x38), 2);
                        C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + 0x10, R32((scratch + 0x34)), (int)sVar24, 0);
                    } else {
                        C(0x80984fe4, pbVar12, R32(0x8099fb20 + (R8(pbVar12)) * 4) + S16(pbVar12 + 8) * 8, R32(pbVar12 + 4), (int)sVar24, 0);
                    }

                    iVar14 = iVar14 + 1;
                    pbVar12 = pbVar12 + 0x14;
                } while (iVar14 < 4);

                if (S8(R32(0x809d3568) + 0x10fa) == '\x02') {
                    CP20((scratch + 0x30), 0x8099e774);
                } else {
                    CP20((scratch + 0x30), 0x8099e788);
                }

                C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, R32((scratch + 0x34)), (int)sVar24, 0);
                CP20((scratch + 0x30), (uint32_t)R8(R32(0x809d3568) + 0x10e1) * 0x14 + -0x7f661860);
                C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, R32((scratch + 0x34)), (int)sVar24, 0);
                iVar14 = 0;
                psVar16 = 0x8099e8d4;

                do {
                    if ((R8(R32(0x809d3568) + 0x10fa) < 3) || (iVar14 != (int32_t)R8(R32(0x809d3568) + 0x10fa) - 3)) {
                        CP20((scratch + 0x30), 0x8099e804);
                        puVar11 = R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8;
                        W16((scratch + 0x3a), S16(psVar16));
                        W16((scratch + 0x3c), S16(psVar16 + (1) * 2));
                    } else {
                        CP20((scratch + 0x30), 0x8099e7f0);
                        puVar11 = R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8;
                        W16((scratch + 0x3a), S16(psVar16) + 0xc);
                        W16((scratch + 0x3c), S16(psVar16 + (1) * 2) + -1);
                    }

                    C(0x80984fe4, (scratch + 0x30), puVar11, R32((scratch + 0x34)), (int)sVar24, 0);
                    psVar16 = psVar16 + 4;
                    iVar14 = iVar14 + 1;
                } while (iVar14 < 9);
                puVar11 = 0x8099e980;
                uVar3 = 0;
                iVar14 = 0;
                psVar16 = 0x8099e8f8;

                do {
                    uVar18 = 0;
                    CP20((scratch + 0x30), puVar11);

                    if (((uVar3 == 10) && (S8(R32(0x809d3568) + 0x10fa) == '\f')) || ((uVar3 == 0 && (S8(R32(0x809d3568) + 0x10fa) == '\x01')))) {
                        W16((scratch + 0x38), S16((scratch + 0x38)) + 1);
                    }

                    W16((scratch + 0x3a), S16(psVar16));
                    W16((scratch + 0x3c), S16(psVar16 + (1) * 2));

                    if (((2 < R8(R32(0x809d3568) + 0x10fa)) && (uVar3 != 10)) && (uVar3 + 2 == (uint32_t)R8(R32(0x809d3568) + 0x10fa))) {
                        W16((scratch + 0x3a), S16((scratch + 0x3a)) + 0xc);
                    }

                    if ((uVar3 == 0) || (uVar3 == 10)) {
                        W32((scratch + 0x10), R32((scratch + 0x34)));
                    } else if ((R8(R32(0x809d3568) + 0x10fa) < 3) || (uVar3 + 2 != (uint32_t)R8(R32(0x809d3568) + 0x10fa))) {
                        W32((scratch + 0x10), 0xd4c894ff);
                    } else {
                        W32((scratch + 0x10), 0xff9000ff);
                        uVar18 = 1;
                    }

                    C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, R32((scratch + 0x10)), (int)sVar24, 0);
                    iVar10 = (int)S16(psVar16);
                    iVar22 = (int)S16(psVar16 + (1) * 2);

                    if (((2 < R8(R32(0x809d3568) + 0x10fa)) && (uVar3 != 10)) && (uVar3 + 2 == (uint32_t)R8(R32(0x809d3568) + 0x10fa))) {
                        iVar10 = (int)(short)(S16(psVar16) + 0xc);
                    }

                    if (uVar3 < 10) {

                        switch (iVar14) {
                        case 0:
                            C(0x80987acc, iVar10, iVar22, R32(0x809d3568) + 0x10d8);
                            break;
                        case 8:
                            C(0x809879a0, iVar10, iVar22, R8(R32(0x809d3568) + 0x10e6), uVar18, 0);
                            break;
                        case 0xc:
                            W32((scratch + 0x10), (uint32_t)R8(R32(0x809d3568) + 0x10e7) << 0x18 | (uint32_t)R8(R32(0x809d3568) + 0x10e8) << 0x10 | (uint32_t)R8(R32(0x809d3568) + 0x10e9) << 8 | 0xff);
                            C(0x80987b68);
                            break;
                        case 0x10:
                            C(0x809879a0, iVar10, iVar22, R8(R32(0x809d3568) + 0x10ee), uVar18, 0);
                            break;
                        case 0x14:
                            C(0x809879a0, iVar10, iVar22, R8(R32(0x809d3568) + 0x10ef), uVar18, 0);
                            break;
                        case 0x18:
                            C(0x809879a0, iVar10, iVar22, R8(R32(0x809d3568) + 0x10f0), uVar18, 1);
                            break;
                        case 0x1c:
                            C(0x809879a0, iVar10, iVar22, R8(R32(0x809d3568) + 0x10f1), uVar18, 0);
                            break;
                        case 0x20:
                            W32((scratch + 0x10), (uint32_t)R8(R32(0x809d3568) + 0x10f2) << 0x18 | (uint32_t)R8(R32(0x809d3568) + 0x10f3) << 0x10 | (uint32_t)R8(R32(0x809d3568) + 0x10f4) << 8 | 0xff);
                            C(0x80987b68);
                            break;
                        case 0x24:
                            C(0x809879a0, iVar10, iVar22, R8(R32(0x809d3568) + 0x10f9), uVar18, 0);
                        }
                    }

                    psVar16 = psVar16 + 4;
                    uVar3 = uVar3 + 1;
                    iVar14 = iVar14 + 4;
                    puVar11 = puVar11 + 0x14;
                } while ((int)uVar3 < 0xb);

                if ((R8(R32(0x809d3568) + 0x10fa) + 0xff & 0xff) < 0xc) {
                    iVar14 = R8(R32(0x809d3568) + 0x10fa) - 1;
                    C(0x8098538c, (int)S16(0x8099e928 + iVar14 * 4), (int)S16(0x8099e928 + (iVar14 * 2 + 1) * 2));
                }

                if ((S8(R32(0x809d3568) + 0x10fa) == '\x02') && (S8(R32(0x809d3568) + 0x10fb) == '\x01')) {
                    puVar11 = 0x8099e7c8;
                    iVar14 = 0;
                    dVar26 = D(0x80993968);

                    do {
                        if (iVar14 == 0) {
                            bVar1 = R8(R32(0x809d3568) + 0x10e2);
                        } else {
                            bVar1 = R8(R32(0x809d3568) + 0x10e3);
                        }

                        uVar17 = (uint16_t)bVar1;
                        W32((scratch + 0x74), (int)(short)uVar17 ^ 0x80000000);
                        dVar25 = (double)FP(0x80052364, 0x8099e958, (double)(float)(dbits(CONCAT44(R32((scratch + 0x70)), (int)(short)uVar17) ^ 0x80000000) - dVar26));
                        CP20((scratch + 0x30), puVar11);

                        if (uVar17 == 0) {
                            W16((scratch + 0x38), S16((scratch + 0x38)) + 2);
                        }

                        sVar6 = (short)(int)dVar25;

                        if (iVar14 != 0) {
                            sVar6 = -sVar6;
                        }

                        W16((scratch + 0x3a), S16((scratch + 0x3a)) + sVar6);
                        C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, R32((scratch + 0x34)), (int)sVar24, 0);
                        iVar14 = iVar14 + 1;
                        puVar11 = puVar11 + 0x14;
                    } while (iVar14 < 2);
                }
            }

            uVar3 = (uint32_t)R8(0x8065aa69);
            puVar11 = R32(0x809a7a88 + (uVar3) * 4);
            uVar9 = (uint32_t)R8(R32(0x809d3568) + 0x10fa);

            if (uVar9 != 0) {
                sVar6 = sVar24 + 0x5e;
                cVar8 = S8(R32(0x809d3568) + 0x10fb);

                if ((cVar8 == '\x01') && (uVar9 == 2)) {
                    C(0x8005b780, sVar6 + 2, 0x186);
                    C(0x8005b7ec, 0x202020ff);
                    C(0x8005ba04, R32(puVar11 + 4));
                    C(0x8005b780, (int)sVar6, 0x184);
                    C(0x8005b7ec, 0xffffffff);
                    C(0x8005ba04, R32(puVar11 + 4));
                    C(0x809878b0, 0);
                } else {
                    if (cVar8 == '\x02') {
                        iVar14 = 0;
                    } else {
                        if (cVar8 == '\x03') {
                            puVar11 = R32(0x809a7aa0 + (uVar3) * 4);
                            C(0x8005b780, sVar6 + 2, 0x186);
                            C(0x8005b7ec, 0x202020ff);
                            C(0x8005ba04, R32(puVar11 + 0xc));
                            C(0x8005b780, (int)sVar6, 0x184);
                            C(0x8005b7ec, 0xffffffff);
                            C(0x8005ba04, R32(puVar11 + 0xc));
                            goto code_r0x80989cac;
                        }

                        if (cVar8 != '\x04') {
                            puVar11 = R32(0x809a7a70 + (uVar3) * 4);
                            C(0x8005b780, sVar6 + 2, 0x186);
                            C(0x8005b7ec, 0x202020ff);
                            iVar14 = (uVar9 - 1) * 4;
                            C(0x8005ba04, R32(puVar11 + iVar14));
                            C(0x8005b780, (int)sVar6, 0x184);
                            C(0x8005b7ec, 0xffffffff);
                            C(0x8005ba04, R32(puVar11 + iVar14));
                            C(0x809878b0, 0);
                            goto code_r0x80989cac;
                        }

                        iVar14 = 4;
                    }

                    puVar11 = R32(0x809a7aa0 + (uVar3) * 4);
                    C(0x8005b780, sVar6 + 2, 0x186);
                    C(0x8005b7ec, 0x202020ff);
                    C(0x8005ba04, R32(puVar11 + iVar14 * 4));
                    C(0x8005b780, (int)sVar6, 0x184);
                    C(0x8005b7ec, 0xffffffff);
                    C(0x8005ba04, R32(puVar11 + iVar14 * 4));
                    C(0x80987c8c, R8(R32(0x809d3568) + 0x10fc), (int)sVar24);
                }
            }
        }

        goto code_r0x80989cac;
    }

    pbVar12 = 0x8099e710;
    iVar14 = 0;

    do {
        C(0x80984fe4, pbVar12, R32(0x8099fb20 + (R8(pbVar12)) * 4) + S16(pbVar12 + 8) * 8, R32(pbVar12 + 4), (int)sVar24, 0);
        iVar14 = iVar14 + 1;
        pbVar12 = pbVar12 + 0x14;
    } while (iVar14 < 2);
    uVar3 = (uint32_t)R8(R32(0x809d3568) + 0x10fa);

    if (uVar3 - 6 < 4) {
    code_r0x80988a90:
        pbVar12 = 0x8099ea60;
        iVar14 = 0;

        do {
            C(0x80984fe4, pbVar12, R32(0x8099fb20 + (R8(pbVar12)) * 4) + S16(pbVar12 + 8) * 8, R32(pbVar12 + 4), (int)sVar24, 0);
            iVar14 = iVar14 + 1;
            pbVar12 = pbVar12 + 0x14;
        } while (iVar14 < 2);
    } else if (uVar3 == 0) {
        if (S8(R32(0x809d3568) + 0x10fd) == '\x01') {
            C(0x80984e10, 0x8099d9e4, R32(0x8099faf0 + (R8(0x8099d9e4)) * 4) + S16(0x8099d9ec) * 8, R32(0x8099d9e8), (int)sVar24, 0);
            C(0x800e46ec, 0, 0, 0);
            C(0x800e45d8, 4, 5, 1);
            cVar8 = C(0x803becd4, 7);

            if (cVar8 == '\0') {
                C(0x802e0fc0, (int)(short)(sVar24 + 0x1e0), 0x118, 0x20, 0xffffffff, 0, 0);
            } else {
                C(0x802e0fc0, (int)(short)(sVar24 + 0x1e0), 0x118, 0x20, 0xffffffff, 2, 0);
            }

            sVar6 = C(0x8005b738);
            sVar7 = C(0x8005b75c);
            puVar13 = R32(0x809a7980 + (R8(0x8065aa69)) * 4);
            C(0x8005b714, 0x14, 0x14);
            C(0x8005b780, sVar24 + 100, 200);
            C(0x8005b7ec, 0xff);
            C(0x8005ba04, R32(puVar13));
            C(0x8005b714, (int)sVar6, (int)sVar7);
        }
    } else if ((uVar3 == 4) || (uVar3 == 0xb))
        goto code_r0x80988a90;

    cVar8 = S8(R32(0x809d3568) + 0x10fa);

    switch (cVar8) {
    case '\x03':
        pbVar12 = 0x8099eaf8;
        iVar14 = 0;

        do {
            C(0x80984fe4, pbVar12, R32(0x8099fb20 + (R8(pbVar12)) * 4) + S16(pbVar12 + 8) * 8, R32(pbVar12 + 4), (int)sVar24, 0);
            iVar14 = iVar14 + 1;
            pbVar12 = pbVar12 + 0x14;
        } while (iVar14 < 4);
        CP20((scratch + 0x30), 0x8099eb48);
        W32((scratch + 0x7c), (int)S8(R32(0x809d3568) + 0x10e4) ^ 0x80000000);
        W32((scratch + 0x74), (int)S8(R32(0x809d3568) + 0x10e5) ^ 0x80000000);
        iVar14 = (int)(F(0x80993990) * (float)(dbits(CONCAT44(R32((scratch + 0x78)), (int)S8(R32(0x809d3568) + 0x10e4)) ^ 0x80000000) - D(0x80993968)) +
                       F(0x80993990) * ((float)(dbits(CONCAT44(R32((scratch + 0x70)), (int)S8(R32(0x809d3568) + 0x10e5)) ^ 0x80000000) - D(0x80993968)) / F(0x80993950)));
        W16((scratch + 0x3a), S16((scratch + 0x3a)) + (short)iVar14 + 0x5d);
        W16((scratch + 0x3c), S16((scratch + 0x3c)) + 0xdb);
        C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, R32((scratch + 0x34)), (int)sVar24, 0);
        CP20((scratch + 0x30), 0x8099e994);
        W32((scratch + 0x10), 0xff9000ff);
        W16((scratch + 0x3a), 0x59);
        W16((scratch + 0x3c), 0xb0);
        C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, 0xff9000ff, (int)sVar24, 0);
        goto code_r0x80989584;
    case '\x04':
        iVar14 = 4;
        uVar3 = (uint32_t)S8(R32(0x809d3568) + 0x10e6);
        break;
    case '\x05':
    case '\n':
        if (cVar8 == '\x05') {
            pbVar12 = (R32(0x809d3568) + 0x10e7);
        } else {
            if (cVar8 != '\n')
                goto code_r0x80989584;

            pbVar12 = (R32(0x809d3568) + 0x10f2);
        }

        if (R8(pbVar12 + 4) == 1)
            o->begin_shape_sample(o->context, (int16_t)(S16(0x8099eba2) + R8(pbVar12 + 5)), (int16_t)(S16(0x8099eba4) + R8(pbVar12 + 6)));

        bVar1 = R8(pbVar12 + (3) * 1);
        W32((scratch + 0x94), (uint32_t)R8(pbVar12 + (4) * 1));
        W16((scratch + 0x90), (uint16_t)R8(pbVar12 + (5) * 1));
        bVar2 = R8(pbVar12 + (6) * 1);
        pbVar15 = 0x8099eb5c;
        iVar14 = 0;

        do {
            if ((iVar14 == 2) && (bVar1 == 0xc)) {
                CP20((scratch + 0x30), 0x8099ebc0);
                W32((scratch + 0x10), R32((scratch + 0x34)));
                C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, R32((scratch + 0x34)), (int)sVar24, 0);
            }

            puVar11 = R32(0x8099fb20 + (R8(pbVar15)) * 4);
            sVar6 = S16(pbVar15 + 8);
            W32((scratch + 0x10), R32(pbVar15 + 4));
            CP20((scratch + 0x30), pbVar15);

            if (iVar14 == 1) {
                W32((scratch + 0x10), (uint32_t)R8(pbVar12) << 0x18 | (uint32_t)R8(pbVar12 + (1) * 1) << 0x10 | (uint32_t)R8(pbVar12 + (2) * 1) << 8 | 0xff);
                W16((scratch + 0x3a), 299);
                W16((scratch + 0x3c), 0x5d);
            }

            C(0x80984fe4, (scratch + 0x30), puVar11 + sVar6 * 8, R32((scratch + 0x10)), (int)sVar24, 0);
            iVar14 = iVar14 + 1;
            pbVar15 = pbVar15 + 0x14;
        } while (iVar14 < 5);

        if (S8(R32(0x809d3568) + 0x10fa) == '\x05') {
            pbVar15 = 0x8099e9bc;
        } else {
            if (S8(R32(0x809d3568) + 0x10fa) != '\n')
                goto code_r0x80989584;

            pbVar15 = 0x8099ea20;
        }

        CP20((scratch + 0x30), pbVar15);
        W32((scratch + 0x10), 0xff9000ff);
        W16((scratch + 0x3a), 0x41);
        W16((scratch + 0x3c), 0x52);
        C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8(pbVar15)) * 4) + S16(pbVar15 + 8) * 8, 0xff9000ff, (int)sVar24, 0);
        uVar3 = 0;
        psVar16 = 0x8099ec28;

        do {
            sVar6 = S16(psVar16);
            sVar7 = S16(psVar16 + (1) * 2);

            if (uVar3 == bVar1) {
                CP20((scratch + 0x30), 0x8099ebd8);
                W16((scratch + 0x3a), sVar6);
                W16((scratch + 0x3c), sVar7);
                C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, R32((scratch + 0x34)), (int)sVar24, 0);
                CP20((scratch + 0x30), 0x8099ebec);
                uVar18 = 0xf0f0f0ff;
            } else {
                CP20((scratch + 0x30), 0x8099ec00);
                uVar18 = 0xd2d2d2ff;
            }

            W16((scratch + 0x3a), sVar6);
            W16((scratch + 0x3c), sVar7);
            C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, R32((scratch + 0x34)), (int)sVar24, 0);
            CP20((scratch + 0x30), 0x8099ec14);
            W16((scratch + 0x3a), sVar6 + 5);
            W16((scratch + 0x3c), sVar7 + 6);
            puVar11 = R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4);
            iVar14 = (int)S16((scratch + 0x38));

            if (S8(R32(0x809d3568) + 0x10fa) == '\x05') {
                C(0x801fb2a4, uVar3 & 0xff, (scratch + 0x10), (scratch + 0xc), (scratch + 0xa), (scratch + 0x8));
            } else {
                C(0x801fb23c, uVar3 & 0xff, (scratch + 0x10), (scratch + 0xc), (scratch + 0xa), (scratch + 0x8));
            }

            C(0x80984fe4, (scratch + 0x30), puVar11 + iVar14 * 8, R32((scratch + 0x10)), (int)sVar24, 0);
            W16((scratch + 0x14), 0x33);
            W16((scratch + 0x16), 6);
            W16((scratch + 0x18), sVar6);
            W16((scratch + 0x1a), sVar7);
            uVar4 = C(0x80211e80, 0, R16((scratch + 0xc) + 0));
            C(0x802e2104, (scratch + 0x14), 0x12, 0x12, uVar18, uVar4, 5, (scratch + 0x18));
            psVar16 = psVar16 + 4;
            uVar3 = uVar3 + 1;
        } while ((int)uVar3 < 0xc);

        if (R32((scratch + 0x94)) == 1) {
            CP20((scratch + 0x30), 0x8099ec58);
            uVar3 = (uint32_t)((float)(dbits(CONCAT44(R32((scratch + 0x78)), (int)(short)(S16(0x8099eba2) + R16((scratch + 0x90)))) ^ 0x80000000) - D(0x80993968)) *
                               ((float)(dbits(CONCAT44(R32((scratch + 0x70)), (int)R16(0x8065b4bc)) ^ 0x80000000) - D(0x80993968)) / F(0x8065b4c0)));
            uVar9 = (uint32_t)(short)(S16(0x8099eba4) + (uint16_t)bVar2);
            W32((scratch + 0x7c), uVar9 ^ 0x80000000);
            W32((scratch + 0x74), (int)R16(0x8065b4be) ^ 0x80000000);
            uVar9 = (uint32_t)((float)(dbits(CONCAT44(R32((scratch + 0x78)), uVar9) ^ 0x80000000) - D(0x80993968)) *
                               ((float)(dbits(CONCAT44(R32((scratch + 0x70)), (int)R16(0x8065b4be)) ^ 0x80000000) - D(0x80993968)) / F(0x8065b4c4)));
            END_PICK((scratch + 0x10));
            W8(pbVar12, (uint8_t)(R32((scratch + 0x10)) >> 0x10));
            W8(pbVar12 + (1) * 1, (uint8_t)(R32((scratch + 0x10)) >> 8));
            W8(pbVar12 + (2) * 1, (uint8_t)R32((scratch + 0x10)));
            W16((scratch + 0x3a), (S16(0x8099eba2) + R16((scratch + 0x90))) - S16((scratch + 0x3e)) / 2);
            W16((scratch + 0x3c), (S16(0x8099eba4) + (uint16_t)bVar2) - S16((scratch + 0x40)) / 2);
            puVar11 = R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4);
            W32((scratch + 0x10), R32((scratch + 0x10)) << 8 | 0xff);
            C(0x80984a8c);
            C(0x80984fe4, (scratch + 0x30), puVar11 + S16((scratch + 0x38)) * 8, R32((scratch + 0x34)), (int)sVar24, 0);
        }

        goto code_r0x80989584;
    case '\x06':
        iVar14 = 9;
        uVar3 = (uint32_t)S8(R32(0x809d3568) + 0x10ee);
        break;
    case '\a':
        iVar14 = 10;
        uVar3 = (uint32_t)R8(R32(0x809d3568) + 0x10ef);
        break;
    case '\b':
        iVar14 = 0xd;
        uVar3 = (uint32_t)S8(R32(0x809d3568) + 0x10f0);
        break;
    case '\t':
        iVar14 = 0xb;
        uVar3 = (uint32_t)S8(R32(0x809d3568) + 0x10f1);
        break;
    case '\v':
        iVar14 = 0x14;
        uVar3 = (uint32_t)S8(R32(0x809d3568) + 0x10f9);
        break;
    default:
        goto code_r0x80989584;
    }

    uVar9 = uVar3 - 7 & -(uint32_t)(7 < uVar3) - ((int)uVar3 >> 0x1f);
    iVar10 = uVar3 - uVar9;
    W32((scratch + 0x98), uVar9);
    C(0x8005b714, 0x12, 0x12);
    iVar22 = 0;
    psVar16 = 0x8099ead8;

    do {
        if (iVar14 <= (int)uVar9)
            break;

        sVar6 = S16(psVar16);
        sVar7 = S16(psVar16 + (1) * 2);
        CP20((scratch + 0x30), 0x8099ea88);
        W16((scratch + 0x3a), S16((scratch + 0x3a)) + sVar6);
        W16((scratch + 0x3c), S16((scratch + 0x3c)) + sVar7);
        W32((scratch + 0x10), 0xb6ba93ff);

        if (iVar22 == iVar10) {
            W32((scratch + 0x10), 0xfaa014ff);
        }

        C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, R32((scratch + 0x10)), (int)sVar24, 0);
        CP20((scratch + 0x30), 0x8099ea9c);
        W16((scratch + 0x3a), S16((scratch + 0x3a)) + sVar6);
        W16((scratch + 0x3c), S16((scratch + 0x3c)) + sVar7);
        W32((scratch + 0x10), 0x564836ff);

        if (iVar22 == iVar10) {
            W32((scratch + 0x10), 0x30281eff);
        }

        C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, R32((scratch + 0x10)), (int)sVar24, 0);
        puVar13 = R32(0x809a7860 + (R8(0x8065aa69)) * 4);
        iVar20 = 0;

        do {
            if (iVar20 == 0) {
                C(0x8005b780, sVar6 + 0x20, sVar7 + 4);
                C(0x8005b7ec, 0x202020ff);
            } else {
                C(0x8005b780, sVar6 + 0x1e, sVar7 + 2);

                if (iVar22 == iVar10) {
                    C(0x8005b7ec, 0xf0f0f0ff);
                } else {
                    C(0x8005b7ec, 0xd0d0d0ff);
                }
            }

            if (S8(R32(0x809d3568) + 0x10fa) == '\b') {
                if (uVar9 == 0) {
                    C(0x8005ba04, R32(puVar13 + (1) * 4));
                } else {
                    C(0x8005ba04, R32(puVar13));
                    C(0x8045e54c, (scratch + 0x20), 0x8099fe94, uVar9);
                    C(0x8005ba04, (scratch + 0x20));
                }
            } else {
                C(0x8005ba04, R32(puVar13));
                C(0x8045e54c, (scratch + 0x20), 0x8099fe94, uVar9 + 1);
                C(0x8005ba04, (scratch + 0x20));
            }

            iVar20 = iVar20 + 1;
        } while (iVar20 < 2);
        psVar16 = psVar16 + 4;
        iVar22 = iVar22 + 1;
        uVar9 = uVar9 + 1;
    } while (iVar22 < 8);
    CP20((scratch + 0x30), 0x8099e980 + (R8(R32(0x809d3568) + 0x10fa) - 2) * 0x14);
    W32((scratch + 0x10), 0xff9000ff);
    W16((scratch + 0x3a), 0x67);
    W16((scratch + 0x3c), 0x49);
    C(0x80984fe4, (scratch + 0x30), R32(0x8099fb20 + (R8((scratch + 0x30) + 0)) * 4) + S16((scratch + 0x38)) * 8, 0xff9000ff, (int)sVar24, 0);

    if (R32((scratch + 0x98)) != 0) {
        C(0x80984fe4, 0x8099eab0, R32(0x8099fb20 + (R8(0x8099eab0)) * 4) + S16(0x8099eab8) * 8, R32(0x8099eab4), (int)sVar24, 0);
    }

    if ((int)uVar9 < iVar14) {
        C(0x80984fe4, 0x8099eac4, R32(0x8099fb20 + (R8(0x8099eac4)) * 4) + S16(0x8099eacc) * 8, R32(0x8099eac8), (int)sVar24, 0);
    }
code_r0x80989584:
    if (R32((scratch + 0x94)) == 0) {
        C(0x809878b0, 0);
    }

    puVar11 = R32(0x809a7a88 + (R8(0x8065aa69)) * 4);
    bVar1 = R8(R32(0x809d3568) + 0x10fa);

    if (bVar1 != 0) {
        C(0x8005b780, (short)(sVar24 + 0x5e) + 2, 0x186);
        C(0x8005b7ec, 0x202020ff);
        iVar14 = (bVar1 - 1) * 4;
        C(0x8005ba04, R32(puVar11 + iVar14));
        C(0x8005b780, (int)(short)(sVar24 + 0x5e), 0x184);
        C(0x8005b7ec, 0xffffffff);
        C(0x8005ba04, R32(puVar11 + iVar14));
    }
code_r0x80989cac:
    return 1;
}
