# Sponza asset inventory and fetch tool

`sponza.json` records 80 original objects uploaded to the R2 bucket `gameengine-vulkancpp` and verified by full SHA-256 readback on 2026-11-20. The content-addressed keys and hashes pin the distributed bytes; they do not imply permission to redistribute publicly. Verify source and license before any public release. The PNG set conservatively includes the existing Sponza texture directory; actual material/normal/ORM paths depend on the model and runtime options, so this is not a claim that every file is consumed.

- `dawn-web`: the model, textures and HDR for browser modes 0–10.
- `dawn-mode11`: the native Dawn Sponza profile for browser Mode 11, including SponzaTangentFrames.model, the merged v3 signed SDF, textures and HDR. Probe lighting and surface lighting update at runtime.
- `vulkan-reference`: tangent-frame render model, texture set, HDR, and v5 SDF source plus v3 merged SDF cache. Use the geometry-visibility or tangent-frame launcher to select this model. The measured launcher alone defaults to `Sponza.model`.
- `vulkan-default`: default model with the same textures, HDR and SDF inputs.
- `windows-f350`: the two render models required by the Windows F350 gate plus textures, HDR and SDF inputs.

For browser delivery, the two models and the v3 SDF also have immutable gzip objects. The optional `gzip` entry supplies the compressed object key and byte count. Browsers cache the compressed bytes after checking the decoded size and original SHA-256. Both browser profiles share the texture and HDR cache. Reopening requires engine initialization and GPU uploads even when all scene objects are cached. Native downloads continue to use the original objects.

The SDF source and cache are separate from the render model. Their inclusion records current local bytes, **not** proof that the cache was generated from these exact inputs, or that SDF and tangent-frame render geometry match. The checked-in scene hash is not a full dependency or bake-parameter validation. Until provenance is verified, this is an inventory of reproducible bytes, not a validated GI bake release. Runtime GI history and diagnostics are excluded.

```sh
python3 scripts/fetch_assets.py list
python3 scripts/fetch_assets.py verify vulkan-reference
python3 scripts/fetch_assets.py fetch vulkan-reference --source /path/to/object-root
# With AWS CLI and bucket-scoped R2 read-only credentials configured:
export PRIMAL_ASSET_S3_ENDPOINT=https://ACCOUNT_ID.r2.cloudflarestorage.com
python3 scripts/fetch_assets.py fetch vulkan-reference --source s3://gameengine-vulkancpp
python3 scripts/asset_tests/test_fetch_assets.py
```

The public development URL supports `--source https://pub-e0a46516544c4038bea407d5fe82f92a.r2.dev` without credentials; it is rate-limited and intended for development. The script validates size and SHA-256, uses a temporary file, and never overwrites an existing different destination. The S3 path uses the AWS CLI credential chain; never commit credentials. A source must actually contain the manifest's object keys before fetch can succeed. Files are installed beneath `EngineTest/assets` relative to the checkout (or `--root` for an isolated checkout).

The 80 original objects total 566.2 MB. The three browser gzip variants add 39.1 MB. This is account-wide R2 usage in addition to any other objects and requests; it is not a spending cap. Standard storage has a free allowance, but charges can accrue when account-wide usage exceeds it. Review current [R2 pricing](https://developers.cloudflare.com/r2/pricing/) and account usage before upload and monitor afterward. The public development URL is enabled for the bucket: the original and compressed objects are publicly accessible to anyone with their keys. CORS limits browser origins but is not an access control. The Windows parity workflow accepts `PRIMAL_CI_ASSET_SOURCE=s3://gameengine-vulkancpp` as an opt-in alternative to `PRIMAL_CI_ASSET_DIR`; configure `PRIMAL_ASSET_S3_ENDPOINT`, AWS CLI and bucket-scoped read-only credentials on the runner first. No CI credential has been configured, and the R2 CI path has not yet run on Windows.
