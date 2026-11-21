"""Publish a port-variant link of the decomp working copy (tools/decomp/wc.py build) as the decomp publishes
its own: OUT/TM2_SOURCE_PORT.EXE, OUT/TM2_SOURCE_PORT.json (the keys the port's recompiler reads) and
OUT/TM2_SOURCE_PORT/ (the maps, port_data.bin).

    python publish.py LINK_DIR OUT"""
import json
import shutil
import sys
from pathlib import Path

link, out = Path(sys.argv[1]), Path(sys.argv[2])
maps = out / 'TM2_SOURCE_PORT'
maps.mkdir(parents=True, exist_ok=True)
for name in ('addrmap.tsv', 'symbols.tsv', 'pointer_slots.tsv', 'functions.tsv', 'jumptables.tsv', 'functions_ida.tsv',
             'names_ida.tsv', 'recomp_inventory.json', 'port_data.bin', 'port_text.bin', 'port_rodata.bin'):
    if (link / name).exists():
        shutil.copyfile(link / name, maps / name)
shutil.copyfile(link / 'TM2_SOURCE.EXE', out / 'TM2_SOURCE_PORT.EXE')
syms = {}
for line in (link / 'symbols.tsv').read_text(encoding='ascii').splitlines():
    p = line.split('\t')
    if len(p) == 4 and p[0] != 'va' and not line.startswith('#'):
        syms.setdefault(p[2], p[0])
rep = json.loads((link / 'report.json').read_text(encoding='utf-8'))
pv = rep['port_variant']
arrays = [dict(a, address=syms[a['name']]) for a in pv['arrays']]
info = dict(schema='tm2-source-port-v1', executable='out/TM2_SOURCE_PORT.EXE', sha256=rep['sha256'],
            flavour='port variant + the PC port overlay (tools/decomp/wc.py)', defines=pv['defines'],
            limits=pv['limits'], t_size=rep['t_size'], pc0=rep['pc0'], port_base=pv['base'],
            port_sections=pv['sections'], port_arrays=arrays,
            port_functions=pv.get('port_functions', []), port_defines=pv.get('port_defines', []),
            changed_functions=[dict(retail_va=s['function'], name=s['symbol'], unit=s['unit'], delta=s['delta'])
                               for s in pv['changed_sections'] if s['code']],
            contains_retail_bytes=True)
(out / 'TM2_SOURCE_PORT.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8', newline='\n')
print('published', out / 'TM2_SOURCE_PORT.EXE', rep['sha256'][:12], len(info['changed_functions']), 'changed functions')
