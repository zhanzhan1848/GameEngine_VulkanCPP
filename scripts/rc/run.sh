#!/bin/zsh
set -euo pipefail
root="${0:A:h:h:h}"
output="${1:-/tmp/sponza-rc-$(date +%Y%m%d-%H%M%S)}"
# Resolve relative arguments before entering the repository.
output="${output:A}"
if [[ -d "$output" && -n "$(ls -A "$output")" ]]; then
    print -u2 "Refusing to overwrite existing experiment data: $output"
    exit 2
fi
mkdir -p "$output"
cd "$root"
# Record the executable, private SPIR-V, relevant source and actual model input.
# A full repository patch captures concurrent GI edits without claiming HEAD
# alone identifies the binary. No unrelated environment values are recorded.
git rev-parse HEAD > "$output/revision.txt"
git diff -- EngineTest/IntegrationTests/TestVulkanSponzaRenderGraph.cpp EngineTest/IntegrationTests/CMakeLists.txt > "$output/host.patch"
python3 - "$root" "$output" <<'PY'
import hashlib, json, os, pathlib, subprocess, sys
root, output = map(pathlib.Path, sys.argv[1:])
model = root / 'EngineTest/assets' / os.environ.get('PRIMAL_TEST_MODEL', 'Sponza.model')
inputs = [root / path for path in (
    'EngineTest/shaders/Vulkan/RC/Cascade.comp', 'EngineTest/IntegrationTests/RC/SponzaRC.h',
    'scripts/rc/report.py', 'scripts/rc/run.sh',
    'Darwin/DebugVulkan/Tests/RC/Cascade.comp.spv', 'Darwin/Debug/TestVulkanSponzaRC')]
inputs += [model]
inputs += [pathlib.Path(str(model)+suffix) for suffix in ('.scsurf', '.scboundary')
           if pathlib.Path(str(model)+suffix).exists()]
# Runtime production shader inputs are shared by the baseline and SC supplier.
inputs += sorted((root / 'Engine/Graphics/Vulkan/shaders').rglob('*.spv'))
with (output/'inputs.sha256').open('w') as stream:
    for path in inputs:
        digest = hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
        stream.write(f'{digest}  {path}\n')
config = {key: value for key,value in os.environ.items()
          if key.startswith(('PRIMAL_', 'SPONZA_', 'DDGI_', 'SC_', 'SPGI_', 'GI_', 'WRC_', 'ENV_', 'RC_'))}
config.update(RC_TEST_ENABLE='1', RC_OUTPUT=str(output), PRIMAL_FRAME_TIMING='1', SPONZA_TIMING_ONLY='1')
(output/'configuration.json').write_text(json.dumps(config,indent=2)+'\n')
PY
# Bounded execution kills only the process group started by this script.
python3 - "$root" "$output" <<'PY'
import json, os, pathlib, signal, subprocess, sys
root, output = map(pathlib.Path, sys.argv[1:])
env = dict(os.environ, **json.loads((output/'configuration.json').read_text()))
with (output/'run.log').open('wb') as log:
    process = subprocess.Popen([str(root/'Darwin/Debug/TestVulkanSponzaRC')], cwd=root,
                               env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        code = process.wait(timeout=900)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        try: process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL); process.wait()
        code = 124
(output/'exit-status.json').write_text(json.dumps({'exit_code':code})+'\n')
if code: raise SystemExit(code)
PY
python3 scripts/rc/report.py "$output" > "$output/report.log"
print "RC comparison: $output/comparison.html"
