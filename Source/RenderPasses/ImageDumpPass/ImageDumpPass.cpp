/***************************************************************************
 # Copyright (c) 2015-21, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "ImageDumpPass.h"

const RenderPass::Info ImageDumpPass::kInfo { "ImageDumpPass", "Dump Image to file." };


namespace
{
    const char kDst[] = "dst";
    const char kSrc[] = "src";
    const char kImageName[] = "image_name";
    const char kDumpAlpha[] = "alpha";

    void regImageDumpPass(pybind11::module& m)
    {
        pybind11::class_<ImageDumpPass, RenderPass, ImageDumpPass::SharedPtr> pass(m, "ImageDumpPass");
        //pass.def_property(kFilter, &ImageDumpPass::getFilter, &ImageDumpPass::setFilter);
    }
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerPass(ImageDumpPass::kInfo, ImageDumpPass::create);
    ScriptBindings::registerBinding(regImageDumpPass);
}

RenderPassReflection ImageDumpPass::reflect(const CompileData& compileData)
{
    RenderPassReflection r;
    r.addInput(kSrc, "The source texture");
    r.addOutput(kDst, "The destination texture");
    return r;
}

ImageDumpPass::SharedPtr ImageDumpPass::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new ImageDumpPass(dict));
    return pPass;
}

Dictionary ImageDumpPass::getScriptingDictionary()
{
    Dictionary dict;
    dict[kImageName] = imageName;
    dict[kDumpAlpha] = alpha;
    return dict;
}


void dumpTexture(ConstTextureSharedPtrRef pTex, std::string filename, bool bDumpAlpha) 
{
    const ResourceFormat format = pTex->getFormat();
    const uint32_t channels = getFormatChannelCount(format);


    auto ext = Bitmap::getFileExtFromResourceFormat(pTex->getFormat());
    auto fileformat = Bitmap::getFormatFromFileExtension(ext);
    std::string totalFileName = filename + "." + ext;
    Bitmap::ExportFlags flags = Bitmap::ExportFlags::None;
    if (bDumpAlpha) flags |= Bitmap::ExportFlags::ExportAlpha;

    pTex->captureToFile(0, 0, totalFileName, fileformat, flags);
}

void ImageDumpPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    CHAR frameId_str[100];
    CHAR spp_str[100];
    CHAR sample_no_str[100];

    GetEnvironmentVariableA("IKSDE_FRAME_ID", frameId_str, 50);
    int frameID = std::atoi(frameId_str);

    GetEnvironmentVariableA("IKSDE_SPP", spp_str, 50);
    int spp = std::atoi(spp_str);

    GetEnvironmentVariableA("IKSDE_SAMPLE_NO", sample_no_str, 50);
    int sample_no = std::atoi(sample_no_str);

    bool isMotionVectorPass = imageName[0] == 'M';

    // renderData holds the requested resources
    if ((frameID >= 1 && !isMotionVectorPass && sample_no == spp) ||
        (frameID >= 1 && isMotionVectorPass && sample_no == 1) ) {
        const auto& pTex = renderData["src"]->asTexture();

        // Write output image.

        std::string basename = "C:/Users/Wojtas/Desktop/AMD/Falcor/dumps/" + imageName + std::to_string(frameID);

        Bitmap::ExportFlags flags = Bitmap::ExportFlags::None;
        //if (mask == TextureChannelFlags::RGBA) flags |= Bitmap::ExportFlags::ExportAlpha;
        std::filesystem::path totalPath = basename;
        auto parentPath = totalPath.parent_path();
        if (!is_directory(parentPath))
        {
            create_directories(parentPath);
        }
        dumpTexture(pTex, basename, alpha);
    }
    if(isMotionVectorPass)
        logInfo(frameId_str + std::string(" ") + sample_no_str);
}

void ImageDumpPass::renderUI(Gui::Widgets& widget)
{
    widget.textbox("Image name", imageName);
    widget.checkbox("Dump alpha", alpha);
}

ImageDumpPass::ImageDumpPass(const Dictionary& dict) : RenderPass(kInfo)
{
    for (const auto& [key, value] : dict)
    {
        if (key == kImageName)
        {
            imageName = value.operator std::string();
        }
        else if (key == kDumpAlpha)
        {
            alpha = value;
        }
        else {
            logWarning("Unknown field '{}' in a ImageLoader dictionary.", key);
        }
    }
}
