'use strict';

const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const parityDirectory = path.join(root, 'parity');
const outputJson = path.join(root, 'premiere-parity-report.json');
const outputMarkdown = path.join(root, 'premiere-parity-report.md');
const allowedStatuses = new Set(['planned', 'prototype', 'partial', 'in_progress', 'implemented', 'validated', 'deferred']);
const seenCapabilityIds = new Set();

const manifests = fs.readdirSync(parityDirectory, { withFileTypes: true })
  .filter(entry => entry.isFile() && entry.name.endsWith('.yaml'))
  .map(entry => {
    const file = path.join(parityDirectory, entry.name);
    let manifest;
    try { manifest = JSON.parse(fs.readFileSync(file, 'utf8')); } catch (error) { throw new Error(`${entry.name} is not valid JSON-profile YAML: ${error.message}`); }
    if (!manifest.domain || !Array.isArray(manifest.features) || !manifest.feature_defaults) throw new Error(`${entry.name} needs domain, feature_defaults, and features.`);
    const inherited = {
      domain: manifest.domain,
      reference_product: manifest.reference_product || 'Professional NLE capability baseline',
      reference_version: manifest.reference_version || '26.5',
      reference_channel: manifest.reference_channel || 'stable'
    };
    const features = manifest.features.map(feature => ({ ...inherited, ...manifest.feature_defaults, ...feature }));
    for (const feature of features) {
      for (const key of ['id', 'name', 'domain', 'reference_product', 'reference_version', 'reference_channel', 'priority', 'status', 'implementation', 'acceptance', 'tests', 'platforms', 'hardware', 'known_gaps', 'licensing_notes']) {
        if (!(key in feature)) throw new Error(`${entry.name}:${feature.id || '<unknown>'} is missing ${key}.`);
      }
      if (seenCapabilityIds.has(feature.id)) throw new Error(`Capability ID ${feature.id} is duplicated.`);
      seenCapabilityIds.add(feature.id);
      if (!allowedStatuses.has(feature.status)) throw new Error(`${entry.name}:${feature.id} has invalid status ${feature.status}.`);
      if (!Array.isArray(feature.acceptance) || feature.acceptance.length === 0) throw new Error(`${entry.name}:${feature.id} needs measurable acceptance.`);
      if (feature.status === 'implemented' || feature.status === 'validated') {
        if (!Array.isArray(feature.tests.unit) || !feature.tests.unit.length || !Array.isArray(feature.tests.integration) || !feature.tests.integration.length) {
          throw new Error(`${entry.name}:${feature.id} needs non-empty unit and integration evidence.`);
        }
      }
    }
    return { file: entry.name, domain: manifest.domain, features };
  });

const domains = manifests.map(manifest => {
  const counts = Object.fromEntries([...allowedStatuses].map(status => [status, 0]));
  for (const feature of manifest.features) counts[feature.status] += 1;
  return { domain: manifest.domain, manifest: `parity/${manifest.file}`, total: manifest.features.length, counts };
}).sort((left, right) => left.domain.localeCompare(right.domain));

const capabilities = manifests.flatMap(manifest => manifest.features).sort((left, right) => left.domain.localeCompare(right.domain) || left.id.localeCompare(right.id));
const report = { generated_at: new Date().toISOString(), status_definitions: [...allowedStatuses], domains, capabilities, total_features: domains.reduce((total, domain) => total + domain.total, 0), weighting: 'No overall percentage is emitted. Domains are unweighted and statuses are evidence-based.' };
fs.writeFileSync(outputJson, `${JSON.stringify(report, null, 2)}\n`);
const lines = ['# Cutline parity report', '', `Generated: ${report.generated_at}`, '', 'No overall marketing percentage is reported; domains are intentionally unweighted.', '', '| Domain | Tracked | Validated | Implemented | Partial | Prototype | In progress | Planned | Deferred |', '|---|---:|---:|---:|---:|---:|---:|---:|---:|'];
for (const domain of domains) lines.push(`| ${domain.domain} | ${domain.total} | ${domain.counts.validated} | ${domain.counts.implemented} | ${domain.counts.partial} | ${domain.counts.prototype} | ${domain.counts.in_progress} | ${domain.counts.planned} | ${domain.counts.deferred} |`);
lines.push('', '## Capability inventory', '');
for (const domain of domains) {
  lines.push(`### ${domain.domain}`, '', '| ID | Capability | Priority | Status | Acceptance |', '|---|---|---|---|---|');
  for (const capability of capabilities.filter(feature => feature.domain === domain.domain)) {
    lines.push(`| ${capability.id} | ${capability.name} | ${capability.priority} | ${capability.status} | ${capability.acceptance.join(' ')} |`);
  }
  lines.push('');
}
fs.writeFileSync(outputMarkdown, `${lines.join('\n')}\n`);
console.log(`Validated ${report.total_features} atomic features across ${domains.length} domains.`);
