#include "pipelines.hpp"

#include <mutex>

namespace acv
{
	static bool shader_stage_of(api::pipeline_subobject_type type, Stage &stage) noexcept
	{
		switch (type)
		{
		case api::pipeline_subobject_type::vertex_shader: stage = Stage::Vertex; return true;
		case api::pipeline_subobject_type::hull_shader: stage = Stage::Hull; return true;
		case api::pipeline_subobject_type::domain_shader: stage = Stage::Domain; return true;
		case api::pipeline_subobject_type::geometry_shader: stage = Stage::Geometry; return true;
		case api::pipeline_subobject_type::pixel_shader: stage = Stage::Pixel; return true;
		case api::pipeline_subobject_type::compute_shader: stage = Stage::Compute; return true;
		case api::pipeline_subobject_type::amplification_shader: stage = Stage::Amplification; return true;
		case api::pipeline_subobject_type::mesh_shader: stage = Stage::Mesh; return true;
		default: return false;
		}
	}

	static bool same_shader(const ShaderId &a, const ShaderId &b) noexcept
	{
		return a.present == b.present && a.dxbc == b.dxbc && a.size == b.size && a.checksum == b.checksum;
	}
	static bool same_pipeline(const PipelineInfo &a, const PipelineInfo &b) noexcept
	{
		if (a.layout != b.layout || a.kind != b.kind)
			return false;
		for (size_t i = 0; i < static_cast<size_t>(Stage::Count); ++i)
			if (!same_shader(a.stages[i], b.stages[i]))
				return false;
		return true;
	}

	void PipelineRegistry::add(uint64_t pipeline, uint64_t layout, uint32_t subobject_count, const api::pipeline_subobject *subobjects)
	{
		if (pipeline == 0)
			return;

		PipelineInfo info;
		info.layout = layout;

		for (uint32_t i = 0; subobjects != nullptr && i < subobject_count; ++i)
		{
			const api::pipeline_subobject &subobject = subobjects[i];

			Stage stage;
			if (!shader_stage_of(subobject.type, stage) || subobject.count == 0 || subobject.data == nullptr)
				continue;

			const auto *const desc = static_cast<const api::shader_desc *>(subobject.data);
			if (desc->code == nullptr || desc->code_size == 0)
				continue;

			ShaderId &id = info.stages[static_cast<size_t>(stage)];
			id.present = true;
			id.size = desc->code_size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(desc->code_size);
			id.dxbc = dxbc_checksum(desc->code, desc->code_size, id.checksum);
		}

		// A pipeline is identified by its pixel shader or its compute shader
		for (const Stage stage : { Stage::Pixel, Stage::Compute })
		{
			const ShaderId &id = info.stage(stage);
			if (!id.dxbc)
				continue;
			info.kind = classify(stage, id.checksum);
			if (info.kind != Kind::None)
			{
				if (id.size != known_shader(info.kind).size)
					log::warn("pipeline 0x%016llX matches %s by checksum, but the %s is %u bytes instead of %u",
						pipeline, kind_name(info.kind), stage_name(stage), id.size, known_shader(info.kind).size);
				break;
			}
		}

		const bool has_identity = info.stage(Stage::Pixel).present || info.stage(Stage::Compute).present;

		bool changed = true;
		{
			const std::unique_lock<std::shared_mutex> lock(_mutex);

			++_created;
			if (!has_identity)
				++_without_bytecode;

			// Direct3D 12 hands out the same object again for an identical description, so the same handle can arrive more than once
			if (const auto it = _all.find(pipeline); it != _all.end())
			{
				changed = !same_pipeline(it->second, info);
				it->second = info;
			}
			else
			{
				_all.emplace(pipeline, info);
			}

			if (info.kind != Kind::None)
				_known[pipeline] = info.kind;
			else
				_known.erase(pipeline);
		}

		// Recognised pipelines are always logged, the hundreds of others only with the developer tools
		if (!changed || (info.kind == Kind::None && !settings::get().dev_mode))
			return;

		std::string line;
		line.reserve(256);
		append_format(line, "pipeline 0x%016llX layout 0x%016llX", pipeline, layout);
		bool any_stage = false;
		for (size_t i = 0; i < static_cast<size_t>(Stage::Count); ++i)
		{
			const ShaderId &id = info.stages[i];
			if (!id.present)
				continue;
			any_stage = true;
			if (id.dxbc)
			{
				char hex[33];
				checksum_to_hex(id.checksum, hex);
				append_format(line, " | %s %u %s", stage_name(static_cast<Stage>(i)), id.size, hex);
			}
			else
			{
				append_format(line, " | %s %u (not DXBC)", stage_name(static_cast<Stage>(i)), id.size);
			}
		}
		if (!any_stage)
			line += " | no shader bytecode in the description";
		if (info.kind != Kind::None)
			append_format(line, " => %s", kind_name(info.kind));

		log::write("%s", line.c_str());
		if (info.kind != Kind::None)
			log::flush_if_dirty();
	}

	void PipelineRegistry::remove(uint64_t pipeline)
	{
		const std::unique_lock<std::shared_mutex> lock(_mutex);
		_all.erase(pipeline);
		_known.erase(pipeline);
	}

	Kind PipelineRegistry::kind_of(uint64_t pipeline) const
	{
		const std::shared_lock<std::shared_mutex> lock(_mutex);
		const auto it = _known.find(pipeline);
		return it != _known.end() ? it->second : Kind::None;
	}

	bool PipelineRegistry::find(uint64_t pipeline, PipelineInfo &out) const
	{
		const std::shared_lock<std::shared_mutex> lock(_mutex);
		const auto it = _all.find(pipeline);
		if (it == _all.end())
			return false;
		out = it->second;
		return true;
	}

	PipelineRegistry::Counts PipelineRegistry::counts() const
	{
		Counts result;
		const std::shared_lock<std::shared_mutex> lock(_mutex);
		result.created = _created;
		result.alive = _all.size();
		result.without_bytecode = _without_bytecode;
		for (const auto &entry : _known)
			++result.matched[static_cast<size_t>(entry.second)];
		return result;
	}
}
