#!/usr/bin/env node
// Verifies that numbers and paths the markdown files state are true of the code.
//
// Documentation that quotes a test count or names a file rots the moment either
// changes, and nothing else notices. This runs in seconds and fails loudly, so
// it belongs in CI beside scripts/cross-check-parity.js.
//
// It checks:
//   * the per-suite and total test counts in IMPLEMENTATION_STATUS.md, and any
//     "NNN tests/cases" claim elsewhere, against CUTLINE_TEST in tests/native
//   * every relative markdown link resolves
//   * every source path named in backticks exists
//   * no stray control characters (a backslash path pasted through a shell or a
//     template literal turns \b, \t and \n into real characters)

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const DOCS = ['README.md', 'ARCHITECTURE.md', 'GAP_ANALYSIS.md', 'IMPLEMENTATION_STATUS.md', 'ROADMAP.md', 'MCP.md',
  'REMEDIATION.md', 'PERFORMANCE_LOG.md',
  path.join('parity', 'README.md')];
const problems = [];
const fail = message => problems.push(message);
const read = file => fs.readFileSync(path.join(root, file), 'utf8');

// ---- test counts ------------------------------------------------------------
const testDirectory = path.join(root, 'tests', 'native');
const counts = {};
let total = 0;
for (const file of fs.readdirSync(testDirectory).filter(name => name.endsWith('_tests.cpp'))) {
  const count = (fs.readFileSync(path.join(testDirectory, file), 'utf8').match(/CUTLINE_TEST\(/g) || []).length;
  counts[file.replace('_tests.cpp', '')] = count;
  total += count;
}

const status = read('IMPLEMENTATION_STATUS.md');
for (const [suite, count] of Object.entries(counts)) {
  const match = status.match(new RegExp('`cutline_' + suite + '_tests` \\| (\\d+)'));
  if (!match) fail(`IMPLEMENTATION_STATUS.md has no row for cutline_${suite}_tests`);
  else if (Number(match[1]) !== count) fail(`IMPLEMENTATION_STATUS.md says ${suite} has ${match[1]} tests, the source has ${count}`);
}
const suiteCount = Object.keys(counts).length;
const words = ['zero', 'one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine', 'ten', 'eleven', 'twelve'];
for (const doc of DOCS) {
  const text = read(doc);
  for (const match of text.matchAll(/\b(\d{2,4}) (?:native )?(?:tests|cases)\b/g)) {
    if (Number(match[1]) !== total) fail(`${doc} claims ${match[1]} tests, the source has ${total}`);
  }
  for (const match of text.matchAll(/\b(\w+) (?:suites|binaries|test binaries)\b/g)) {
    const named = words.indexOf(match[1].toLowerCase());
    if (named >= 0 && named !== suiteCount) fail(`${doc} says "${match[0]}", there are ${suiteCount}`);
  }
}

// ---- links and paths --------------------------------------------------------
const seenPaths = new Set();
for (const doc of DOCS) {
  const text = read(doc);
  for (const match of text.matchAll(/\]\(([^)#\s]+)(#[^)]*)?\)/g)) {
    const target = match[1];
    if (/^(https?:|mailto:)/.test(target)) continue;
    if (!fs.existsSync(path.join(root, path.dirname(doc), target))) fail(`${doc} links to ${target}, which does not exist`);
  }
  const sourcePath = /`((?:core|media|render|audio|playback|timeline|exporter|scripts|tests|cmake|app|third_party)\/[A-Za-z0-9_./-]+)`/g;
  for (const match of text.matchAll(sourcePath)) {
    const candidate = match[1];
    if (candidate.endsWith('/') || seenPaths.has(doc + candidate)) continue;
    seenPaths.add(doc + candidate);
    if (!fs.existsSync(path.join(root, candidate))) fail(`${doc} names ${candidate}, which does not exist`);
  }
}

// ---- stray control characters ----------------------------------------------
for (const doc of DOCS) {
  const text = read(doc);
  const controls = (text.match(/[\x00-\x08\x0b\x0c\x0e-\x1f]/g) || []).length;
  const tabs = (text.match(/\t/g) || []).length;
  if (controls || tabs) fail(`${doc} contains ${controls} control character(s) and ${tabs} tab(s); a path was probably mangled by an escape`);
}

console.log(`${total} tests in ${suiteCount} suites; ${DOCS.length} documents checked`);
if (problems.length > 0) {
  console.error('\nDocumentation check failed:');
  for (const problem of problems) console.error('  ' + problem);
  process.exit(1);
}
console.log('documentation check passed');
