#pragma once

#include "MetalCommonHeaders.h"

namespace primal::graphics::metal
{
    constexpr u64 align_size_for_constant_buffer(u64 size)
	{
		return math::align_size_up<256>(size);
	}

	constexpr u64 align_size_for_texture(u64 size)
	{
		return math::align_size_up<512>(size);
	}

	struct METAL_COLOR_ATTACHMENT_ARRAY
	{
		MTL::RenderPipelineColorAttachmentDescriptor* 	descs[8];
		u32 			 								count{ 0 };
	};
	

	enum class MetalPipelineSubobjectType {
		VertexFunction,
		FragmentFunction,
		VertexDescriptor,
		ColorAttachments,
		DepthAttachmentFormat,
		StencilAttachmentFormat,
		RasterizerState,
		SampleCount,
		AlphaToCoverage,
		TessellationFunction,
		TessellationParams,
		RenderPass,
		PrimitiveTopology,
		InputPrimitiveTopology,
		ComputeFunction,
		ViewInstancing,
		CachedPSO,
		NodeMask,
		Flags,
	};

	template<MetalPipelineSubobjectType Type, typename T>
	class alignas(void*) MetalPipelineSubobject {
	public:
		MetalPipelineSubobject() = default;
		explicit MetalPipelineSubobject(T value) : _type(Type), _value(value) {}
		MetalPipelineSubobject& operator=(const T& value) { _value = value; return *this; }

		MetalPipelineSubobjectType type() const { return _type; }
		T value() const { return _value; }

	private:
		const MetalPipelineSubobjectType 	_type{ Type };
		T 									_value{};
	};
#define METAL_PSS(name, type, value_type) \
	using metal_pipeline_subobject_##name = MetalPipelineSubobject<type, value_type>;

	METAL_PSS(vertex_function, MetalPipelineSubobjectType::VertexFunction, MTL::Function*);
	METAL_PSS(fragment_function, MetalPipelineSubobjectType::FragmentFunction, MTL::Function*);
	METAL_PSS(vertex_descriptor, MetalPipelineSubobjectType::VertexDescriptor, MTL::VertexDescriptor*);
	METAL_PSS(render_attachments, MetalPipelineSubobjectType::ColorAttachments, METAL_COLOR_ATTACHMENT_ARRAY);
	METAL_PSS(depth_attachment_format, MetalPipelineSubobjectType::DepthAttachmentFormat, MTL::PixelFormat);
	METAL_PSS(stencil_attachment_format, MetalPipelineSubobjectType::StencilAttachmentFormat, MTL::PixelFormat);
	METAL_PSS(rasterizer_state, MetalPipelineSubobjectType::RasterizerState, bool);
	METAL_PSS(sample_count, MetalPipelineSubobjectType::SampleCount, NS::UInteger);
	METAL_PSS(alpha_to_coverage, MetalPipelineSubobjectType::AlphaToCoverage, bool);
	METAL_PSS(tessellation_function, MetalPipelineSubobjectType::TessellationFunction, MTL::Function*);
	METAL_PSS(tessellation_params, MetalPipelineSubobjectType::TessellationParams, MTL::TessellationPartitionMode);
	METAL_PSS(render_pass, MetalPipelineSubobjectType::RenderPass, MTL::RenderPipelineColorAttachmentDescriptorArray*);
	METAL_PSS(primitive_topology, MetalPipelineSubobjectType::PrimitiveTopology, MTL::PrimitiveType);
	METAL_PSS(input_primitive_topology, MetalPipelineSubobjectType::InputPrimitiveTopology, MTL::PrimitiveTopologyClass);
	METAL_PSS(compute_function, MetalPipelineSubobjectType::ComputeFunction, MTL::Function*);
	METAL_PSS(view_instancing, MetalPipelineSubobjectType::ViewInstancing, MTL::RenderPipelineDescriptor*); // 占位
	METAL_PSS(cached_pso, MetalPipelineSubobjectType::CachedPSO, NS::Data*);
	METAL_PSS(node_mask, MetalPipelineSubobjectType::NodeMask, u32);
	METAL_PSS(flags, MetalPipelineSubobjectType::Flags, u32);

#undef METAL_PSS

	struct metal_pipeline_state_stream {
		metal_pipeline_subobject_vertex_function vertex_function{ nullptr };
		metal_pipeline_subobject_fragment_function fragment_function{ nullptr };
		metal_pipeline_subobject_vertex_descriptor vertex_descriptor{ nullptr };
		metal_pipeline_subobject_render_attachments color_attachments{};
		metal_pipeline_subobject_depth_attachment_format depth_attachment_format{};
		metal_pipeline_subobject_stencil_attachment_format stencil_attachment_format{};
		metal_pipeline_subobject_rasterizer_state rasterizer_state{ true };
		metal_pipeline_subobject_sample_count sample_count{ 1 };
		metal_pipeline_subobject_alpha_to_coverage alpha_to_coverage{ false };
		metal_pipeline_subobject_tessellation_function tessellation_function{ nullptr };
		metal_pipeline_subobject_tessellation_params tessellation_params{ MTL::TessellationPartitionModePow2 };
		metal_pipeline_subobject_render_pass render_pass{ nullptr };
		metal_pipeline_subobject_primitive_topology primitive_topology{ MTL::PrimitiveTypeTriangle };
		metal_pipeline_subobject_input_primitive_topology input_primitive_topology{ MTL::PrimitiveTopologyClassTriangle };
		metal_pipeline_subobject_compute_function compute_function{ nullptr };
		metal_pipeline_subobject_view_instancing view_instancing{ nullptr }; // 占位
		metal_pipeline_subobject_cached_pso cached_pso{ nullptr };
		metal_pipeline_subobject_node_mask node_mask{ 0 };
		metal_pipeline_subobject_flags flags{ 0 };

		MTL::RenderPipelineDescriptor* ToDescriptor() const {
			MTL::RenderPipelineDescriptor* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();

			// 现有子对象
			if (vertex_function.value()) descriptor->setVertexFunction(vertex_function.value());
			if (fragment_function.value()) descriptor->setFragmentFunction(fragment_function.value());
			if (vertex_descriptor.value()) descriptor->setVertexDescriptor(vertex_descriptor.value());
			descriptor->setDepthAttachmentPixelFormat(depth_attachment_format.value());

			// 新增子对象
			descriptor->setRasterSampleCount(sample_count.value());
			descriptor->setAlphaToCoverageEnabled(alpha_to_coverage.value());
			if (tessellation_function.value()) {
				descriptor->setVertexFunction(tessellation_function.value()); // 假设顶点函数包含曲面细分
				descriptor->setTessellationPartitionMode(tessellation_params.value());
			}
			if (color_attachments.value().count > 0) {
				for(u32 i{ 0 }; i < color_attachments.value().count; ++i)
				{
					auto color_attachment{ color_attachments.value().descs[i] };
					descriptor->colorAttachments()->setObject(color_attachment, i);
					// descriptor->colorAttachments()->object(i)->setPixelFormat(MTL::PixelFormat::PixelFormatRGBA16Float);
				}			
			}
			if (input_primitive_topology.value())
			{
				descriptor->setInputPrimitiveTopology(input_primitive_topology.value());
			}
			if (cached_pso.value()) {
				// 序列化支持（需要额外实现）
			}

			// 其他子对象（如 primitive_topology、stencil_state）可能在渲染时设置
			return descriptor;
		}
	};

	MTL::RenderPipelineState* create_pipeline_status(void* stream);
}