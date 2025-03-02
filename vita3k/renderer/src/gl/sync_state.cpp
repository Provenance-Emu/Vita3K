// Vita3K emulator project
// Copyright (C) 2023 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <renderer/functions.h>
#include <renderer/profile.h>
#include <renderer/types.h>

#include <renderer/gl/functions.h>
#include <renderer/gl/state.h>
#include <renderer/gl/types.h>

#include <gxm/functions.h>
#include <gxm/types.h>
#include <util/align.h>
#include <util/log.h>

#include <shader/spirv_recompiler.h>

#include <cmath>

namespace renderer::gl {

static GLenum translate_depth_func(SceGxmDepthFunc depth_func) {
    R_PROFILE(__func__);

    switch (depth_func) {
    case SCE_GXM_DEPTH_FUNC_NEVER:
        return GL_NEVER;
    case SCE_GXM_DEPTH_FUNC_LESS:
        return GL_LESS;
    case SCE_GXM_DEPTH_FUNC_EQUAL:
        return GL_EQUAL;
    case SCE_GXM_DEPTH_FUNC_LESS_EQUAL:
        return GL_LEQUAL;
    case SCE_GXM_DEPTH_FUNC_GREATER:
        return GL_GREATER;
    case SCE_GXM_DEPTH_FUNC_NOT_EQUAL:
        return GL_NOTEQUAL;
    case SCE_GXM_DEPTH_FUNC_GREATER_EQUAL:
        return GL_GEQUAL;
    case SCE_GXM_DEPTH_FUNC_ALWAYS:
        return GL_ALWAYS;
    }

    return GL_ALWAYS;
}

static GLenum translate_stencil_op(SceGxmStencilOp stencil_op) {
    R_PROFILE(__func__);

    switch (stencil_op) {
    case SCE_GXM_STENCIL_OP_KEEP:
        return GL_KEEP;
    case SCE_GXM_STENCIL_OP_ZERO:
        return GL_ZERO;
    case SCE_GXM_STENCIL_OP_REPLACE:
        return GL_REPLACE;
    case SCE_GXM_STENCIL_OP_INCR:
        return GL_INCR;
    case SCE_GXM_STENCIL_OP_DECR:
        return GL_DECR;
    case SCE_GXM_STENCIL_OP_INVERT:
        return GL_INVERT;
    case SCE_GXM_STENCIL_OP_INCR_WRAP:
        return GL_INCR_WRAP;
    case SCE_GXM_STENCIL_OP_DECR_WRAP:
        return GL_DECR_WRAP;
    }

    return GL_KEEP;
}

static GLenum translate_stencil_func(SceGxmStencilFunc stencil_func) {
    R_PROFILE(__func__);

    switch (stencil_func) {
    case SCE_GXM_STENCIL_FUNC_NEVER:
        return GL_NEVER;
    case SCE_GXM_STENCIL_FUNC_LESS:
        return GL_LESS;
    case SCE_GXM_STENCIL_FUNC_EQUAL:
        return GL_EQUAL;
    case SCE_GXM_STENCIL_FUNC_LESS_EQUAL:
        return GL_LEQUAL;
    case SCE_GXM_STENCIL_FUNC_GREATER:
        return GL_GREATER;
    case SCE_GXM_STENCIL_FUNC_NOT_EQUAL:
        return GL_NOTEQUAL;
    case SCE_GXM_STENCIL_FUNC_GREATER_EQUAL:
        return GL_GEQUAL;
    case SCE_GXM_STENCIL_FUNC_ALWAYS:
        return GL_ALWAYS;
    }

    return GL_ALWAYS;
}

void sync_mask(const GLState &state, GLContext &context, const MemState &mem) {
    auto control = context.record.depth_stencil_surface.control.content;
    // mask is not upscaled
    auto width = context.render_target->width / state.res_multiplier;
    auto height = context.render_target->height / state.res_multiplier;
    GLubyte initial_byte = (control & SceGxmDepthStencilControl::mask_bit) ? 0xFF : 0;

    GLubyte clear_bytes[4] = { initial_byte, initial_byte, initial_byte, initial_byte };
    glClearTexImage(context.render_target->masktexture[0], 0, GL_RGBA, GL_UNSIGNED_BYTE, clear_bytes);
}

void sync_viewport_flat(const GLState &state, GLContext &context) {
    const GLsizei display_w = context.record.color_surface.width;
    const GLsizei display_h = context.record.color_surface.height;

    glViewport(0, (context.current_framebuffer_height - display_h) * state.res_multiplier, display_w * state.res_multiplier, display_h * state.res_multiplier);
    glDepthRange(0, 1);
}

void sync_viewport_real(const GLState &state, GLContext &context, const float xOffset, const float yOffset, const float zOffset,
    const float xScale, const float yScale, const float zScale) {
    const GLfloat ymin = yOffset + yScale;
    const GLfloat ymax = yOffset - yScale - 1;

    const GLfloat w = std::abs(2 * xScale);
    const GLfloat h = std::abs(2 * yScale);
    const GLfloat x = xOffset - std::abs(xScale);
    const GLfloat y = std::min<GLfloat>(ymin, ymax);

    glViewportIndexedf(0, x * state.res_multiplier, y * state.res_multiplier, w * state.res_multiplier, h * state.res_multiplier);
    glDepthRange(0, 1);
}

void sync_clipping(const GLState &state, GLContext &context) {
    const GLsizei display_h = context.current_framebuffer_height;
    const GLsizei scissor_x = context.record.region_clip_min.x;
    GLsizei scissor_y = 0;

    if (context.record.viewport_flip[1] == -1.0f)
        scissor_y = context.record.region_clip_min.y;
    else
        scissor_y = display_h - context.record.region_clip_max.y - 1;

    const GLsizei scissor_w = context.record.region_clip_max.x - context.record.region_clip_min.x + 1;
    const GLsizei scissor_h = context.record.region_clip_max.y - context.record.region_clip_min.y + 1;

    switch (context.record.region_clip_mode) {
    case SCE_GXM_REGION_CLIP_NONE:
        glDisable(GL_SCISSOR_TEST);
        break;
    case SCE_GXM_REGION_CLIP_ALL:
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 0, 0);
        break;
    case SCE_GXM_REGION_CLIP_OUTSIDE:
        glEnable(GL_SCISSOR_TEST);
        glScissor(scissor_x * state.res_multiplier, scissor_y * state.res_multiplier, scissor_w * state.res_multiplier, scissor_h * state.res_multiplier);
        break;
    case SCE_GXM_REGION_CLIP_INSIDE:
        // TODO: Implement SCE_GXM_REGION_CLIP_INSIDE
        glDisable(GL_SCISSOR_TEST);
        LOG_WARN("Unimplemented region clip mode used: SCE_GXM_REGION_CLIP_INSIDE");
        break;
    }
}

void sync_cull(const GxmRecordState &state) {
    // Culling.
    switch (state.cull_mode) {
    case SCE_GXM_CULL_CCW:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        break;
    case SCE_GXM_CULL_CW:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        break;
    case SCE_GXM_CULL_NONE:
        glDisable(GL_CULL_FACE);
        break;
    }
}

void sync_depth_func(const SceGxmDepthFunc func, const bool is_front) {
    if (is_front)
        glDepthFunc(translate_depth_func(func));
}

void sync_depth_write_enable(const SceGxmDepthWriteMode mode, const bool is_front) {
    if (is_front)
        glDepthMask(mode == SCE_GXM_DEPTH_WRITE_ENABLED ? GL_TRUE : GL_FALSE);
}

void sync_depth_data(const renderer::GxmRecordState &state) {
    // Depth test.
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    // If force load is enabled to load saved depth and depth data memory exists (the second condition is just for safe, may sometimes contradict its usefulness, hopefully won't)
    if (((state.depth_stencil_surface.zlsControl & SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED) == 0) && (state.depth_stencil_surface.depthData)) {
        glClearDepth(state.depth_stencil_surface.backgroundDepth);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
}

void sync_stencil_func(const GxmStencilStateOp &state_op, const GxmStencilStateValues &state_vals, const MemState &mem, const bool is_back_stencil) {
    const GLenum face = is_back_stencil ? GL_BACK : GL_FRONT;

    glStencilOpSeparate(face,
        translate_stencil_op(state_op.stencil_fail),
        translate_stencil_op(state_op.depth_fail),
        translate_stencil_op(state_op.depth_pass));
    glStencilFuncSeparate(face, translate_stencil_func(state_op.func), state_vals.ref, state_vals.compare_mask);
    glStencilMaskSeparate(face, state_vals.write_mask);
}

void sync_stencil_data(const GxmRecordState &state, const MemState &mem) {
    // Stencil test.
    glEnable(GL_STENCIL_TEST);
    glStencilMask(GL_TRUE);
    if ((state.depth_stencil_surface.zlsControl & SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED) == 0) {
        glClearStencil(state.depth_stencil_surface.control.content & SceGxmDepthStencilControl::stencil_bits);
        glClear(GL_STENCIL_BUFFER_BIT);
    }
}

void sync_polygon_mode(const SceGxmPolygonMode mode, const bool front) {
    // TODO: Why decap this?
    const GLint face = GL_FRONT_AND_BACK;

    // Polygon Mode.
    switch (mode) {
    case SCE_GXM_POLYGON_MODE_POINT_10UV:
    case SCE_GXM_POLYGON_MODE_POINT:
    case SCE_GXM_POLYGON_MODE_POINT_01UV:
    case SCE_GXM_POLYGON_MODE_TRIANGLE_POINT:
        glPolygonMode(face, GL_POINT);
        break;
    case SCE_GXM_POLYGON_MODE_LINE:
    case SCE_GXM_POLYGON_MODE_TRIANGLE_LINE:
        glPolygonMode(face, GL_LINE);
        break;
    case SCE_GXM_POLYGON_MODE_TRIANGLE_FILL:
        glPolygonMode(face, GL_FILL);
        break;
    }
}

void sync_point_line_width(const std::uint32_t width, const bool is_front) {
    // Point Line Width
    if (is_front) {
        glLineWidth(static_cast<GLfloat>(width));
        glPointSize(static_cast<GLfloat>(width));
    }
}

void sync_depth_bias(const int factor, const int unit, const bool is_front) {
    // Depth Bias
    if (is_front) {
        glPolygonOffset(static_cast<GLfloat>(factor), static_cast<GLfloat>(unit));
    }
}

void sync_texture(GLState &state, GLContext &context, MemState &mem, std::size_t index, SceGxmTexture texture,
    const Config &config, const std::string &base_path, const std::string &title_id) {
    Address data_addr = texture.data_addr << 2;

    const size_t texture_size = renderer::texture::texture_size(texture);
    if (!is_valid_addr_range(mem, data_addr, data_addr + texture_size)) {
        LOG_WARN("Texture has freed data.");
        return;
    }

    const SceGxmTextureFormat format = gxm::get_format(&texture);
    const SceGxmTextureBaseFormat base_format = gxm::get_base_format(format);
    if (gxm::is_paletted_format(base_format) && texture.palette_addr == 0) {
        LOG_WARN("Ignoring null palette texture");
        return;
    }

    if (index >= SCE_GXM_MAX_TEXTURE_UNITS) {
        // Vertex textures
        context.shader_hints.vertex_textures[index - SCE_GXM_MAX_TEXTURE_UNITS] = format;
    } else {
        context.shader_hints.fragment_textures[index] = format;
    }

    glActiveTexture(static_cast<GLenum>(static_cast<std::size_t>(GL_TEXTURE0) + index));

    std::uint64_t texture_as_surface = 0;
    const GLint *swizzle_surface = nullptr;
    bool only_nearest = false;

    if (context.record.color_surface.data.address() == data_addr) {
        texture_as_surface = context.current_color_attachment;
        swizzle_surface = color::translate_swizzle(context.record.color_surface.colorFormat);

        if (std::find(context.self_sampling_indices.begin(), context.self_sampling_indices.end(), index)
            == context.self_sampling_indices.end()) {
            context.self_sampling_indices.push_back(index);
        }
    } else {
        auto res = std::find(context.self_sampling_indices.begin(), context.self_sampling_indices.end(),
            static_cast<GLuint>(index));
        if (res != context.self_sampling_indices.end()) {
            context.self_sampling_indices.erase(res);
        }

        SceGxmColorBaseFormat format_target_of_texture;

        std::uint16_t width = static_cast<std::uint16_t>(gxm::get_width(&texture));
        std::uint16_t height = static_cast<std::uint16_t>(gxm::get_height(&texture));

        if (renderer::texture::convert_base_texture_format_to_base_color_format(base_format, format_target_of_texture)) {
            std::uint16_t stride_in_pixels = width;

            switch (texture.texture_type()) {
            case SCE_GXM_TEXTURE_LINEAR_STRIDED:
                stride_in_pixels = static_cast<std::uint16_t>(gxm::get_stride_in_bytes(&texture)) / ((renderer::texture::bits_per_pixel(base_format) + 7) >> 3);
                break;
            case SCE_GXM_TEXTURE_LINEAR:
                // when the texture is linear, the stride should be aligned to 8 pixels
                stride_in_pixels = align(stride_in_pixels, 8);
                break;
            case SCE_GXM_TEXTURE_TILED:
                // tiles are 32x32
                stride_in_pixels = align(stride_in_pixels, 32);
                break;
            }

            std::uint32_t swizz_raw = 0;

            texture_as_surface = state.surface_cache.retrieve_color_surface_texture_handle(
                state, width, height, stride_in_pixels, format_target_of_texture, Ptr<void>(data_addr),
                renderer::SurfaceTextureRetrievePurpose::READING, swizz_raw);

            swizzle_surface = color::translate_swizzle(static_cast<SceGxmColorFormat>(format_target_of_texture | swizz_raw));
            only_nearest = color::is_write_surface_non_linearity_filtering(format_target_of_texture);
        }

        // Try to retrieve S24D8 texture
        if (!texture_as_surface) {
            SceGxmDepthStencilSurface lookup_temp;
            lookup_temp.depthData = data_addr;
            lookup_temp.stencilData.reset();

            texture_as_surface = state.surface_cache.retrieve_depth_stencil_texture_handle(state, mem, lookup_temp, width, height, true);
            if (texture_as_surface) {
                only_nearest = true;
            }
        }
    }

    if (texture_as_surface != 0) {
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_as_surface));

        if (only_nearest) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }

        if (base_format != SCE_GXM_TEXTURE_BASE_FORMAT_X8U24) {
            const GLint *swizzle = texture::translate_swizzle(format);

            if (swizzle) {
                if (swizzle_surface) {
                    if (std::memcmp(swizzle_surface, swizzle, 16) != 0) {
                        // Surface is stored in RGBA in GPU memory, unless in other circumstances. So we must reverse order
                        for (int i = 0; i < 4; i++) {
                            if ((swizzle[i] < GL_RED) || (swizzle[i] > GL_ALPHA)) {
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R + i, swizzle[i]);
                            } else {
                                for (int j = 0; j < 4; j++) {
                                    if (swizzle[i] == swizzle_surface[j]) {
                                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R + i, GL_RED + j);
                                        break;
                                    }
                                }
                            }
                        }
                    } else {
                        const GLint default_rgba[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
                        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, default_rgba);
                    }
                } else {
                    LOG_TRACE("No surface swizzle found, use default texture swizzle");
                    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
                }
            }
        }
    } else {
        if (config.texture_cache) {
            renderer::texture::cache_and_bind_texture(state.texture_cache, texture, mem);
        } else {
            texture::bind_texture(state.texture_cache, texture, mem);
        }
    }

    if (config.dump_textures) {
        auto frag_program = context.record.fragment_program.get(mem);
        auto program = frag_program->program.get(mem);
        const auto program_hash = sha256(program, program->size);

        std::string parameter_name;
        const auto parameters = gxp::program_parameters(*program);
        for (uint32_t i = 0; i < program->parameter_count; ++i) {
            const auto parameter = &parameters[i];
            if (parameter->resource_index == index) {
                parameter_name = gxp::parameter_name_raw(*parameter);
                break;
            }
        }
        renderer::gl::texture::dump(texture, mem, parameter_name, base_path, title_id, program_hash);
    }

    glActiveTexture(GL_TEXTURE0);
}

void sync_blending(const GxmRecordState &state, const MemState &mem) {
    // Blending.
    const SceGxmFragmentProgram &gxm_fragment_program = *state.fragment_program.get(mem);
    const GLFragmentProgram &fragment_program = *reinterpret_cast<GLFragmentProgram *>(
        gxm_fragment_program.renderer_data.get());

    glColorMask(fragment_program.color_mask_red, fragment_program.color_mask_green, fragment_program.color_mask_blue, fragment_program.color_mask_alpha);
    if (fragment_program.blend_enabled) {
        glEnable(GL_BLEND);
        glBlendEquationSeparate(fragment_program.color_func, fragment_program.alpha_func);
        glBlendFuncSeparate(fragment_program.color_src, fragment_program.color_dst, fragment_program.alpha_src, fragment_program.alpha_dst);
    } else {
        glDisable(GL_BLEND);
    }
}

void clear_previous_uniform_storage(GLContext &context) {
    context.vertex_uniform_buffer_storage_ptr.first = nullptr;
    context.vertex_uniform_buffer_storage_ptr.second = 0;
    context.fragment_uniform_buffer_storage_ptr.first = nullptr;
    context.fragment_uniform_buffer_storage_ptr.second = 0;
}

void sync_vertex_streams_and_attributes(GLContext &context, GxmRecordState &state, const MemState &mem) {
    // Vertex attributes.
    const SceGxmVertexProgram &vertex_program = *state.vertex_program.get(mem);
    GLVertexProgram *glvert = reinterpret_cast<GLVertexProgram *>(vertex_program.renderer_data.get());

    if (!glvert->stripped_symbols_checked) {
        // Insert some symbols here
        const SceGxmProgram *vertex_program_body = vertex_program.program.get(mem);
        if (vertex_program_body && (vertex_program_body->primary_reg_count != 0)) {
            for (std::size_t i = 0; i < vertex_program.attributes.size(); i++) {
                glvert->attribute_infos.emplace(vertex_program.attributes[i].regIndex, shader::usse::AttributeInformation(static_cast<std::uint16_t>(i), SCE_GXM_PARAMETER_TYPE_F32, false, false, false));
            }
        }

        glvert->stripped_symbols_checked = true;
    }

    // Each draw will upload the stream data. Assuming that, we can just bind buffer, upload data
    // The GXM submit side should already submit used buffer, but we just delete all just in case
    std::array<std::size_t, SCE_GXM_MAX_VERTEX_STREAMS> offset_in_buffer;
    for (std::size_t i = 0; i < SCE_GXM_MAX_VERTEX_STREAMS; i++) {
        if (state.vertex_streams[i].data) {
            std::pair<std::uint8_t *, std::size_t> result = context.vertex_stream_ring_buffer.allocate(state.vertex_streams[i].size);
            if (!result.first) {
                LOG_ERROR("Failed to allocate vertex stream data from GPU!");
            } else {
                std::memcpy(result.first, state.vertex_streams[i].data.get(mem), state.vertex_streams[i].size);
                offset_in_buffer[i] = result.second;
            }

            state.vertex_streams[i].data = nullptr;
            state.vertex_streams[i].size = 0;
        } else {
            offset_in_buffer[i] = 0;
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, context.vertex_stream_ring_buffer.handle());

    for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {
        if (glvert->attribute_infos.find(attribute.regIndex) == glvert->attribute_infos.end())
            continue;

        const SceGxmVertexStream &stream = vertex_program.streams[attribute.streamIndex];

        const SceGxmAttributeFormat attribute_format = static_cast<SceGxmAttributeFormat>(attribute.format);
        GLenum type = attribute_format_to_gl_type(attribute_format);
        const GLboolean normalised = attribute_format_normalised(attribute_format);

        int attrib_location = 0;
        bool upload_integral = false;

        shader::usse::AttributeInformation info = glvert->attribute_infos.at(attribute.regIndex);
        attrib_location = info.location();

        // these 2 values are only used when a matrix is used as a vertex attribute
        // this is only supported for regformated attribute for now
        uint32_t array_size = 1;
        uint32_t array_element_size = 0;
        uint8_t component_count = attribute.componentCount;

        if (info.regformat) {
            const int comp_size = gxm::attribute_format_size(attribute_format);
            component_count = (comp_size * component_count + 3) / 4;
            type = GL_INT;
            upload_integral = true;

            if (component_count > 4) {
                // a matrix is used as an attribute, pack everything into an array of vec4
                array_size = (component_count + 3) / 4;
                array_element_size = 4 * sizeof(int32_t);
                component_count = 4;
            }
        } else {
            switch (info.gxm_type()) {
            case SCE_GXM_PARAMETER_TYPE_U8:
            case SCE_GXM_PARAMETER_TYPE_S8:
            case SCE_GXM_PARAMETER_TYPE_U16:
            case SCE_GXM_PARAMETER_TYPE_S16:
            case SCE_GXM_PARAMETER_TYPE_U32:
            case SCE_GXM_PARAMETER_TYPE_S32:
                upload_integral = true;
                break;
            default:
                break;
            }
        }

        const std::uint16_t stream_index = attribute.streamIndex;

        for (int i = 0; i < array_size; i++) {
            if (upload_integral || (attribute_format == SCE_GXM_ATTRIBUTE_FORMAT_UNTYPED)) {
                glVertexAttribIPointer(attrib_location + i, component_count, type, stream.stride, reinterpret_cast<const GLvoid *>(i * array_element_size + attribute.offset + offset_in_buffer[stream_index]));
            } else {
                glVertexAttribPointer(attrib_location + i, component_count, type, normalised, stream.stride, reinterpret_cast<const GLvoid *>(i * array_element_size + attribute.offset + offset_in_buffer[stream_index]));
            }

            glEnableVertexAttribArray(attrib_location + i);

            if (gxm::is_stream_instancing(static_cast<SceGxmIndexSource>(stream.indexSource))) {
                glVertexAttribDivisor(attrib_location + i, 1);
            } else {
                glVertexAttribDivisor(attrib_location + i, 0);
            }
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
} // namespace renderer::gl
