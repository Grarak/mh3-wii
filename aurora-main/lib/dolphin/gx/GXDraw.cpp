#include <aurora/legacy_gx.h>
#include "gx.hpp"

extern "C" {

static GXVtxDescList vcd[27];
static GXVtxAttrFmtList vat[27];

static void GetVertState(void) {
    GXGetVtxDescv(vcd);
    GXGetVtxAttrFmtv(GX_VTXFMT3, vat);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
}

static void RestoreVertState(void) {
    GXSetVtxDescv(vcd);
    GXSetVtxAttrFmtv(GX_VTXFMT3, vat);
}

static void vsub(f32 p1[3], f32 p2[3], f32 u[3]) {
    u32 i;

    for (i = 0; i < 3; i++) {
        u[i] = p2[i] - p1[i];
    }
}

static void vcross(f32 u[3], f32 v[3], f32 n[3]) {
    f32 n1[3];

    n1[0] = (u[1] * v[2]) - (u[2] * v[1]);
    n1[1] = (u[2] * v[0]) - (u[0] * v[2]);
    n1[2] = (u[0] * v[1]) - (u[1] * v[0]);
    n[0] = n1[0];
    n[1] = n1[1];
    n[2] = n1[2];
}

static void normalize(f32 v[3]) {
    f32 d = sqrtf((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2]));

    CHECK(d != 0.0f, "normalize: zero length vector");
    v[0] /= d;
    v[1] /= d;
    v[2] /= d;
}

static void myvertex(f32 v[3], f32 n[3]) {
    GXPosition3f32(v[0], v[1], v[2]);
    GXNormal3f32(n[0], n[1], n[2]);
}

static void DumpTriangle(f32 v0[3], f32 v1[3], f32 v2[3]) {
    GXBegin(GX_TRIANGLES, GX_VTXFMT3, 3);
    myvertex(v0, v0);
    myvertex(v1, v1);
    myvertex(v2, v2);
    GXEnd();
}

static void Subdivide(u8 depth, f32 v0[3], f32 v1[3], f32 v2[3]) {
    f32 v01[3];
    f32 v12[3];
    f32 v20[3];
    u32 i;

    if (depth == 0) {
        DumpTriangle(v0, v1, v2);
        return;
    }

    for (i = 0; i < 3; i++) {
        v01[i] = v0[i] + v1[i];
        v12[i] = v1[i] + v2[i];
        v20[i] = v2[i] + v0[i];
    }

    normalize(v01);
    normalize(v12);
    normalize(v20);
    Subdivide(depth - 1, v0, v01, v20);
    Subdivide(depth - 1, v1, v12, v01);
    Subdivide(depth - 1, v2, v20, v12);
    Subdivide(depth - 1, v01, v12, v20);
}

static void SubDivTriangle(u8 depth, u8 i, f32 (*data)[3], u8 (*ndx)[3]) {
    f32 *x0 = data[ndx[i][0]];
    f32 *x1 = data[ndx[i][1]];
    f32 *x2 = data[ndx[i][2]];

    Subdivide(depth, x0, x1, x2);
}

void GXDrawCylinder(u8 numEdges) {
  aurora_legacy_gx_panic("GXDrawCylinder");
}

void GXDrawTorus(f32 rc, u8 numc, u8 numt) {
  aurora_legacy_gx_panic("GXDrawTorus");
}

void GXDrawSphere(u8 numMajor, u8 numMinor) {
  aurora_legacy_gx_panic("GXDrawSphere");
}

static void GXDrawCubeFace(f32 nx, f32 ny, f32 nz, f32 tx, f32 ty, f32 tz, f32 bx, f32 by, f32 bz, GXAttrType binormal, GXAttrType texture) {
  aurora_legacy_gx_panic("GXDrawCubeFace");
}

void GXDrawCube(void) {
  aurora_legacy_gx_panic("GXDrawCube");
}

static u32 polygons[12][5] = {
    {  0, 12, 10, 11, 16 },
    {  1, 17,  8,  9, 13 },
    {  2, 14,  9,  8, 18 },
    {  3, 19, 11, 10, 15 },
    {  4, 14,  2,  3, 15 },
    {  5, 12,  0,  1, 13 },
    {  6, 17,  1,  0, 16 },
    {  7, 19,  3,  2, 18 },
    {  8, 17,  6,  7, 18 },
    {  9, 14,  4,  5, 13 },
    { 10, 12,  5,  4, 15 },
    { 11, 19,  7,  6, 16 },
};

static f32 verts[20][3] = {
    { -0.809015f,  0.0f,       0.309015f },
    { -0.809015f,  0.0f,      -0.309015f },
    {  0.809015f,  0.0f,      -0.309015f },
    {  0.809015f,  0.0f,       0.309015f },
    {  0.309015f, -0.809015f,  0.0f },
    { -0.309015f, -0.809015f,  0.0f },
    { -0.309015f,  0.809015f,  0 },
    {  0.309015f,  0.809015f,  0 },
    {  0.0f,       0.309015f, -0.809015f },
    {  0.0f,      -0.309015f, -0.809015f },
    {  0.0f,      -0.309015f,  0.809015f },
    {  0.0f,       0.309015f,  0.809015f },
    { -0.5f,      -0.5f,       0.5 },
    { -0.5f,      -0.5f,      -0.5 },
    {  0.5f,      -0.5f,      -0.5 },
    {  0.5f,      -0.5f,       0.5 },
    { -0.5f,       0.5f,       0.5 },
    { -0.5f,       0.5f,      -0.5 },
    {  0.5f,       0.5f,      -0.5 },
    {  0.5f,       0.5f,       0.5 },
};

void GXDrawDodeca(void) {
  aurora_legacy_gx_panic("GXDrawDodeca");
}

static f32 odata[6][3] = {
    {  1.0f,  0.0f,  0.0f },
    { -1.0f,  0.0f,  0.0f },
    {  0.0f,  1.0f,  0.0f },
    {  0.0f, -1.0f,  0.0f },
    {  0.0f,  0.0f,  1.0f },
    {  0.0f,  0.0f, -1.0f },
};

static u8 ondex[8][3] = {
    { 0, 4, 2 },
    { 1, 2, 4 },
    { 0, 3, 4 },
    { 1, 4, 3 },
    { 0, 2, 5 },
    { 1, 5, 2 },
    { 0, 5, 3 },
    { 1, 3, 5 },
};

void GXDrawOctahedron(void) {
  aurora_legacy_gx_panic("GXDrawOctahedron");
}

static f32 idata[12][3] = {
    { -0.5257311f,  0.0f,        0.8506508f },
    {  0.5257311f,  0.0f,        0.8506508f },
    { -0.5257311f,  0.0f,       -0.8506508f },
    {  0.5257311f,  0.0f,       -0.8506508f },
    {  0.0f,        0.8506508f,  0.5257311f },
    {  0.0f,        0.8506508f, -0.5257311f },
    {  0.0f,       -0.8506508f,  0.5257311f },
    {  0.0f,       -0.8506508f, -0.5257311f },
    {  0.8506508f,  0.5257311f,  0.0f },
    { -0.8506508f,  0.5257311f,  0.0f },
    {  0.8506508f, -0.5257311f,  0.0f },
    { -0.8506508f, -0.5257311f,  0.0f },
};

static u8 uindex[20][3] = {
    {  0,  4,  1 },
    {  0,  9,  4 },
    {  9,  5,  4 },
    {  4,  5,  8 },
    {  4,  8,  1 },
    {  8, 10,  1 },
    {  8,  3, 10 },
    {  5,  3,  8 },
    {  5,  2,  3 },
    {  2,  7,  3 },
    {  7, 10,  3 },
    {  7,  6, 10 },
    {  7, 11,  6 },
    { 11,  0,  6 },
    {  0,  1,  6 },
    {  6,  1, 10 },
    {  9,  0, 11 },
    {  9, 11,  2 },
    {  9,  2,  5 },
    {  7,  2, 11 },
};

void GXDrawIcosahedron(void) {
  aurora_legacy_gx_panic("GXDrawIcosahedron");
}

void GXDrawSphere1(u8 depth) {
  aurora_legacy_gx_panic("GXDrawSphere1");
}

static u32 CmpNormal32(f32 n1[3], f32 n2[3]) {
    u32 i;

    for (i = 0; i < 3; i++) {
        if (n1[i] != n2[i])
            return FALSE;
    }

    return TRUE;
}

static u32 nrm_cnt;
static f32* nrm_tab;

static void AddNormal(f32 n[3]) {
    u32 indx;
    u32 i;

    for (i = 0; i < nrm_cnt; i++) {
        if (CmpNormal32(n, &nrm_tab[i * 3]))
            return;
    }

    indx = nrm_cnt * 3;
    nrm_tab[indx + 0] = n[0];
    nrm_tab[indx + 1] = n[1];
    nrm_tab[indx + 2] = n[2];
    nrm_cnt++;
}

static void SubdivideNrm(u8 depth, f32 v0[3], f32 v1[3], f32 v2[3]) {
    f32 v01[3];
    f32 v12[3];
    f32 v20[3];
    u32 i;

    if (depth == 0) {
        AddNormal(v0);
        AddNormal(v1);
        AddNormal(v2);
        return;
    }

    for (i = 0; i < 3; i++) {
        v01[i] = v0[i] + v1[i];
        v12[i] = v1[i] + v2[i];
        v20[i] = v2[i] + v0[i];
    }

    normalize(v01);
    normalize(v12);
    normalize(v20);
    SubdivideNrm(depth - 1, v0, v01, v20);
    SubdivideNrm(depth - 1, v1, v12, v01);
    SubdivideNrm(depth - 1, v2, v20, v12);
    SubdivideNrm(depth - 1, v01, v12, v20);
}

static void SubDivNrm(u8 depth, u8 i, f32 (*data)[3], u8 (*ndx)[3]) {
    f32* x0 = data[ndx[i][0]];
    f32* x1 = data[ndx[i][1]];
    f32* x2 = data[ndx[i][2]];

    SubdivideNrm(depth, x0, x1, x2);
}

u32 GXGenNormalTable(u8 depth, f32* table) {
  aurora_legacy_gx_panic("GXGenNormalTable");
}
}
