// Real Dawn RHI roundtrip: texel strides, tight rows, array layer and completion.
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnCommandBuffer.h"
#include "Engine/Common/CoreDiag.h"
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
using namespace primal::graphics::rhi;
static void Require(bool ok, const char* message) {
    if (!ok) { primal::diag::Emit(primal::diag::kLevelError, "DawnReadback", message); std::exit(1); }
}
int main() {
    DeviceDesc dd{}; dd.platform = RHIPlatform::Dawn; dd.enableValidation = true;
    DawnDevice device(dd);
    Require(device.Initialize(), "device initialization failed");
    u32 cases = 0;
    for (u32 rowTexels : {0u, 64u}) {
        for (u32 layer : {0u, 1u}) {
            TextureDesc td{}; td.size = {3, 5, 1}; td.arraySize = 2;
            td.type = TextureType::Texture2DArray; td.format = DataFormat::RGBA8_UNorm;
            td.usage = TextureUsage::CopySource | TextureUsage::CopyDest;
            const auto texture = device.CreateTexture(td);
            Require(texture != handles::INVALID_RESOURCE, "texture creation failed");
            // Seed through queue texture upload, independently of the encoder
            // copy path under test. A roundtrip alone could hide a layer bug.
            std::vector<unsigned char> neutral(3 * 5 * 4, 0xA5);
            for (u32 z = 0; z < 2; ++z)
                device.UpdateTextureData(texture, neutral.data(), 0, 0, z, 3, 5, 1, 12, 0);
            const u32 rowBytes = (rowTexels ? rowTexels : 3u) * 4;
            const u32 imageHeight = 7;
            const u64 bytes = u64(rowBytes) * imageHeight;
            std::vector<unsigned char> input(bytes, 0xA5);
            for (u32 y = 0; y < 5; ++y)
                for (u32 x = 0; x < 12; ++x) input[y * rowBytes + x] = static_cast<unsigned char>(y * 31 + x * 7 + layer);
            BufferDesc bd{}; bd.size = bytes; bd.memoryUsage = GPUMemoryUsage::Staging;
            auto upload = device.CreateBuffer(bd);
            bd.memoryUsage = GPUMemoryUsage::Readback;
            auto readback = device.CreateBuffer(bd);
            auto neighbor = device.CreateBuffer(bd);
            Require(upload != handles::INVALID_RESOURCE && readback != handles::INVALID_RESOURCE && neighbor != handles::INVALID_RESOURCE, "buffer creation failed");
            auto* mapped = device.MapBuffer(upload, 0, bytes);
            Require(mapped, "upload mapping failed"); std::memcpy(mapped, input.data(), bytes); device.UnmapBuffer(upload);
            auto handle = device.CreateCommandBuffer(CommandQueueType::Graphics);
            auto* cmd = device.GetCommandBuffer(handle);
            Require(cmd && cmd->Begin(), "command begin failed");
            BufferTextureCopyRegion region{};
            region.bufferRowLength = rowTexels; region.bufferImageHeight = imageHeight;
            region.imageSubresource.baseArrayLayer = layer;
            region.imageExtent = {3, 5, 1};
            cmd->CopyBufferToTexture(upload, texture, &region, 1);
            cmd->CopyTextureToBuffer(texture, readback, &region, 1);
            region.imageSubresource.baseArrayLayer = 1u - layer;
            cmd->CopyTextureToBuffer(texture, neighbor, &region, 1);
            Require(cmd->End(), "command end failed");
            QueueSubmitInfo submit{}; submit.cmdBuffer = handle;
            Require(device.Submit(submit), "device submission failed");
            Require(cmd->WaitForCompletion(), "queue completion failed");
            Require(cmd->GetState() == CommandBufferState::Executed, "completion did not advance state");
            const auto* data = static_cast<const unsigned char*>(device.MapBuffer(readback, 0, bytes));
            Require(data, "readback mapping failed");
            for (u32 y = 0; y < 5; ++y)
                Require(std::memcmp(data + y * rowBytes, input.data() + y * rowBytes, 12) == 0, "pixel row mismatch");
            device.UnmapBuffer(readback);
            data = static_cast<const unsigned char*>(device.MapBuffer(neighbor, 0, bytes));
            Require(data, "neighbor mapping failed");
            for (u32 y = 0; y < 5; ++y)
                for (u32 x = 0; x < 12; ++x) Require(data[y * rowBytes + x] == 0xA5, "copy changed or read the wrong array layer");
            device.UnmapBuffer(neighbor);
            device.DestroyCommandBuffer(handle); device.DestroyBuffer(upload); device.DestroyBuffer(readback); device.DestroyBuffer(neighbor); device.DestroyTexture(texture);
            ++cases;
        }
    }
    device.Shutdown();
    std::printf("{\"passed\":true,\"native_dawn_rhi_copy_cases\":%u}\n", cases);
}
