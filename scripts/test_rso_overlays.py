#!/usr/bin/env python3
import unittest
from translate_rso_overlays import isolate_profile_source


class OverlayEmissionTests(unittest.TestCase):
    def test_profile_calls_use_lifetime_dispatch_and_isolated_typed_symbols(self):
        source = ('void func_808C99A4(CpuContext* ctx) { ctx->lr = 0x808C99A4u; '
                  'InvokeDirectCpu<0x808C7790u>(ctx); func_80004000_leaf(); }')
        isolated = isolate_profile_source(source, 'quest_field')
        self.assertIn('mh3_quest_field_808C99A4(', isolated)
        self.assertIn('ctx->lr = 0x808C99A4u', isolated)
        self.assertIn('InvokeIndirectCpu(0x808C7790u, ctx)', isolated)
        self.assertIn('mh3_quest_field_80004000_leaf()', isolated)
        self.assertNotIn('InvokeDirectCpu<', isolated)

    def test_host_symbol_isolated_without_changing_guest_addresses(self):
        source = 'void func_8097D22C(CpuContext* ctx) { ctx->lr = 0x8097D22Cu; InvokeIndirectCpu(ctx->ctr, ctx); }'
        isolated = isolate_profile_source(source, 'map_village')
        self.assertIn('mh3_map_village_8097D22C(', isolated)
        self.assertIn('ctx->lr = 0x8097D22Cu', isolated)
        self.assertIn('InvokeIndirectCpu(ctx->ctr, ctx)', isolated)

    def test_rejects_unknown_direct_call_form(self):
        with self.assertRaises(ValueError):
            isolate_profile_source('InvokeDirectCpu<target>(other_context);', 'quest_field')


if __name__ == '__main__':
    unittest.main()
