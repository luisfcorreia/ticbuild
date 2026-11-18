// Copies a pkg-snapshot asset from the virtual filesystem to a real cache/temp
// file with mode 0755 so it can be passed to child_process.spawn(). In
// development (source on disk) the asset path is used directly.
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

const CACHE_ROOT = path.join(os.tmpdir(), "ticbuild", "assets");

export function isPkgSnapshot(): boolean {
  try {
    return process.execPath === (require.main?.filename ?? "");
  } catch {
    return false;
  }
}

export function extractAssetToCache(assetPath: string): string {
  fs.mkdirSync(CACHE_ROOT, { recursive: true });
  const dest = path.join(CACHE_ROOT, path.basename(assetPath));
  if (!fs.existsSync(dest)) {
    fs.copyFileSync(assetPath, dest);
    fs.chmodSync(dest, 0o755);
  }
  return dest;
}

export function resolveSpawnableAsset(assetPath: string): string {
  if (isPkgSnapshot()) {
    return extractAssetToCache(assetPath);
  }
  return assetPath;
}
