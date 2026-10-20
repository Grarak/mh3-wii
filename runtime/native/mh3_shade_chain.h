#ifndef MH3_SHADE_CHAIN_H
#define MH3_SHADE_CHAIN_H
/* CHAIN forms (mh3_shade.h): per form the steps, each step's layer and mask
 * texture roles (MH3_SHADE_NO_ROLE: none) and the roles sampled. Each form
 * is one fragment program per option set (native_shader_source.cpp); a
 * structure whose steps match none keeps its fallback. Layers take roles 1
 * and 2 (their own coordinates), masks roles 3 and 4 (role 0's). */
#include <stdint.h>

enum {
    MH3_SHADE_NO_ROLE = 0xff,
    MH3_SHADE_CHAIN_FORMS = 9,
    MH3_SHADE_CHAIN_ALPHA_VERTEX = 5,
    MH3_SHADE_CHAIN_ALPHA_ONE = 6
};

typedef struct mh3_shade_chain_form {
    uint8_t steps, layer[3], mask[3], roles;
} mh3_shade_chain_form;

static const mh3_shade_chain_form mh3_shade_chain_forms[MH3_SHADE_CHAIN_FORMS] = {
    /* base + masked environment map: Gigginox, Agnaktor bsm_2, Alatreon */
    {1, {1, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {3, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0x0B},
    /* the same, alpha from a fourth map: Alatreon basm_2/3 */
    {1, {1, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {3, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0x0F},
    /* then a glow or rim map: Ceadeus b_asme_1..3, Deviljho, Agnaktor, Gobul */
    {2, {1, 2, MH3_SHADE_NO_ROLE}, {3, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0x0F},
    /* the rim map masked too: Ceadeus b_asme_0 */
    {2, {1, 2, MH3_SHADE_NO_ROLE}, {3, 4, MH3_SHADE_NO_ROLE}, 0x1F},
    /* masked environment map, lit, then a glow: Lagiacrus bsm_2 */
    {3, {1, MH3_SHADE_NO_ROLE, 2}, {3, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0x0F},
    /* a blended second base, masked environment map, lit: Qurupeco basm_1, Gigginox ibsm_0 */
    {3, {1, 2, MH3_SHADE_NO_ROLE}, {MH3_SHADE_NO_ROLE, 3, MH3_SHADE_NO_ROLE}, 0x0F},
    /* one layer: Agnaktor b_0/1, Barioth c_a_0 */
    {1, {1, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0x03},
    /* one layer, lit: Gobul b_1 */
    {2, {1, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0x03},
    /* a masked tint: Royal Ludroth b_0 */
    {1, {MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, {3, MH3_SHADE_NO_ROLE, MH3_SHADE_NO_ROLE}, 0x09},
};

#endif
