#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Extract actual C functions; compile synthetic I2C/PM/registry fault tests."""
from pathlib import Path
import hashlib,json,re,subprocess,tempfile,sys

module=Path(__file__).resolve().parents[1]
root=module.parents[1]
source=(module/'ov02a10.c').read_text()
fixtures=module/'tests/fixtures'

def function(name):
    match=re.search(r'^(?:static )?(?:inline )?(?:u32|int|void|ssize_t|struct ov02a10 \*)\s*'+re.escape(name)+r'\(',source,re.M)
    assert match,name
    start=match.start(); end=source.index('{',start)+1; depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[start:end]+'\n'

def region(a,b): return source[source.index(a):source.index(b)]
names=['to_ov02a10','ov02a10_record_clear','ov02a10_record_cache','ov02a10_record_begin','ov02a10_record_validate','ov02a10_record_readback','ov02a10_record_register','ov02a10_record_unregister','ov02a10_get_start_record','ov02a10_record_format','telemetry_show','ov02a10_record_publish','ov02a10_record_unpublish','ov02a10_write_array','ov02a10_set_exposure_gain','ov02a10_set_vblank','ov02a10_set_test_pattern','__ov02a10_start_stream','__ov02a10_stop_stream','ov02a10_s_stream']
names+=['ov02a10_bayer_code','ov02a10_set_ctrl']
names=['ov02a10_pace_clear','ov02a10_pace_stat_add','ov02a10_pace_interval','ov02a10_pace_begin','ov02a10_pace_end','ov02a10_pace_bus','ov02a10_get_pace_record','ov02a10_pace_phase_format','ov02a10_pace_format','pace_show']+names
defines='\n'.join(x for x in source[:source.index('static const char * const ov02a10_supply_names[]')].splitlines() if x.startswith('#define '))
decls=region('static const char * const ov02a10_supply_names[]','static void ov02a10_pace_clear(')
tables=region('static const struct ov02a10_reg ov02a10_1600x1200_regs[]','static const char * const ov02a10_test_pattern_menu[]')
modes=region('static const struct ov02a10_mode supported_modes[]','static int ov02a10_write_array(')
unit='#include "host-shim.h"\n'+defines+'\n'+decls+'\n'+tables+'\n'+modes+'\n'+'\n'.join(function(n) for n in names)+'\n'+(module/'tests/test-telemetry.c').read_text().replace('int main(void)', 'static int baseline_main(void)')+'\n'+(module/'tests/test-standard.c').read_text()
# Actual lifecycle callsites are kept distinct from mocked framework semantics.
probe=function('ov02a10_probe'); remove=function('ov02a10_remove')
assert probe.index('mutex_init(')<probe.index('ov02a10_record_clear(')<probe.index('ov02a10_record_publish(')<probe.index('v4l2_async_register_subdev(')
assert 'ov02a10_record_unpublish(ov02a10);' in probe
assert remove.index('ov02a10_record_unpublish(')<remove.index('v4l2_async_unregister_subdev(')<remove.index('v4l2_ctrl_handler_free(')<remove.index('mutex_destroy(')
assert source.count('EXPORT_SYMBOL_GPL(ov02a10_get_start_record)')==1
assert source.count('static DEVICE_ATTR_RO(telemetry)')==1
assert 'i2c_' not in function('ov02a10_get_start_record')+function('telemetry_show')
# Every byte I2C call is inside the wrapper; PM word ID and power sequencing
# remain byte-identical to the preserved v2. No direct transfer can evade pacing.
base=(fixtures/'ov02a10.c').read_text()
def base_function(name):
    global source
    current=source
    try:
        source=base
        return function(name)
    finally:
        source=current
for name in ['ov02a10_check_sensor_id','ov02a10_power_on','ov02a10_power_off','ov02a10_get_start_record','ov02a10_remove']:
    assert function(name)==base_function(name), name
assert tables==base[base.index('static const struct ov02a10_reg ov02a10_1600x1200_regs[]'):base.index('static const char * const ov02a10_test_pattern_menu[]')]
header=(module/'ov02a10-telemetry.h').read_text()
header=re.sub(r'^/\* Modified for the MT8183 camera release on 2026-10-05\.\n \* See docs/MODIFICATIONS.json in the source-kit root; original licenses retained\.\n \*/\n', '', header)
fixture_header=(fixtures/'ov02a10-telemetry.h').read_text()
fixture_header=re.sub(r'^/\* Modified for the MT8183 camera release on 2026-10-05\.\n \* See docs/MODIFICATIONS.json in the source-kit root; original licenses retained\.\n \*/\n', '', fixture_header)
assert header==fixture_header
for call in ['i2c_smbus_write_byte_data(', 'i2c_smbus_read_byte_data(']:
    assert source.count(call)==function('ov02a10_pace_bus').count(call)==2
assert source.count('i2c_smbus_read_word_swapped(')==1
assert source.count('EXPORT_SYMBOL_GPL(ov02a10_get_pace_record)')==1
assert source.count('static DEVICE_ATTR_RO(pace)')==1
assert '&dev_attr_pace.attr,' in source
for name in ['ov02a10_get_pace_record','pace_show']:
    assert 'i2c_' not in function(name) and 'pm_runtime_' not in function(name)
assert 'i2c_' not in function('startup_writes_show')
assert source.count('static DEVICE_ATTR_RO(startup_writes)') == 1
# Production control definition keeps the old default and changes only the minimum.
control_init=function('ov02a10_initialize_controls')
assert 'vblank_def = OV02A10_DEFAULT_VBLANK;' in control_init
assert 'OV02A10_MIN_VBLANK, OV02A10_VTS_MAX - mode->height, 1,' in control_init
assert '#define OV02A10_DEFAULT_VBLANK 1580U' in source
assert '#define OV02A10_MIN_VBLANK 190U' in source
source_guards = 29  # six legacy + nine unchanged functions + ten guards above
with tempfile.TemporaryDirectory(prefix='ov02a10-telemetry-paced-host-') as tmp:
    unit_path=Path(tmp)/'actual.c'; unit_path.write_text(unit)
    exe=Path(tmp)/'check'
    cmd=['clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wformat=2','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I'+str(module/'tests/include'),'-I'+str(module/'tests'),'-I'+str(module),str(unit_path),'-o',str(exe)]
    subprocess.run(cmd,check=True)
    run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=60)
    if run.returncode:
        print(run.stdout); print(run.stderr); run.check_returncode()
    result=json.loads(run.stdout.splitlines()[-1]); result['cold_live']=json.loads(run.stdout.splitlines()[0])
result.update(files_sha256={str(p.relative_to(module)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(module.rglob('*')) if p.is_file() and p.suffix in ('.c','.h','.py')},status='PASS',pace_header_sha256=hashlib.sha256((module/'ov02a10-pace.h').read_bytes()).hexdigest(),source_sha256=hashlib.sha256(source.encode()).hexdigest(),header_sha256=hashlib.sha256((module/'ov02a10-telemetry.h').read_bytes()).hexdigest(),fixture_source_sha256=hashlib.sha256(base.encode()).hexdigest(),actual_functions=names,sanitizers=['ASan','UBSan'],source_callsite_assertions=source_guards,scope='Author actual-function host tests; synthetic I2C/PM/sysfs/list/lock wrappers, mutex-order checks; no concurrency stress in this run. No native kernel, real I2C/latch, lockdep/KASAN, or image validation.')
# Reporting is opt-in; running a public regression must not require private logs.
if '--output' in sys.argv:
    out=Path(sys.argv[sys.argv.index('--output')+1])
    with out.open('x') as f:
        f.write(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
