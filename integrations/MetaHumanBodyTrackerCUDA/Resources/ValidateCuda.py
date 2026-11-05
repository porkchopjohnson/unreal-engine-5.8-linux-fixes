"""Run a short body-only solve on a transient Performance; does not save any assets.

Run in Unreal with -ExecutePythonScript=<this file> -unattended -RenderOffscreen -nosound.
Set MH_CUDA_TEST_ASSET to an existing imported Performance object path.
Optional MH_CUDA_TEST_FRAMES defaults to 32. Add -MetaHumanCudaProfile for kernel traces.
"""
import math
import os
import time
import unreal

source_path = os.environ["MH_CUDA_TEST_ASSET"]
source = unreal.load_asset(source_path)
assert source, f"Cannot load {source_path}"
performance = unreal.new_object(unreal.MetaHumanPerformance)
performance.set_editor_property("input_type", unreal.DataInputType.MONO_FOOTAGE)
performance.set_editor_property("footage_capture_data", source.get_editor_property("footage_capture_data"))
performance.set_editor_property("face_tracking", False)
performance.set_editor_property("body_tracking", True)
performance.set_editor_property("skip_preview", True)
performance.set_processing_range(0, int(os.environ.get("MH_CUDA_TEST_FRAMES", "32")))
performance.set_blocking_processing(True)
unreal.log(f"CUDA validation can_process={performance.can_process()}")
start = time.monotonic()
result = performance.start_pipeline()
assert result == unreal.StartPipelineErrorType.NONE, f"Could not start: {result}"
assert not performance.is_processing(), "Blocking processing did not complete"
frames = performance.get_animation_data()
valid = [f for f in frames if len(f.raw_body_animation_smplx_pose)]
assert valid, "No body pose frames produced"
assert all(math.isfinite(v) for f in valid for v in f.raw_body_animation_smplx_pose), "Non-finite pose"
assert performance.contains_animation_data_type(unreal.FrameAnimationDataType.BODY)
unreal.log(f"CUDA_VALIDATION_SUCCESS: {len(valid)} body pose frames, {time.monotonic() - start:.3f}s")
