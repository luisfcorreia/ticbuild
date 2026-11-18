// Resolves the bundled custom TIC-80 executable for the current platform/arch,
// with an environment override. Returns undefined when no bundled binary is
// available (callers fall back to the external/stock controller).
import * as os from "node:os";
import * as path from "node:path";
import { fileExists } from "../fileSystem";
import { getPathRelativeToTemplates } from "../templates";

export interface Tic80ExecutableOptions {
  environment?: NodeJS.ProcessEnv;
}

const TICBUILD_TEMPLATE_DIR = "TIC-80-ticbuild";

// Env override for the bundled custom TIC-80 binary (full path incl. binary name).
const CUSTOM_TIC80_ENV = "TIC80_CUSTOM_LOCATION";

function platformArchDir(): string {
  const platform = process.platform;
  const arch = process.arch;
  if (platform === "win32") return `win-${arch}`;
  if (platform === "linux") return `linux-${arch}`;
  if (platform === "darwin") return `darwin-${arch}`;
  return `${platform}-${arch}`;
}

function binaryNameForPlatform(): string {
  return process.platform === "win32" ? "tic80.exe" : "tic80";
}

export function resolveCustomTic80LogicalPath(options?: Tic80ExecutableOptions): string | undefined {
  const environment = options?.environment ?? process.env;

  // Explicit env override takes precedence.
  const override = environment[CUSTOM_TIC80_ENV];
  if (override && fileExists(override)) {
    return override;
  }

  const dir = path.join(getPathRelativeToTemplates(TICBUILD_TEMPLATE_DIR), platformArchDir());
  const candidate = path.join(dir, binaryNameForPlatform());
  if (fileExists(candidate)) {
    return candidate;
  }

  return undefined;
}

export function customTic80Exists(options?: Tic80ExecutableOptions): boolean {
  return resolveCustomTic80LogicalPath(options) !== undefined;
}
