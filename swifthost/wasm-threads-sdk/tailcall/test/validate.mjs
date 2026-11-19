// Validate a wasm module with V8 (WebAssembly.Module) to get a precise error location. Usage: node validate.mjs <file.wasm>
import { readFileSync } from 'node:fs';
const buf = readFileSync(process.argv[2]);
try { new WebAssembly.Module(buf); console.log('valid'); } catch (e) { console.log(String(e)); }
