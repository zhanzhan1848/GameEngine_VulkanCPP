#include "ToolsCommon.h"
#include "Geometry.h"
#include <iostream>
#include <fstream>
#include <string>

// Define the callback type matching ToolsCommon.h
// using progress_callback = void(*)(s32, s32);

extern "C" void ImportFbx(const char* file, primal::tools::scene_data* data, void(*callback)(s32, s32));

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: ContentToolsCLI <input_fbx> <output_bin>" << std::endl;
        return 1;
    }

    const char* input_file = argv[1];
    const char* output_file = argv[2];

    primal::tools::scene_data data{};
    
    // Set default settings
    data.settings.calculate_normals = true;
    data.settings.calculate_tangents = true;
    data.settings.reverse_handedness = false;
    data.settings.import_embedded_textures = true;
    data.settings.import_animations = false;

    std::cout << "Importing FBX: " << input_file << "..." << std::endl;

    ImportFbx(input_file, &data, [](s32 val, s32 max) {
        // Simple progress indicator
        // std::cout << "\rProgress: " << val << "/" << max << std::flush;
    });
    
    std::cout << "Import finished." << std::endl;

    if (data.buffer && data.buffer_size > 0) {
        std::ofstream out(output_file, std::ios::binary);
        if (out) {
            out.write(reinterpret_cast<char*>(data.buffer), data.buffer_size);
            std::cout << "Successfully wrote to " << output_file << " (" << data.buffer_size << " bytes)" << std::endl;
            
            // Free memory allocated in ContentTools
            // Since we are linking against ContentTools, we should probably use a Free function if exposed.
            // But ContentTools uses malloc on Mac (Geometry.cpp line 710), so free() is correct.
            free(data.buffer);
        } else {
            std::cerr << "Failed to open output file: " << output_file << std::endl;
            return 1;
        }
    } else {
        std::cerr << "Failed to import FBX or empty result." << std::endl;
        return 1;
    }

    return 0;
}
