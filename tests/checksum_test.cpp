// Offline check of the DXBC checksum extraction and the known-shader table against two shader
// blobs extracted from the PIX captures.
// Usage: checksum_test <pso22536_ps.bin (TAA full, capture A)> <pso22376_cs.bin (EASU, capture B)>
#include "../src/dxbc.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace acv;

static int s_failures = 0;

static void check(bool condition, const char *what)
{
	std::printf("%s  %s\n", condition ? "ok  " : "FAIL", what);
	if (!condition)
		++s_failures;
}

static void check_file(const char *path, Stage stage, Kind expected)
{
	std::ifstream file(path, std::ios::binary);
	const std::vector<char> blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	std::printf("%s: %zu bytes\n", path, blob.size());
	check(!blob.empty(), "file could be read");
	if (blob.empty())
		return;

	Checksum checksum = {};
	check(dxbc_checksum(blob.data(), blob.size(), checksum), "blob is a DXBC container");

	char hex[33];
	checksum_to_hex(checksum, hex);
	std::printf("      checksum %s\n", hex);

	const Kind kind = classify(stage, checksum);
	std::printf("      classified as %s, expected %s\n", kind_name(kind), kind_name(expected));
	check(kind == expected, "checksum matches the expected known shader");
	check(blob.size() == known_shader(expected).size, "blob size matches the table");

	// The same checksum must not match for another stage
	check(classify(stage == Stage::Pixel ? Stage::Compute : Stage::Pixel, checksum) == Kind::None, "no match for the wrong stage");
}

int main(int argc, char **argv)
{
	if (argc != 3)
	{
		std::fprintf(stderr, "usage: checksum_test <taa_full_ps.bin> <easu_cs.bin>\n");
		return 2;
	}

	check_file(argv[1], Stage::Pixel, Kind::TaaFull);
	check_file(argv[2], Stage::Compute, Kind::Fsr1Easu);

	// Malformed input must be rejected without reading out of bounds
	Checksum checksum = {};
	const char short_blob[19] = { 'D', 'X', 'B', 'C' };
	check(!dxbc_checksum(short_blob, sizeof(short_blob), checksum), "blob shorter than the header is rejected");
	const char not_dxbc[32] = { 'D', 'X', 'I', 'L' };
	check(!dxbc_checksum(not_dxbc, sizeof(not_dxbc), checksum), "blob without DXBC magic is rejected");
	check(!dxbc_checksum(nullptr, 100, checksum), "null blob is rejected");

	// The table itself: indexed by Kind, unique checksums, hex round trip
	bool table_ok = true;
	for (size_t i = 1; i < kKindCount; ++i)
	{
		const KnownShader &entry = kKnownShaders[i];
		table_ok = table_ok && static_cast<size_t>(entry.kind) == i;
		table_ok = table_ok && classify(entry.stage, entry.checksum) == entry.kind;
		table_ok = table_ok && (entry.stage == Stage::Pixel || entry.stage == Stage::Compute);
		for (size_t k = i + 1; k < kKindCount; ++k)
			table_ok = table_ok && entry.checksum != kKnownShaders[k].checksum;
	}
	check(table_ok, "known-shader table is consistent (index == Kind, unique checksums)");

	char hex[33];
	checksum_to_hex(known_shader(Kind::Fsr1Rcas).checksum, hex);
	check(std::string(hex) == "a9a0bb6688d819184c1a37d76f279f84", "checksum hex round trip");

	std::printf("%s (%d failure%s)\n", s_failures == 0 ? "PASSED" : "FAILED", s_failures, s_failures == 1 ? "" : "s");
	return s_failures == 0 ? 0 : 1;
}
