#ifndef MH3_SHADE_CHAIN_H
#define MH3_SHADE_CHAIN_H
/* CHAIN forms (mh3_shade.h): per form the steps, each step's layer and mask
 * texture roles (MH3_SHADE_NO_ROLE: none) and the roles sampled. Each form
 * is one fragment program per option set (native_shader_source.cpp); a
 * structure whose steps match none keeps its fallback. Layers take roles 1
 * and 2 (their own coordinates), masks the rest of them, then roles 3 and 4
 * (role 0's coordinate). */
#include <stdint.h>

enum {
    MH3_SHADE_NO_ROLE = 0xff,
    MH3_SHADE_CHAIN_FORMS = 14
};

/* mask_alpha bit i: step i's mask is its role's alpha; base bit i: step i
 * reads the base x0; alpha_sum: the alpha is a clamped sum times a second
 * source (else one source times its factor). */
typedef struct mh3_shade_chain_form {
    uint8_t steps, layer[3], mask[3], mask_alpha, base, alpha_sum, roles;
} mh3_shade_chain_form;

static const mh3_shade_chain_form mh3_shade_chain_forms[MH3_SHADE_CHAIN_FORMS] = {
    /* base + masked environment map: Gigginox, Agnaktor bsm_2, Alatreon */
    {1, {1, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {2, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0, 0, 0, 0x07},
    /* the same, alpha from a fourth map: Alatreon basm_2/3 */
    {1, {1, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {2, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0, 0, 0, 0x0F},
    /* then a glow or rim map: Ceadeus b_asme_1..3, Deviljho, Agnaktor, Gobul */
    {2, {1, 2, MH3_SHADE_NO_ROLE}, {3, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0, 0, 0, 0x0F},
    /* the rim map masked too: Ceadeus b_asme_0 */
    {2, {1, 2, MH3_SHADE_NO_ROLE}, {3, 4, MH3_SHADE_NO_ROLE}, 0, 0, 0, 0x1F},
    /* masked environment map, lit, then a glow: Lagiacrus bsm_2 */
    {3, {1, MH3_SHADE_NO_ROLE, 2}, {3, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0, 0, 0, 0x0F},
    /* a blended second base, masked environment map, lit: Qurupeco basm_1, Gigginox ibsm_0 */
    {3, {1, 2, MH3_SHADE_NO_ROLE}, {MH3_SHADE_NO_ROLE, 3, MH3_SHADE_NO_ROLE}, 0, 0, 0, 0x0F},
    /* one layer: Agnaktor b_0/1, Barioth c_a_0 */
    {1, {1, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0, 0, 0, 0x03},
    /* one layer, lit: Gobul b_1 */
    {2, {1, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0, 0, 0, 0x03},
    /* the same, alpha a sum: the A-p08-suimen-L water */
    {2, {1, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0, 0, 1, 0x03},
    /* a masked tint: Royal Ludroth b_0 */
    {1, {MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {1, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0, 0, 0, 0x03},
    /* the same, alpha from a third map: Royal Ludroth ba_0 */
    {1, {MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {1, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0, 0, 0, 0x07},
    /* a lightmap blended toward c0, doubled, lit doubled: the Tundra B-lm ground */
    {3, {MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {1, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0, 0, 1, 0x03},
    /* a tinted base, an overlay on the base masked by its alpha, lit: Qurupeco basm_0 */
    {3, {0, 1, MH3_SHADE_NO_ROLE}, {2, 1, MH3_SHADE_NO_ROLE}, 2, 2, 0, 0x07},
    /* the lit base alone, alpha from two maps: Jhen Mohran ba_0 */
    {0, {MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0, 0, 1, 0x03},
};

#endif
