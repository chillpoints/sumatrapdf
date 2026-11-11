import { copyFileSync, existsSync, statSync } from "node:fs";
import { join } from "node:path";
import { genDocsForBuild } from "./gen-docs";
import { detectVisualStudio, getGitSha1, msbuildDefinesArg, runLogged } from "./util";

async function main() {
  const start = performance.now();
  console.log("=== Building SumatraPDF Custom (x64 Standalone with Folder Sidebar) ===");

  console.log("1) Fetching translations into .work/translations.txt ...");
  await runLogged("bun", [join("cmd", "trans-dl.ts"), "--no-ai"]);

  console.log("2) Generating embedded scripts & docs ...");
  await genDocsForBuild();

  console.log("3) Detecting Visual Studio & MSBuild ...");
  const { msbuildPath } = detectVisualStudio();
  const sha1 = await getGitSha1();
  const defines = msbuildDefinesArg({ GIT_COMMIT_ID: sha1 });
  const slnPath = join("vs2022", "SumatraPDF.sln");

  console.log(`4) Compiling SumatraPDF-static (x64 Release, sha1=${sha1.slice(0, 8)}) ...`);
  await runLogged(msbuildPath, [
    slnPath,
    "/t:SumatraPDF-static:Rebuild",
    "/p:Configuration=Release;Platform=x64",
    defines,
    "/m",
  ]);

  const staticExe = join("out", "rel64", "SumatraPDF-static.exe");
  const outExe = join("out", "rel64", "SumatraPDF.exe");
  if (!existsSync(staticExe)) {
    throw new Error(`Expected output binary not found: ${staticExe}`);
  }
  copyFileSync(staticExe, outExe);
  const szMb = (statSync(outExe).size / (1024 * 1024)).toFixed(2);
  const elapsed = ((performance.now() - start) / 1000).toFixed(1);
  console.log(`=== Done in ${elapsed}s: ${outExe} (${szMb} MB) ===`);
}

await main();
