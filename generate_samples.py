#!/usr/bin/env python3

"""Pack WAV files into a contiguous flash image with a simple header + TOC.

Image layout (all little-endian):

- Header (32 bytes):
  - magic[4]          : b"PSMP"
  - version           : u16 (1)
  - reserved0         : u16 (0)
  - sample_count      : u32
  - toc_offset        : u32 (bytes from image start)
  - entry_size        : u32
  - data_offset       : u32 (start of payload area)
  - used_size         : u32 (last used byte + 1)
  - image_size        : u32 (final output file size)

- TOC entries (fixed-size, 64 bytes each):
  - name[32]          : UTF-8, zero-terminated
  - data_offset       : u32
  - data_size         : u32
  - sample_rate       : u32
  - frame_count       : u32
  - channels          : u16
  - bits_per_sample   : u16
  - block_align       : u16
  - audio_format      : u16 (1 = PCM)
  - data_crc32        : u32
  - reserved1         : u32

- Payload area:
  - Raw WAV PCM payload for each sample, placed contiguously and aligned.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import struct
import sys
import wave
import zlib


MAGIC = b"PSMP"
VERSION = 1
HEADER_STRUCT = struct.Struct("<4sHHIIIIII")
ENTRY_STRUCT = struct.Struct("<32sIIIIHHHHII")


@dataclasses.dataclass
class Sample:
	name: str
	path: pathlib.Path
	channels: int
	bits_per_sample: int
	sample_rate: int
	frame_count: int
	block_align: int
	audio_format: int
	data: bytes
	data_offset: int = 0
	data_crc32: int = 0


def align_up(value: int, alignment: int) -> int:
	if alignment <= 0:
		raise ValueError("alignment must be > 0")
	return (value + alignment - 1) // alignment * alignment


def parse_int_auto(text: str) -> int:
	return int(text, 0)


def read_wav(path: pathlib.Path, allow_non16: bool) -> Sample:
	with wave.open(str(path), "rb") as wf:
		channels = wf.getnchannels()
		sample_width = wf.getsampwidth()
		sample_rate = wf.getframerate()
		frame_count = wf.getnframes()
		comp_type = wf.getcomptype()
		data = wf.readframes(frame_count)

	if comp_type != "NONE":
		raise ValueError(f"{path.name}: unsupported compression type {comp_type}")

	bits_per_sample = sample_width * 8
	if not allow_non16 and bits_per_sample != 16:
		raise ValueError(
			f"{path.name}: expected 16-bit PCM, found {bits_per_sample}-bit"
		)

	if channels <= 0:
		raise ValueError(f"{path.name}: invalid channel count {channels}")
	if sample_rate <= 0:
		raise ValueError(f"{path.name}: invalid sample rate {sample_rate}")
	if frame_count < 0:
		raise ValueError(f"{path.name}: invalid frame count {frame_count}")

	expected_size = frame_count * channels * sample_width
	if len(data) != expected_size:
		raise ValueError(
			f"{path.name}: payload size mismatch, got {len(data)}, expected {expected_size}"
		)

	stem = path.stem
	return Sample(
		name=stem,
		path=path,
		channels=channels,
		bits_per_sample=bits_per_sample,
		sample_rate=sample_rate,
		frame_count=frame_count,
		block_align=channels * sample_width,
		audio_format=1,
		data=data,
	)


def encode_name(name: str, max_len: int = 31) -> bytes:
	raw = name.encode("utf-8")
	if len(raw) > max_len:
		raise ValueError(
			f"sample name '{name}' too long ({len(raw)} bytes), max is {max_len}"
		)
	return raw + b"\x00" * (32 - len(raw))


def collect_samples(
	input_dir: pathlib.Path,
	pattern: str,
	allow_non16: bool,
) -> list[Sample]:
	wav_paths = sorted(p for p in input_dir.glob(pattern) if p.is_file())
	if not wav_paths:
		raise ValueError(f"no WAV files found in {input_dir} matching '{pattern}'")

	samples = [read_wav(path, allow_non16) for path in wav_paths]

	seen = set()
	for sample in samples:
		key = sample.name.lower()
		if key in seen:
			raise ValueError(
				"duplicate sample names after case-folding: "
				f"'{sample.name}'"
			)
		seen.add(key)

	return samples


def build_image(
	samples: list[Sample],
	payload_align: int,
	image_size: int | None,
) -> tuple[bytes, int, int]:
	header_size = HEADER_STRUCT.size
	entry_size = ENTRY_STRUCT.size
	toc_size = len(samples) * entry_size
	toc_offset = header_size
	data_offset = align_up(toc_offset + toc_size, payload_align)

	cursor = data_offset
	for sample in samples:
		cursor = align_up(cursor, payload_align)
		sample.data_offset = cursor
		sample.data_crc32 = zlib.crc32(sample.data) & 0xFFFFFFFF
		cursor += len(sample.data)

	used_size = cursor
	final_size = used_size if image_size is None else image_size
	if final_size < used_size:
		raise ValueError(
			f"image too small: need at least {used_size} bytes, got {final_size}"
		)

	blob = bytearray(b"\xFF" * final_size)

	header = HEADER_STRUCT.pack(
		MAGIC,
		VERSION,
		0,
		len(samples),
		toc_offset,
		entry_size,
		data_offset,
		used_size,
		final_size,
	)
	blob[0:header_size] = header

	toc_cursor = toc_offset
	for sample in samples:
		entry = ENTRY_STRUCT.pack(
			encode_name(sample.name),
			sample.data_offset,
			len(sample.data),
			sample.sample_rate,
			sample.frame_count,
			sample.channels,
			sample.bits_per_sample,
			sample.block_align,
			sample.audio_format,
			sample.data_crc32,
			0,
		)
		blob[toc_cursor : toc_cursor + entry_size] = entry
		toc_cursor += entry_size

	for sample in samples:
		start = sample.data_offset
		end = start + len(sample.data)
		blob[start:end] = sample.data

	return bytes(blob), used_size, final_size


def build_arg_parser() -> argparse.ArgumentParser:
	parser = argparse.ArgumentParser(
		description="Pack WAV samples into a contiguous flash image"
	)
	parser.add_argument(
		"--input-dir",
		type=pathlib.Path,
        required=True,
		help="Directory containing WAV files",
	)
	parser.add_argument(
		"--pattern",
		default="*.wav",
		help="Glob pattern for sample files (default: *.wav)",
	)
	parser.add_argument(
		"--output",
		type=pathlib.Path,
		default=pathlib.Path("samples.bin"),
		help="Output binary image path (default: samples.bin)",
	)
	parser.add_argument(
		"--payload-align",
		type=parse_int_auto,
		default=256,
		help="Alignment for each sample payload (default: 256)",
	)
	parser.add_argument(
		"--image-size",
		type=parse_int_auto,
		default=0x100000,
		help="Total output image size in bytes; 0 means no padding (default: 0x100000)",
	)
	parser.add_argument(
		"--allow-non16",
		action="store_true",
		help="Allow non-16-bit PCM WAV files",
	)
	return parser


def main() -> int:
	parser = build_arg_parser()
	args = parser.parse_args()

	if args.payload_align <= 0:
		print("error: --payload-align must be > 0", file=sys.stderr)
		return 2

	if not args.input_dir.exists() or not args.input_dir.is_dir():
		print(f"error: input directory not found: {args.input_dir}", file=sys.stderr)
		return 2

	image_size = None if args.image_size == 0 else args.image_size
	if image_size is not None and image_size <= 0:
		print("error: --image-size must be > 0 or 0", file=sys.stderr)
		return 2

	try:
		samples = collect_samples(args.input_dir, args.pattern, args.allow_non16)
		blob, used_size, final_size = build_image(samples, args.payload_align, image_size)
	except ValueError as exc:
		print(f"error: {exc}", file=sys.stderr)
		return 1

	args.output.parent.mkdir(parents=True, exist_ok=True)
	args.output.write_bytes(blob)

	print(f"Wrote {args.output} ({final_size} bytes, used {used_size} bytes)")
	print(f"Samples: {len(samples)}")
	for idx, sample in enumerate(samples):
		print(
			f"  [{idx:02d}] {sample.name}: off=0x{sample.data_offset:08X} "
			f"size={len(sample.data)} sr={sample.sample_rate} ch={sample.channels} "
			f"bits={sample.bits_per_sample}"
		)

	return 0


if __name__ == "__main__":
	raise SystemExit(main())
