#!/usr/bin/env node
// Converts the PPM images the golden-frame tests write into PNG, so a golden
// can be reviewed in any viewer before it is accepted.
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

function readPpm(file) {
  const buffer = fs.readFileSync(file);
  // P6 header: magic, width, height, maxval, then one whitespace byte.
  let offset = 0;
  const token = () => {
    while (offset < buffer.length && /\s/.test(String.fromCharCode(buffer[offset]))) offset++;
    let start = offset;
    while (offset < buffer.length && !/\s/.test(String.fromCharCode(buffer[offset]))) offset++;
    return buffer.slice(start, offset).toString('ascii');
  };
  if (token() !== 'P6') throw new Error(`${file} is not a P6 PPM`);
  const width = parseInt(token(), 10);
  const height = parseInt(token(), 10);
  const maxval = parseInt(token(), 10);
  if (maxval !== 255) throw new Error(`${file} has unsupported depth ${maxval}`);
  offset++;
  return { width, height, rgb: buffer.slice(offset, offset + width * height * 3) };
}

function crc32(buffer) {
  let crc = ~0;
  for (const byte of buffer) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc >>> 0;
}

function chunk(type, data) {
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body));
  return Buffer.concat([length, body, crc]);
}

function writePng(file, image) {
  const header = Buffer.alloc(13);
  header.writeUInt32BE(image.width, 0);
  header.writeUInt32BE(image.height, 4);
  header[8] = 8;   // bit depth
  header[9] = 2;   // truecolour RGB
  // Each scanline is prefixed with its filter type; 0 means none.
  const raw = Buffer.alloc((image.width * 3 + 1) * image.height);
  for (let y = 0; y < image.height; y++) {
    const target = y * (image.width * 3 + 1);
    raw[target] = 0;
    image.rgb.copy(raw, target + 1, y * image.width * 3, (y + 1) * image.width * 3);
  }
  fs.writeFileSync(file, Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', header),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]));
}

// Upscales with nearest-neighbour so small test frames are legible.
function scale(image, factor) {
  if (factor <= 1) return image;
  const width = image.width * factor;
  const height = image.height * factor;
  const rgb = Buffer.alloc(width * height * 3);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const source = (Math.floor(y / factor) * image.width + Math.floor(x / factor)) * 3;
      const target = (y * width + x) * 3;
      rgb[target] = image.rgb[source];
      rgb[target + 1] = image.rgb[source + 1];
      rgb[target + 2] = image.rgb[source + 2];
    }
  }
  return { width, height, rgb };
}

const args = process.argv.slice(2);
if (args.length === 0) {
  console.error('usage: node scripts/ppm-to-png.js <file.ppm|directory> [scale]');
  process.exit(2);
}
const factor = args[1] ? parseInt(args[1], 10) : 1;
const target = args[0];
const files = fs.statSync(target).isDirectory()
  ? fs.readdirSync(target).filter(name => name.endsWith('.ppm')).map(name => path.join(target, name))
  : [target];
for (const file of files) {
  const out = file.replace(/\.ppm$/, '.png');
  writePng(out, scale(readPpm(file), factor));
  console.log(`${path.basename(out)}`);
}
