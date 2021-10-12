/****************************************************************************
MIT License

Copyright (c) 2021 Guillaume Boissé

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
****************************************************************************/
#include "ibl.h"

#define _USE_MATH_DEFINES
#include "math.h"
#include "glm/gtc/matrix_transform.hpp"

namespace
{

glm::dvec3 const forward_vectors[] =
{
    glm::dvec3(-1.0,  0.0,  0.0),
    glm::dvec3( 1.0,  0.0,  0.0),
    glm::dvec3( 0.0,  1.0,  0.0),
    glm::dvec3( 0.0, -1.0,  0.0),
    glm::dvec3( 0.0,  0.0, -1.0),
    glm::dvec3( 0.0,  0.0,  1.0)
};

glm::dvec3 const up_vectors[] =
{
    glm::dvec3( 0.0, -1.0,  0.0),
    glm::dvec3( 0.0, -1.0,  0.0),
    glm::dvec3( 0.0,  0.0, -1.0),
    glm::dvec3( 0.0,  0.0,  1.0),
    glm::dvec3( 0.0, -1.0,  0.0),
    glm::dvec3( 0.0, -1.0,  0.0)
};

} //! Unnamed namespace

IBL ConvolveIBL(GfxContext gfx, GfxTexture environment_buffer)
{
    IBL ibl = {};

    if(environment_buffer)
    {
        uint32_t const cubemap_size    = 512;
        uint32_t const mip_level_count = gfxCalculateMipCount(cubemap_size);

        GfxTexture      cubemap        = gfxCreateTextureCube(gfx, cubemap_size, DXGI_FORMAT_R16G16B16A16_FLOAT, mip_level_count);
        GfxProgram      ibl_program    = gfxCreateProgram(gfx, "ibl");
        GfxSamplerState linear_sampler = gfxCreateSamplerState(gfx, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

        ibl.irradiance_buffer = gfxCreateTextureCube(gfx, 32, DXGI_FORMAT_R16G16B16A16_FLOAT);

        gfxProgramSetParameter(gfx, ibl_program, "g_Cubemap", cubemap);
        gfxProgramSetParameter(gfx, ibl_program, "g_EnvironmentBuffer", environment_buffer);

        gfxProgramSetParameter(gfx, ibl_program, "g_LinearSampler", linear_sampler);

        // Draw the environment buffer into a cubemap
        GfxKernel draw_cubemap_kernel = gfxCreateComputeKernel(gfx, ibl_program, "DrawCubemap");
        {
            gfxCommandBindKernel(gfx, draw_cubemap_kernel);

            uint32_t const *num_threads = gfxKernelGetNumThreads(gfx, draw_cubemap_kernel);
            uint32_t const num_groups_x = (cubemap_size + num_threads[0] - 1) / num_threads[0];
            uint32_t const num_groups_y = (cubemap_size + num_threads[1] - 1) / num_threads[1];

            for(uint32_t face_index = 0; face_index < 6; ++face_index)
            {
                glm::dmat4 const view          = glm::lookAt(glm::dvec3(0.0), forward_vectors[face_index], up_vectors[face_index]);
                glm::dmat4 const proj          = glm::perspective(M_PI / 2.0, 1.0, 0.1, 1e4);
                glm::mat4  const view_proj_inv = glm::mat4(glm::inverse(proj * view));

                gfxProgramSetParameter(gfx, ibl_program, "g_FaceIndex", face_index);
                gfxProgramSetParameter(gfx, ibl_program, "g_ViewProjectionInverse", view_proj_inv);

                gfxProgramSetParameter(gfx, ibl_program, "g_OutCubemap", cubemap);

                gfxCommandDispatch(gfx, num_groups_x, num_groups_y, 1);
            }

            gfxDestroyKernel(gfx, draw_cubemap_kernel);
        }

        // Blur the cubemap to generate its mip levels
        GfxKernel blur_cubemap_kernel = gfxCreateComputeKernel(gfx, ibl_program, "BlurCubemap");
        {
            gfxCommandBindKernel(gfx, blur_cubemap_kernel);

            uint32_t const *num_threads = gfxKernelGetNumThreads(gfx, blur_cubemap_kernel);
            uint32_t mip_level_size     = GFX_MAX(cubemap_size >> 1, 1u);

            for(uint32_t mip_level = 1; mip_level < mip_level_count; ++mip_level)
            {
                uint32_t const num_groups_x = (mip_level_size + num_threads[0] - 1) / num_threads[0];
                uint32_t const num_groups_y = (mip_level_size + num_threads[1] - 1) / num_threads[1];

                gfxProgramSetParameter(gfx, ibl_program, "g_InCubemap", cubemap, mip_level - 1);
                gfxProgramSetParameter(gfx, ibl_program, "g_OutCubemap", cubemap, mip_level);

                gfxCommandDispatch(gfx, num_groups_x, num_groups_y, 6);

                mip_level_size = GFX_MAX(mip_level_size >> 1, 1u);
            }

            gfxDestroyKernel(gfx, blur_cubemap_kernel);
        }

        // Convolve the cubemap to pre-calculate irradiance
        GfxKernel draw_irradiance_kernel = gfxCreateComputeKernel(gfx, ibl_program, "DrawIrradiance");
        {
            gfxCommandBindKernel(gfx, draw_irradiance_kernel);

            uint32_t const *num_threads = gfxKernelGetNumThreads(gfx, draw_irradiance_kernel);
            uint32_t const num_groups_x = (ibl.irradiance_buffer.getWidth()  + num_threads[0] - 1) / num_threads[0];
            uint32_t const num_groups_y = (ibl.irradiance_buffer.getHeight() + num_threads[1] - 1) / num_threads[1];

            for(uint32_t face_index = 0; face_index < 6; ++face_index)
            {
                glm::dmat4 const view          = glm::lookAt(glm::dvec3(0.0), forward_vectors[face_index], up_vectors[face_index]);
                glm::dmat4 const proj          = glm::perspective(M_PI / 2.0, 1.0, 0.1, 1e4);
                glm::mat4  const view_proj_inv = glm::mat4(glm::inverse(proj * view));

                gfxProgramSetParameter(gfx, ibl_program, "g_FaceIndex", face_index);
                gfxProgramSetParameter(gfx, ibl_program, "g_ViewProjectionInverse", view_proj_inv);

                gfxProgramSetParameter(gfx, ibl_program, "g_IrradianceBuffer", ibl.irradiance_buffer);

                gfxCommandDispatch(gfx, num_groups_x, num_groups_y, 1);
            }

            gfxDestroyKernel(gfx, draw_irradiance_kernel);
        }

        gfxDestroyTexture(gfx, cubemap);
        gfxDestroyProgram(gfx, ibl_program);
        gfxDestroySamplerState(gfx, linear_sampler);
    }

    return ibl;
}

void ReleaseIBL(GfxContext gfx, IBL const &ibl)
{
    gfxDestroyTexture(gfx, ibl.irradiance_buffer);
}
