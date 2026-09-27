#!/usr/bin/env python3
"""Validate the local DSP coefficient data used by WiiCompiled's native mixer."""
import hashlib
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
target = ROOT / 'Assets/DSP/dsp_coef.bin'
if not target.exists():
    source = Path(os.environ.get('WIICOMPILED_AUDIO_ROOT', str(ROOT))) / 'runtime/assets/dsp/dsp_coef.bin'
    data = source.read_bytes()
else:
    data = target.read_bytes()
if hashlib.sha256(data).hexdigest() != 'd7741279c2e8ec5c5fb318f8fbdd6de6bf583520d288e836a5383233a4238179':
    raise ValueError('Unrecognized DSP coefficient data')
if not target.exists():
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
