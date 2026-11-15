// Registry of every pipeline state object the game creates, with the shader checksums per stage
// and the classification against the table of known passes.
#pragma once

#include "common.hpp"
#include "dxbc.hpp"

#include <shared_mutex>
#include <unordered_map>

namespace acv
{
	struct ShaderId
	{
		bool present = false; // Bytecode was part of the pipeline description
		bool dxbc = false;    // Bytecode is a DXBC container, 'checksum' is valid
		uint32_t size = 0;
		Checksum checksum = {};
	};

	struct PipelineInfo
	{
		uint64_t layout = 0; // Root signature
		ShaderId stages[static_cast<size_t>(Stage::Count)];
		Kind kind = Kind::None;

		const ShaderId &stage(Stage s) const { return stages[static_cast<size_t>(s)]; }
		bool is_compute() const { return stage(Stage::Compute).present; }
	};

	class PipelineRegistry
	{
	public:
		// 'init_pipeline': records the shaders, classifies and logs one line.
		void add(uint64_t pipeline, uint64_t layout, uint32_t subobject_count, const api::pipeline_subobject *subobjects);
		// 'destroy_pipeline'
		void remove(uint64_t pipeline);

		// Cheap lookup for the draw path: only consults the (tiny) map of matched pipelines.
		Kind kind_of(uint64_t pipeline) const;
		// Full lookup, used while a frame capture is active.
		bool find(uint64_t pipeline, PipelineInfo &out) const;

		struct Counts
		{
			uint64_t created = 0;          // 'init_pipeline' events seen
			uint64_t alive = 0;            // Distinct pipelines currently registered
			uint64_t without_bytecode = 0; // Events that carried neither PS nor CS bytecode
			uint32_t matched[kKindCount] = {};
		};
		Counts counts() const;

	private:
		mutable std::shared_mutex _mutex;
		std::unordered_map<uint64_t, PipelineInfo> _all;
		std::unordered_map<uint64_t, Kind> _known;
		uint64_t _created = 0;
		uint64_t _without_bytecode = 0;
	};
}
