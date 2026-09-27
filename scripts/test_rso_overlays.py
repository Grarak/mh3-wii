#!/usr/bin/env python3
import unittest
from translate_rso_overlays import isolate_source


class OverlayEmissionTests(unittest.TestCase):
    def test_host_symbol_isolated_without_changing_guest_addresses(self):
        source = 'void func_8097D22C(CpuContext* ctx) { ctx->lr = 0x8097D22Cu; InvokeIndirectCpu(ctx->ctr, ctx); }'
        isolated = isolate_source(source, 0x8097D22C)
        self.assertIn('mh3_map_village_8097D22C(', isolated)
        self.assertIn('ctx->lr = 0x8097D22Cu', isolated)
        self.assertIn('InvokeIndirectCpu(ctx->ctr, ctx)', isolated)

    def test_rejects_other_profile_symbols_and_direct_calls(self):
        for source in ('void func_8097D22C() { func_8097D160(); }',
                       'void func_8097D22C() { InvokeDirectCpu<0x8097D160u>(ctx); }',
                       'void func_8097D22C() { func_8097D22C_raw(); }'):
            with self.assertRaises(ValueError):
                isolate_source(source, 0x8097D22C)


if __name__ == '__main__':
    unittest.main()
