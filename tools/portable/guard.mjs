// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
//
// Drift guard for the Node and Docker ports of the Windows build (see ported-from.json).
import { createHash } from 'node:crypto';
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));

/** Returns an error message when one of `names` (paths relative to `root`) differs from the version the
 *  ports were written against, or null. FM1_ALLOW_UNSYNCED=1 turns the error into a warning. */
export function portedFromProblem(root, names) {
  const pinned = JSON.parse(readFileSync(join(HERE, 'ported-from.json'), 'utf8')).files;
  const changed = [];
  for (const name of names) {
    const file = join(root, name);
    if (!existsSync(file)) { changed.push(`${name} (missing)`); continue; }
    if (createHash('sha256').update(readFileSync(file)).digest('hex') !== pinned[name]) changed.push(name);
  }
  if (!changed.length) return null;
  const message = `${changed.join(', ')} changed since tools/docker and tools/portable were ported from it. The ` +
    'Docker route may no longer match the Windows build: review the change, update the ports, compare with a ' +
    'Windows build (node tools/portable/elf-compare.mjs), then update tools/portable/ported-from.json. ' +
    'Set FM1_ALLOW_UNSYNCED=1 to build anyway.';
  if (process.env.FM1_ALLOW_UNSYNCED === '1') { console.error(`warning: ${message}`); return null; }
  return message;
}
