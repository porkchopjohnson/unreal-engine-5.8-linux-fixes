"""A/B solve on transient Performances; never saves project assets.

Run with -RenderOffscreen -unattended -ExecutePythonScript=<this file>.
MH_PELVIS_ASSET: source Performance path (required).
MH_PELVIS_START / MH_PELVIS_END: processing range (default whole saved range).
MH_PELVIS_OUTPUT: diagnostic JSON path (required; parent must exist).
MH_PELVIS_MODES: comma-separated modes (default 0,1; use 1 for a smoke test).
MH_PELVIS_FOOT_LOCKING: optional 0/1 override.
MH_PELVIS_BASELINE: optional previous JSON with a matching mode-0 solve.
MH_PELVIS_GAIN: optional exaggeration gain, default 1.
MH_PELVIS_IMAGE_TRACKING: optional 0/1 image tracking override, default 1.
MH_PELVIS_EASING: optional easing strength, default 0.5.
"""
import json
import math
import os
import time
import traceback
from pathlib import Path
import unreal

output = Path(os.environ['MH_PELVIS_OUTPUT'])
source = unreal.load_asset(os.environ['MH_PELVIS_ASSET'])
assert source
start = int(os.environ.get('MH_PELVIS_START', '0'))
end = int(os.environ.get('MH_PELVIS_END', str(source.get_editor_property('end_frame_to_process'))))
result = {'source': source.get_path_name(), 'start': start, 'end': end, 'runs': {}}
if os.environ.get('MH_PELVIS_BASELINE'):
    baseline = json.loads(Path(os.environ['MH_PELVIS_BASELINE']).read_text())
    assert baseline['source'] == result['source'] and baseline['start'] == start and baseline['end'] == end
    result['runs']['0'] = baseline['runs']['0']
    result['baseline_reused_from'] = os.environ['MH_PELVIS_BASELINE']
performances = {}

def serialize_transform(t):
    return {'translation': [t.translation.x, t.translation.y, t.translation.z],
            'rotation': [t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w]}

try:
    gain = float(os.environ.get('MH_PELVIS_GAIN', '1'))
    image_tracking = int(os.environ.get('MH_PELVIS_IMAGE_TRACKING', '1'))
    easing = float(os.environ.get('MH_PELVIS_EASING', '0.5'))
    result['settings'] = {'exaggeration': gain, 'image_tracking': image_tracking, 'easing_strength': easing}
    modes = [int(v) for v in os.environ.get('MH_PELVIS_MODES', '1' if '0' in result['runs'] else '0,1').split(',')]
    assert modes and all(v in [0, 1] for v in modes)
    for enabled in modes:
        performance = unreal.new_object(unreal.MetaHumanPerformance)
        performances[enabled] = performance
        performance.set_editor_property('input_type', unreal.DataInputType.MONO_FOOTAGE)
        for prop in ['footage_capture_data', 'auto_body_height', 'body_height', 'body_detection_confidence',
                     'body_tracking_confidence', 'enable_foot_locking', 'pelvis_pitch_strength',
                     'pelvis_roll_strength', 'pelvis_twist_strength', 'pelvis_tilt_strength',
                     'pelvis_image_weight', 'pelvis_max_tilt', 'pelvis_max_twist']:
            performance.set_editor_property(prop, source.get_editor_property(prop))
        performance.set_editor_property('use_pelvis_console_settings', False)
        performance.set_editor_property('enable_pelvis_refinement', bool(enabled))
        performance.set_editor_property('pelvis_easing_strength', easing)
        performance.set_editor_property('pelvis_exaggeration', gain)
        performance.set_editor_property('pelvis_image_tracking', bool(image_tracking))
        performance.set_editor_property('pelvis_dump_diagnostics', True)
        if 'MH_PELVIS_FOOT_LOCKING' in os.environ:
            performance.set_editor_property('enable_foot_locking', bool(int(os.environ['MH_PELVIS_FOOT_LOCKING'])))
        performance.set_editor_property('face_tracking', False)
        performance.set_editor_property('body_tracking', True)
        performance.set_editor_property('skip_preview', True)
        performance.set_processing_range(start, end)
        performance.set_blocking_processing(True)
        began = time.monotonic()
        status = performance.start_pipeline()
        assert status == unreal.StartPipelineErrorType.NONE, str(status)
        assert not performance.is_processing()
        frames = performance.get_animation_data()
        data = []
        for i, frame in enumerate(frames):
            pose = list(frame.raw_body_animation_smplx_pose)
            assert all(math.isfinite(v) for v in pose), f'Nonfinite pose at {i}'
            data.append({'frame': i, 'pose': pose,
                         'source': {str(k): serialize_transform(v) for k, v in frame.raw_body_animation_smplx_data.items()},
                         'body': {str(k): serialize_transform(v) for k, v in frame.body_animation_data.items()}})
        valid = sum(bool(f['pose']) for f in data)
        assert valid == end - start, f'Expected {end-start} valid frames, got {valid}'
        result['runs'][str(enabled)] = {'seconds': time.monotonic() - began, 'valid_frames': valid,
                                      'foot_locking': performance.get_editor_property('enable_foot_locking'), 'frames': data}
        output.write_text(json.dumps(result))
        unreal.log(f'PELVIS_VALIDATION_MODE_{enabled}_SUCCESS: {valid} frames')
    if '0' in result['runs'] and '1' in result['runs']:
        assert result['runs']['0']['foot_locking'] == result['runs']['1']['foot_locking']
        assert len(result['runs']['0']['frames']) == len(result['runs']['1']['frames'])
        unreal.log('PELVIS_AB_VALIDATION_SUCCESS')
    else:
        unreal.log('PELVIS_SMOKE_VALIDATION_SUCCESS')
except Exception:
    result['error'] = traceback.format_exc()
    unreal.log_error(result['error'])
    raise
finally:
    output.write_text(json.dumps(result))
