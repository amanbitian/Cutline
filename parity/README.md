# Cutline detailed parity manifests

Each domain manifest uses the YAML 1.2 JSON profile: JSON syntax stored in a `.yaml` file. This is valid YAML and lets the repository validate manifests with Node.js without a third-party parser.

`feature_defaults` applies to every entry unless an entry overrides a field. The generator also inherits domain and reference-product/version/channel fields from the manifest. Therefore every feature has the same required shape: identity, reference target, priority, status, implementation notes, acceptance checks, test placeholders, platform/hardware policy, gaps, and licensing notes. Status is evidence-based: `implemented` requires a real runtime plus non-empty unit/integration evidence; `validated` additionally requires its relevant compatibility/performance coverage.

The top-level `feature-parity.yaml` remains the backwards-compatible aggregate roadmap. `scripts/generate-parity-report.js` reads the detailed manifests and produces `premiere-parity-report.json` and `premiere-parity-report.md`; generated reports are intentionally not hand-edited.

## Keeping the manifests honest

The manifests were originally hand-maintained and drifted from the code: they recorded nothing implemented while dozens of capabilities were built. `scripts/cross-check-parity.js` closes that gap. A record claiming `implemented` or `validated` must list real test names under `tests.unit`, and every name must exist as a `CUTLINE_TEST` in `tests/native`. Run it in CI:

```powershell
node scripts/cross-check-parity.js            # verify; exits non-zero on a mismatch
node scripts/cross-check-parity.js --apply    # re-apply the recorded assessment
node scripts/generate-parity-report.js        # regenerate the report
```

The assessment table at the top of the script is the reviewed judgement of what the code does. Silence is not evidence either way: a capability not listed there keeps its recorded status. When you build something, add its entry with the tests that show it; when a claim turns out to be optimistic, downgrade it there.
