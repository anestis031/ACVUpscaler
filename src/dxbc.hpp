// DXBC container checksum extraction and the table of known game shaders.
// Header-only and free of ReShade/Windows dependencies so the offline test can use it as is.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace acv
{
	using Checksum = std::array<uint8_t, 16>;

	enum class Stage : uint8_t
	{
		Vertex,
		Hull,
		Domain,
		Geometry,
		Pixel,
		Compute,
		Amplification,
		Mesh,
		Count
	};

	inline const char *stage_name(Stage stage) noexcept
	{
		static constexpr const char *names[] = { "VS", "HS", "DS", "GS", "PS", "CS", "AS", "MS" };
		return stage < Stage::Count ? names[static_cast<size_t>(stage)] : "??";
	}

	// Render passes of the game that the add-on recognises (docs/TECHNICAL.md, section 9).
	enum class Kind : uint8_t
	{
		None,
		TaaFull,
		TaaDeditherTiles,
		TaaCameraOnly,
		Dedither,
		DeditherCopy,
		Tonemap,
		PostCopyReactiveMask,
		FinalComposite,
		Fsr1Easu,
		Fsr1Rcas,
		LinearDepth,
		MotionBlurVelocityPrep,
		Count
	};
	inline constexpr size_t kKindCount = static_cast<size_t>(Kind::Count);

	constexpr uint8_t hex_digit(char c)
	{
		return static_cast<uint8_t>(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
	}
	constexpr Checksum parse_checksum(const char (&hex)[33])
	{
		Checksum result = {};
		for (size_t i = 0; i < 16; ++i)
			result[i] = static_cast<uint8_t>((hex_digit(hex[i * 2]) << 4) | hex_digit(hex[i * 2 + 1]));
		return result;
	}

	struct KnownShader
	{
		Kind kind;
		const char *name;
		Stage stage;
		uint32_t size;
		Checksum checksum;
	};

	// Indexed by Kind (entry 0 is the "None" placeholder).
	inline constexpr KnownShader kKnownShaders[kKindCount] = {
		{ Kind::None,                   "None",                   Stage::Count,   0,     {} },
		{ Kind::TaaFull,                "TaaFull",                Stage::Pixel,   13008, parse_checksum("6202d8776055bca21d0917ea2f132489") },
		{ Kind::TaaDeditherTiles,       "TaaDeditherTiles",       Stage::Pixel,   11328, parse_checksum("c93036c2eb6e088d4c4b300c323fea29") },
		{ Kind::TaaCameraOnly,          "TaaCameraOnly",          Stage::Pixel,   10512, parse_checksum("bcfb0dcbb5a587e9bd51453d221285c5") },
		{ Kind::Dedither,               "Dedither",               Stage::Pixel,   5428,  parse_checksum("f2ed63a06a52cf953e754542d9d19021") },
		{ Kind::DeditherCopy,           "DeditherCopy",           Stage::Pixel,   360,   parse_checksum("6fba7fa6744f0910e9b8f6a78597e80b") },
		{ Kind::Tonemap,                "Tonemap",                Stage::Pixel,   2584,  parse_checksum("b86c9ac92739b98e177976a5af1fc189") },
		{ Kind::PostCopyReactiveMask,   "PostCopyReactiveMask",   Stage::Pixel,   708,   parse_checksum("bfb62f74067b78e1fddf476cca004f89") },
		{ Kind::FinalComposite,         "FinalComposite",         Stage::Pixel,   700,   parse_checksum("3e59ae4e62e0027df6ccf7ca802d9f40") },
		{ Kind::Fsr1Easu,               "Fsr1Easu",               Stage::Compute, 8588,  parse_checksum("23b12224a412441108b16e5cd05eaff5") },
		{ Kind::Fsr1Rcas,               "Fsr1Rcas",               Stage::Compute, 2056,  parse_checksum("a9a0bb6688d819184c1a37d76f279f84") },
		{ Kind::LinearDepth,            "LinearDepth",            Stage::Pixel,   548,   parse_checksum("4aa38de423c338cbcfbd17b4e70595ac") },
		{ Kind::MotionBlurVelocityPrep, "MotionBlurVelocityPrep", Stage::Compute, 3244,  parse_checksum("fa944d58d1cf9e3556e83c4f4d0af149") },
	};

	inline const KnownShader &known_shader(Kind kind) noexcept
	{
		return kKnownShaders[kind < Kind::Count ? static_cast<size_t>(kind) : 0];
	}
	inline const char *kind_name(Kind kind) noexcept { return known_shader(kind).name; }
	inline bool kind_is_compute(Kind kind) noexcept { return known_shader(kind).stage == Stage::Compute; }
	inline bool kind_is_taa(Kind kind) noexcept
	{
		return kind == Kind::TaaFull || kind == Kind::TaaDeditherTiles || kind == Kind::TaaCameraOnly;
	}

	// Copies the 16-byte checksum stored in a DXBC container header (bytes 4..19).
	// Returns false when the blob is not a DXBC container.
	inline bool dxbc_checksum(const void *code, size_t size, Checksum &out) noexcept
	{
		if (code == nullptr || size < 20 || std::memcmp(code, "DXBC", 4) != 0)
			return false;
		std::memcpy(out.data(), static_cast<const uint8_t *>(code) + 4, out.size());
		return true;
	}

	// Looks up a shader of the given stage in the table of known shaders.
	inline Kind classify(Stage stage, const Checksum &checksum) noexcept
	{
		for (size_t i = 1; i < kKindCount; ++i)
			if (kKnownShaders[i].stage == stage && kKnownShaders[i].checksum == checksum)
				return kKnownShaders[i].kind;
		return Kind::None;
	}

	// Writes 32 lower-case hex digits plus a terminating zero.
	inline void checksum_to_hex(const Checksum &checksum, char (&out)[33]) noexcept
	{
		static constexpr char digits[] = "0123456789abcdef";
		for (size_t i = 0; i < 16; ++i)
		{
			out[i * 2] = digits[checksum[i] >> 4];
			out[i * 2 + 1] = digits[checksum[i] & 0xF];
		}
		out[32] = '\0';
	}
}
