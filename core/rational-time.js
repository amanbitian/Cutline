'use strict';

// Canonical, JSON-safe rational time values. BigInt is used internally so
// timeline arithmetic never accumulates binary floating-point drift.
const FRAME_RATES = Object.freeze({
  '23.976': Object.freeze({ numerator: 24000, denominator: 1001 }),
  '24': Object.freeze({ numerator: 24, denominator: 1 }),
  '25': Object.freeze({ numerator: 25, denominator: 1 }),
  '29.97': Object.freeze({ numerator: 30000, denominator: 1001 }),
  '30': Object.freeze({ numerator: 30, denominator: 1 }),
  '50': Object.freeze({ numerator: 50, denominator: 1 }),
  '59.94': Object.freeze({ numerator: 60000, denominator: 1001 }),
  '60': Object.freeze({ numerator: 60, denominator: 1 })
});

function bigint(value, label) {
  try { return BigInt(value); } catch { throw new TypeError(`${label} must be an integer.`); }
}
function gcd(a, b) { a = a < 0n ? -a : a; b = b < 0n ? -b : b; while (b) [a, b] = [b, a % b]; return a || 1n; }
function normalize(numerator, denominator) {
  let n = bigint(numerator, 'numerator'); let d = bigint(denominator, 'denominator');
  if (d === 0n) throw new RangeError('denominator cannot be zero.');
  if (d < 0n) { n = -n; d = -d; }
  const divisor = gcd(n, d);
  return Object.freeze({ numerator: (n / divisor).toString(), denominator: (d / divisor).toString() });
}
function parts(time) { if (!time || time.numerator === undefined || time.denominator === undefined) throw new TypeError('Expected RationalTime.'); return [bigint(time.numerator, 'numerator'), bigint(time.denominator, 'denominator')]; }
function add(left, right) { const [a, b] = parts(left); const [c, d] = parts(right); return normalize(a * d + c * b, b * d); }
function subtract(left, right) { const [a, b] = parts(left); const [c, d] = parts(right); return normalize(a * d - c * b, b * d); }
function compare(left, right) { const [a, b] = parts(left); const [c, d] = parts(right); return a * d === c * b ? 0 : a * d < c * b ? -1 : 1; }
function rateParts(rate) { if (!rate || !Number.isSafeInteger(rate.numerator) || !Number.isSafeInteger(rate.denominator) || rate.numerator <= 0 || rate.denominator <= 0) throw new TypeError('Invalid frame rate.'); return [BigInt(rate.numerator), BigInt(rate.denominator)]; }
function fromFrames(frames, rate) { const [numerator, denominator] = rateParts(rate); return normalize(bigint(frames, 'frames') * denominator, numerator); }
function toFrameCount(time, rate, rounding = 'nearest') {
  const [n, d] = parts(time); const [rateNumerator, rateDenominator] = rateParts(rate); const scaled = n * rateNumerator; const divisor = d * rateDenominator;
  if (rounding === 'floor') return scaled / divisor;
  if (rounding === 'ceil') return (scaled + divisor - 1n) / divisor;
  return (scaled + divisor / 2n) / divisor;
}
function displayUnitsToTime(units, rate, unitsPerSecond = 10) {
  if (!Number.isFinite(units) || !Number.isSafeInteger(unitsPerSecond) || unitsPerSecond <= 0) throw new TypeError('Invalid display time.');
  const [rateNumerator, rateDenominator] = rateParts(rate);
  const frames = BigInt(Math.round(units * Number(rateNumerator) / (Number(rateDenominator) * unitsPerSecond)));
  return fromFrames(frames, rate);
}
function formatTimecode(time, rate) {
  const frames = toFrameCount(time, rate); const [rateNumerator, rateDenominator] = rateParts(rate); const nominal = Number((rateNumerator + rateDenominator / 2n) / rateDenominator);
  const totalSeconds = frames / BigInt(nominal); const frame = frames % BigInt(nominal);
  const hours = totalSeconds / 3600n; const minutes = (totalSeconds / 60n) % 60n; const seconds = totalSeconds % 60n;
  const pad = value => value.toString().padStart(2, '0');
  return `${pad(hours)}:${pad(minutes)}:${pad(seconds)}:${pad(frame)}`;
}

module.exports = { FRAME_RATES, normalize, add, subtract, compare, fromFrames, toFrameCount, displayUnitsToTime, formatTimecode };
