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
#include "stdafx.h"
#include "Core/Program/ProgramVars.h"
#include "Core/Program/GraphicsProgram.h"
#include "Core/Program/ComputeProgram.h"
#include "Core/API/ComputeContext.h"
#include "Core/API/RenderContext.h"
#include "Core/API/D3D12/D3D12RootSignature.h"

#include <slang/slang.h>

namespace Falcor
{
    static bool compareRootSets(const D3D12DescriptorSet::Layout& a, const D3D12DescriptorSet::Layout& b)
    {
        if (a.getRangeCount() != b.getRangeCount()) return false;
        if (a.getVisibility() != b.getVisibility()) return false;
        for (uint32_t i = 0; i < a.getRangeCount(); i++)
        {
            const auto& rangeA = a.getRange(i);
            const auto& rangeB = b.getRange(i);
            if (rangeA.baseRegIndex != rangeB.baseRegIndex) return false;
            if (rangeA.descCount != rangeB.descCount) return false;
#ifdef FALCOR_D3D12
            if (rangeA.regSpace != rangeB.regSpace) return false;
#endif
            if (rangeA.type != rangeB.type) return false;
        }
        return true;
    }

    ProgramVars::ProgramVars(
        const ProgramReflection::SharedConstPtr& pReflector)
        : ParameterBlock(pReflector->getProgramVersion(), pReflector->getDefaultParameterBlock())
        , mpReflector(pReflector)
    {
        assert(pReflector);
    }

    template<bool forGraphics>
    void bindRootSet(D3D12DescriptorSet::SharedPtr const& pSet, CopyContext* pContext, D3D12RootSignature* pRootSignature, uint32_t rootIndex)
    {
        if (forGraphics)
        {
            pSet->bindForGraphics(pContext, pRootSignature, rootIndex);
        }
        else
        {
            pSet->bindForCompute(pContext, pRootSignature, rootIndex);
        }
    }

    template<bool forGraphics>
    void bindRootDescriptor(CopyContext* pContext, uint32_t rootIndex, const Resource::SharedPtr& pResource, bool isUav)
    {
        auto pBuffer = pResource->asBuffer();
        assert(!pResource || pBuffer); // If a resource is bound, it must be a buffer
        uint64_t gpuAddress = pBuffer ? pBuffer->getGpuAddress() : 0;

        if (forGraphics)
        {
            if (isUav)
                pContext->getLowLevelData()->getCommandList()->SetGraphicsRootUnorderedAccessView(rootIndex, gpuAddress);
            else
                pContext->getLowLevelData()->getCommandList()->SetGraphicsRootShaderResourceView(rootIndex, gpuAddress);
        }
        else
        {
            if (isUav)
                pContext->getLowLevelData()->getCommandList()->SetComputeRootUnorderedAccessView(rootIndex, gpuAddress);
            else
                pContext->getLowLevelData()->getCommandList()->SetComputeRootShaderResourceView(rootIndex, gpuAddress);
        }
    }

    template<bool forGraphics>
    void bindRootConstants(CopyContext* pContext, uint32_t rootIndex, ParameterBlock* pParameterBlock, const ParameterBlockReflection* pParameterBlockReflector)
    {
        uint32_t count = uint32_t(pParameterBlockReflector->getElementType()->getByteSize() / sizeof(uint32_t));
        void const* pSrc = pParameterBlock->getRawData();
        if (forGraphics)
        {
            pContext->getLowLevelData()->getCommandList()->SetGraphicsRoot32BitConstants(
                rootIndex,
                count,
                pSrc,
                0);
        }
        else
        {
            pContext->getLowLevelData()->getCommandList()->SetComputeRoot32BitConstants(
                rootIndex,
                count,
                pSrc,
                0);
        }
    }

    template<bool forGraphics>
    bool bindParameterBlockSets(
        ParameterBlock*                 pParameterBlock,
        const ParameterBlockReflection* pParameterBlockReflector,
        CopyContext*                    pContext,
        D3D12RootSignature*             pRootSignature,
        bool                            bindRootSig,
        uint32_t&                       descSetIndex,
        uint32_t&                       rootConstIndex)
    {
        auto defaultConstantBufferInfo = pParameterBlockReflector->getDefaultConstantBufferBindingInfo();
        if( defaultConstantBufferInfo.useRootConstants )
        {
            uint32_t rootIndex = rootConstIndex++;

            bindRootConstants<forGraphics>(pContext, rootIndex, pParameterBlock, pParameterBlockReflector);
        }

        auto descriptorSetCount = pParameterBlockReflector->getD3D12DescriptorSetCount();
        for(uint32_t s = 0; s < descriptorSetCount; ++s)
        {
            auto pSet = pParameterBlock->getD3D12DescriptorSet(s);

            uint32_t rootIndex = descSetIndex++;

            bindRootSet<forGraphics>(pSet, pContext, pRootSignature, rootIndex);
        }

        // Iterate over parameter blocks to recursively bind their descriptor sets.
        auto parameterBlockRangeCount = pParameterBlockReflector->getParameterBlockSubObjectRangeCount();
        for(uint32_t i = 0; i < parameterBlockRangeCount; ++i)
        {
            auto resourceRangeIndex = pParameterBlockReflector->getParameterBlockSubObjectRangeIndex(i);
            auto& resourceRange = pParameterBlockReflector->getResourceRange(resourceRangeIndex);
            auto& bindingInfo = pParameterBlockReflector->getResourceRangeBindingInfo(resourceRangeIndex);

            auto pSubObjectReflector = bindingInfo.pSubObjectReflector;
            auto objectCount = resourceRange.count;

            for(uint32_t i = 0; i < objectCount; ++i)
            {
                auto pSubBlock = pParameterBlock->getParameterBlock(resourceRangeIndex, i);
                if(!bindParameterBlockSets<forGraphics>(pSubBlock.get(), pSubObjectReflector.get(), pContext, pRootSignature, bindRootSig, descSetIndex, rootConstIndex))
                {
                    return false;
                }
            }
        }

        return true;
    }

    template<bool forGraphics>
    bool bindParameterBlockRootDescs(
        ParameterBlock*                 pParameterBlock,
        const ParameterBlockReflection* pParameterBlockReflector,
        CopyContext*                    pContext,
        D3D12RootSignature*             pRootSignature,
        bool                            bindRootSig,
        uint32_t&                       rootDescIndex)
    {
        auto rootDescriptorRangeCount = pParameterBlockReflector->getRootDescriptorRangeCount();
        for (uint32_t i = 0; i < rootDescriptorRangeCount; ++i)
        {
            auto resourceRangeIndex = pParameterBlockReflector->getRootDescriptorRangeIndex(i);
            auto& resourceRange = pParameterBlockReflector->getResourceRange(resourceRangeIndex);

            assert(resourceRange.count == 1); // Root descriptors cannot be arrays
            auto [pResource, isUav] = pParameterBlock->getRootDescriptor(resourceRangeIndex, 0);

            bindRootDescriptor<forGraphics>(pContext, rootDescIndex++, pResource, isUav);
        }

        // Iterate over constant buffers and parameter blocks to recursively bind their root descriptors.
        uint32_t resourceRangeCount = pParameterBlockReflector->getResourceRangeCount();
        for (uint32_t resourceRangeIndex = 0; resourceRangeIndex < resourceRangeCount; ++resourceRangeIndex)
        {
            auto& resourceRange = pParameterBlockReflector->getResourceRange(resourceRangeIndex);
            auto& bindingInfo = pParameterBlockReflector->getResourceRangeBindingInfo(resourceRangeIndex);

            if (bindingInfo.flavor != ParameterBlockReflection::ResourceRangeBindingInfo::Flavor::ConstantBuffer &&
                bindingInfo.flavor != ParameterBlockReflection::ResourceRangeBindingInfo::Flavor::ParameterBlock)
                continue;

            auto pSubObjectReflector = bindingInfo.pSubObjectReflector;
            auto objectCount = resourceRange.count;

            for (uint32_t i = 0; i < objectCount; ++i)
            {
                auto pSubBlock = pParameterBlock->getParameterBlock(resourceRangeIndex, i);
                if (!bindParameterBlockRootDescs<forGraphics>(pSubBlock.get(), pSubObjectReflector.get(), pContext, pRootSignature, bindRootSig, rootDescIndex))
                {
                    return false;
                }
            }
        }

        return true;
    }

    template<bool forGraphics>
    bool bindRootSetsCommon(ParameterBlock* pVars, CopyContext* pContext, bool bindRootSig, D3D12RootSignature* pRootSignature)
    {
        if(!pVars->prepareDescriptorSets(pContext)) return false;

        uint32_t descSetIndex = pRootSignature->getDescriptorSetBaseIndex();
        uint32_t rootDescIndex = pRootSignature->getRootDescriptorBaseIndex();
        uint32_t rootConstIndex = pRootSignature->getRootConstantBaseIndex();

        if (!bindParameterBlockSets<forGraphics>(pVars, pVars->getSpecializedReflector().get(), pContext, pRootSignature, bindRootSig, descSetIndex, rootConstIndex)) return false;
        if (!bindParameterBlockRootDescs<forGraphics>(pVars, pVars->getSpecializedReflector().get(), pContext, pRootSignature, bindRootSig, rootDescIndex)) return false;

        return true;
    }

    template<bool forGraphics>
    bool applyProgramVarsCommon(ParameterBlock* pVars, CopyContext* pContext, bool bindRootSig, const ProgramKernels* pProgramKernels)
    {
        auto pRootSignature = pProgramKernels->getD3D12RootSignature().get();

        if (bindRootSig)
        {
            if (forGraphics)
            {
                pRootSignature->bindForGraphics(pContext);
            }
            else
            {
                pRootSignature->bindForCompute(pContext);
            }
        }

        return bindRootSetsCommon<forGraphics>(pVars, pContext, bindRootSig, pRootSignature);
    }

    bool ProgramVars::updateSpecializationImpl() const
    {
        ParameterBlock::SpecializationArgs specializationArgs;
        collectSpecializationArgs(specializationArgs);
        if( specializationArgs.size() == 0 )
        {
            mpSpecializedReflector = ParameterBlock::mpReflector;
            return false;
        }

        // TODO: Want a caching step here, if possible...

        auto pProgramKernels = mpProgramVersion->getKernels(this);
        mpSpecializedReflector = pProgramKernels->getReflector()->getDefaultParameterBlock();
        return false;
    }

    bool ComputeVars::apply(ComputeContext* pContext, bool bindRootSig, const ProgramKernels* pProgramKernels)
    {
        return applyProgramVarsCommon<false>(this, pContext, bindRootSig, pProgramKernels);
    }

    bool GraphicsVars::apply(RenderContext* pContext, bool bindRootSig, const ProgramKernels* pProgramKernels)
    {
        return applyProgramVarsCommon<true>(this, pContext, bindRootSig, pProgramKernels);
    }

    void ComputeVars::dispatchCompute(ComputeContext* pContext, uint3 const& threadGroupCount)
    {
        auto pProgram = std::dynamic_pointer_cast<ComputeProgram>(getReflection()->getProgramVersion()->getProgram());
        assert(pProgram);
        pProgram->dispatchCompute(pContext, this, threadGroupCount);
    }

    void RtProgramVars::init(const RtBindingTable::SharedPtr& pBindingTable)
    {
        mRayTypeCount = pBindingTable->getRayTypeCount();
        mGeometryCount = pBindingTable->getGeometryCount();

        // We must create sub-shader-objects for all the entry point
        // groups that are used by the supplied binding table.
        //
        assert(mpProgramVersion);
        assert(dynamic_cast<RtProgram*>(mpProgramVersion->getProgram().get()));
        auto pProgram = static_cast<RtProgram*>(mpProgramVersion->getProgram().get());
        auto pReflector = mpProgramVersion->getReflector();

        auto& rtDesc = pProgram->getRtDesc();
        std::set<int32_t> entryPointGroupIndices;

        // Ray generation and miss programs are easy: we just allocate space
        // for one parameter block per entry-point of the given type in the binding table.
        //
        const auto& info = pBindingTable->getRayGen();
        assert(info.isValid());
        mRayGenVars.resize(1);
        mRayGenVars[0].pVars = EntryPointGroupVars::create(pReflector->getEntryPointGroup(info.groupIndex), info.groupIndex);
        entryPointGroupIndices.insert(info.groupIndex);

        uint32_t missCount = pBindingTable->getMissCount();
        mMissVars.resize(missCount);

        for (uint32_t i = 0; i < missCount; ++i)
        {
            const auto& info = pBindingTable->getMiss(i);
            if (!info.isValid())
            {
                logWarning("Raytracing binding table has no shader at miss index " + std::to_string(i) + ". Is that intentional?");
                continue;
            }

            mMissVars[i].pVars = EntryPointGroupVars::create(pReflector->getEntryPointGroup(info.groupIndex), info.groupIndex);
            entryPointGroupIndices.insert(info.groupIndex);
        }

        // Hit groups are more complicated than ray generation and miss shaders.
        // We typically want a distinct parameter block per declared hit group
        // and per geometry in the scene.
        //
        // We need to take this extra complexity into account when allocating
        // space for the hit group parameter blocks.
        //
        uint32_t hitCount = mRayTypeCount * mGeometryCount;
        mHitVars.resize(hitCount);

        for (uint32_t rayType = 0; rayType < mRayTypeCount; rayType++)
        {
            for (uint32_t geometryID = 0; geometryID < mGeometryCount; geometryID++)
            {
                const auto& info = pBindingTable->getHitGroup(rayType, geometryID);
                if (!info.isValid()) continue;

                mHitVars[mRayTypeCount * geometryID + rayType].pVars = EntryPointGroupVars::create(pReflector->getEntryPointGroup(info.groupIndex), info.groupIndex);
                entryPointGroupIndices.insert(info.groupIndex);
            }
        }

        mUniqueEntryPointGroupIndices.assign(entryPointGroupIndices.begin(), entryPointGroupIndices.end());
        assert(!mUniqueEntryPointGroupIndices.empty());

        // Build list of vars for all entry point groups.
        // Note that there may be nullptr entries, as not all hit groups need to be assigned.
        assert(mRayGenVars.size() == 1);
        mpEntryPointGroupVars.push_back(mRayGenVars[0].pVars);
        for (auto entryPointGroupInfo : mMissVars)
            mpEntryPointGroupVars.push_back(entryPointGroupInfo.pVars);
        for (auto entryPointGroupInfo : mHitVars)
            mpEntryPointGroupVars.push_back(entryPointGroupInfo.pVars);
    }

    static RtEntryPointGroupKernels* getUniqueRtEntryPointGroupKernels(const ProgramKernels::SharedConstPtr& pKernels, uint32_t uniqueEntryPointGroupIndex)
    {
        if (uniqueEntryPointGroupIndex < 0) return nullptr;
        auto pEntryPointGroup = pKernels->getUniqueEntryPointGroup(uniqueEntryPointGroupIndex);
        assert(dynamic_cast<RtEntryPointGroupKernels*>(pEntryPointGroup.get()));
        return static_cast<RtEntryPointGroupKernels*>(pEntryPointGroup.get());
    }

    bool RtProgramVars::applyVarsToTable(ShaderTable::SubTableType type, uint32_t tableOffset, VarsVector& varsVec, const RtStateObject* pRtso)
    {
        auto& pKernels = pRtso->getKernels();

        for (uint32_t i = 0; i < (uint32_t)varsVec.size(); i++)
        {
            auto& varsInfo = varsVec[i];
            auto pBlock = varsInfo.pVars.get();
            if (!pBlock) continue;

            auto uniqueGroupIndex = pBlock->getGroupIndexInProgram();

            auto pGroupKernels = getUniqueRtEntryPointGroupKernels(pKernels, uniqueGroupIndex);
            if (!pGroupKernels) continue;

            // If we get here the shader record is used and will be assigned a valid shader identifier.
            // Shader records that are skipped above are left in their default state: initialized to zeros,
            // indicating that no shader should be executed.

            uint8_t* pRecord = mpShaderTable->getRecordPtr(type, tableOffset + i);

            auto pShaderIdentifier = pRtso->getShaderIdentifier(uniqueGroupIndex);
            memcpy(pRecord, pShaderIdentifier, mpShaderTable->getShaderIdentifierSize());


            varsInfo.lastObservedChangeEpoch = getEpochOfLastChange(pBlock);
        }

        return true;
    }

    bool RtProgramVars::apply(RenderContext* pCtx, RtStateObject* pRtso)
    {
        auto& pKernels = pRtso->getKernels();

        bool needShaderTableUpdate = false;
        if (!mpShaderTable)
        {
            mpShaderTable = ShaderTable::create();
            needShaderTableUpdate = true;
        }

        if (!needShaderTableUpdate)
        {
            if (pRtso != mpShaderTable->getRtso())
            {
                needShaderTableUpdate = true;
            }
        }

        if (!needShaderTableUpdate)
        {
            // We need to check if anything has changed that would require the shader
            // table to be rebuilt.
            assert(mRayGenVars.size() == 1);
            auto& varsInfo = mRayGenVars[0];
            auto pBlock = varsInfo.pVars.get();

            auto changeEpoch = computeEpochOfLastChange(pBlock);

            if (changeEpoch != varsInfo.lastObservedChangeEpoch)
            {
                needShaderTableUpdate = true;
            }
        }

        if (needShaderTableUpdate)
        {
            mpShaderTable->update(pCtx, pRtso, this);

            // We will iterate over the sub-tables (ray-gen, hit, miss)
            // in a specific order that matches the way that we have
            // enumerated the entry-point-group "instances" for indexing
            // in other parts of the code.
            if (!applyVarsToTable(ShaderTable::SubTableType::RayGen, 0, mRayGenVars, pRtso)) return false;
            if (!applyVarsToTable(ShaderTable::SubTableType::Miss, 0, mMissVars, pRtso)) return false;
            if (!applyVarsToTable(ShaderTable::SubTableType::Hit, 0, mHitVars, pRtso)) return false;

            mpShaderTable->flushBuffer(pCtx);
        }

        if (!applyProgramVarsCommon<false>(this, pCtx, true, pRtso->getKernels().get()))
        {
            return false;
        }

        return true;
    }

}