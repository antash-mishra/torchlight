"""Complex-plane field reference and composition invariants for shader studies."""
import hashlib
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parent
gpu = json.loads((ROOT / 'gpu.json').read_text())
ui = json.loads((ROOT / 'preview.json').read_text())
frames = {frame['name']: frame for frame in gpu['frames']}

# Predeclared: two RGBA8 code values for the field kernel. Derive wave
# harmonics in the complex plane, independently of GLSL vector operations.
TOLERANCE = 2
import cmath

def expected_kernel(frame):
    values = []
    rotation = complex(0.8, 0.6) * 2.03
    for y in range(frame['height']):
        for x in range(frame['width']):
            z = complex(3 * ((x+.5)/frame['width']-.5), 3 * ((y+.5)/frame['height']-.5))
            coordinates = [z]
            for level in range(3): coordinates.append(coordinates[-1] * rotation + complex(1.7, -2.6))
            terms = []
            for level, position in enumerate(coordinates):
                ex, ey = cmath.exp(1j*position.real), cmath.exp(1j*position.imag)
                phase_x = position.real + .7 * ey.real
                phase_y = position.imag - .6 * ex.imag
                terms.append(.52 * .48**level * cmath.exp(1j*phase_x).imag * cmath.exp(1j*phase_y).real)
            shade = round(255 * (.5 + .5 * math.fsum(terms)))
            values.extend([shade,shade,shade,255])
    return values

def difference(left, right): return max(abs(a-b) for a,b in zip(left,right))
kernel = frames['field-kernel']
reference_errors = {'field-kernel':difference(kernel['pixels'],expected_kernel(kernel))}
fault_errors = {'field-kernel-fault':difference(frames['field-kernel-fault']['pixels'],expected_kernel(kernel)),
                'frozen-time-fault':difference(frames['frozen-time-fault']['pixels'],frames['moving']['pixels']),
                'resolution-fault':difference(frames['resolution-fault']['pixels'],frames['static']['pixels'])}
static, moving = (frames[name]['pixels'] for name in ['static','moving'])
scale_errors, repeat_errors, temporal_errors, safe_query = [],[],[],[]
for variant in [0,1,2]:
    prefix = '' if variant==0 else f'variant-{variant}-'
    baseline=frames[prefix+'static']['pixels']
    scaled=frames[prefix+'scaled']['pixels']
    for y in range(32):
        for x in range(96):
            source, target=(y*96+x)*4,((y*3+1)*288+x*3+1)*4
            scale_errors.extend(abs(baseline[source+i]-scaled[target+i]) for i in range(4))
    repeat_errors.append(difference(baseline,frames[prefix+'repeat']['pixels']))
    temporal_errors.append(difference(baseline,frames[prefix+'moving']['pixels']))
    safe_query.extend(baseline[(y*96+x)*4+3] for y in range(32) for x in range(40))
art_frames=[f for f in gpu['frames'] if not f['name'].startswith('field-kernel')]
valid_premultiplication=all(f['pixels'][i+c]<=f['pixels'][i+3] for f in art_frames for i in range(0,len(f['pixels']),4) for c in range(3))
metrics = {
    'reference_errors':reference_errors, 'fault_errors':fault_errors,
    'scale_error':max(scale_errors), 'repeat_error':max(repeat_errors),
    'temporal_difference':min(temporal_errors), 'protected_query_max_alpha':max(safe_query),
    'maximum_alpha':max(max(f['pixels'][3::4]) for f in art_frames), 'premultiplication':valid_premultiplication,
    'disabled_max':max(frames['disabled']['pixels']), 'variants':3,
}
(ROOT / 'reference.json').write_text(json.dumps(metrics,indent=2))
command = 'npm exec --yes --package=@playwright/cli -- playwright-cli -s=shader-study run-code --filename docs/designs/quiet-system/validation/check-shader.js'
def assertion(name, actual, op='eq', expected=True):
    return {'name':name,'op':op,'actual':actual,'expected':expected}
def gate(identifier, category, method, assertions, artifacts=('gpu.json','reference.json')):
    return {'id':identifier,'category':category,'required':True,'method':method,'command':command,'artifacts':list(artifacts),'assertions':assertions}
manifest = {
    'schema_version':1,
    'subject':{'feature':'three spectral search-field shader studies','revision':'fdd7f3b5e01beb839029de3ecbb3ff6c1ff97ac9','dirty':True,
               'claim':'Original spectral silk, ion cloud and folded prism shaders; complex-plane field reference, bounded premultiplied composition and lifecycle-safe animation'},
    'environment':{'build_type':'browser design preview','compiler':'Chrome GLSL ES compiler','gpu':gpu['environment']['renderer'],'driver':gpu['environment']['version'],'api':gpu['environment']['shading'],
                   'asset_hash':'sha256:'+hashlib.sha256((ROOT.parent / 'search-art.js').read_bytes()).hexdigest(),'resolution':'96x32, 288x96','seed':0},
    'checks':[
        gate('shader.build','build','Compile/link actual fragment shader; numeric draw and eight independent resource ownership cycles.',[assertion('GL errors',[c['error'] for c in gpu['cycles']],'max_abs_le',0)]),
        gate('shader.reference','shader-contract','Compare the actual four-scale field kernel with an independent double-precision complex-plane harmonic series; validate final output invariants separately.',[assertion('Maximum CPU/GPU error',list(reference_errors.values()),'max_abs_le',TOLERANCE)]),
        gate('shader.invariants','numeric-invariants','Read complete buffers; require protected query region transparency, bounded opacity and premultiplied channels.',[assertion('Protected alpha',max(safe_query),'eq',0),assertion('Opacity cap',metrics['maximum_alpha'],'le',200),assertion('Premultiplied',valid_premultiplication)]),
        gate('shader.scenes','controlled-scenes','Check repeat draws at fixed time and temporal change at eight seconds.',[assertion('Repeat error',metrics['repeat_error'],'eq',0),assertion('Temporal difference',metrics['temporal_difference'],'gt',2)]),
        gate('shader.ablation','ablation','Set strength uniform to zero and redraw; require every output byte to vanish.',[assertion('Disabled output',metrics['disabled_max'],'eq',0)]),
        gate('shader.scale','metamorphic','Scale both viewport dimensions by three; compare corresponding normalized pixel centers.',[assertion('Resolution invariance',metrics['scale_error'],'le',TOLERANCE)]),
        gate('shader.lifecycle','lifecycle','Eight create/draw/pause/resume/destroy cycles; test missing WebGL, failed shader compilation and actual loss/recovery. Exercise query expansion, collapse and reduced motion.',[
            assertion('Resource objects remaining',[sum(c[k] for k in ['programs','buffers','shaders']) for c in gpu['cycles']],'max_abs_le',0),
            assertion('Final resource objects',sum(gpu['finalObjects'][k] for k in ['programs','buffers','shaders']),'eq',0),
            assertion('Compile fallback',gpu['badCompile']['fallback']),assertion('No WebGL fallback',gpu['unavailable']['fallback']),assertion('Context recovery',ui['contextRecovery']),assertion('UI checks',ui['checks'],'eq','passed'),assertion('Artwork variants verified',len(ui['variants']),'eq',3)],('gpu.json','preview.json')),
        gate('shader.sensitivity','test-sensitivity','Zero the actual field amplitude, freeze time and overwrite the resolution binding. All faults must be detected.',[assertion('Field kernel fault detected',fault_errors['field-kernel-fault'],'gt',TOLERANCE),assertion('Frozen time detected',fault_errors['frozen-time-fault'],'gt',TOLERANCE),assertion('Resolution binding detected',fault_errors['resolution-fault'],'gt',TOLERANCE)]),
    ],
    'limitations':['Chrome/SwiftShader validation; hardware GPU and native GTK rendering not validated.','GPU timer queries unavailable; CPU dispatch samples are diagnostic, not a GPU performance budget or benchmark.','Frozen PNG fallback captures shader time zero; future hardware/native GTK color behavior has not been validated.'],
}
(ROOT/'evidence.json').write_text(json.dumps(manifest,indent=2))
print(json.dumps(metrics))
