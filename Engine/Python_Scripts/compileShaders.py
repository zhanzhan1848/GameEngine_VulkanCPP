import argparse
import os
import subprocess
import sys

parser = argparse.ArgumentParser(description = 'Compile all GLSL shaders')
parser.add_argument('--glslang', type = str, help = 'path to glslangValidator executable')
parser.add_argument('--g', action = 'store_true', help = 'compile with debug symbols')
args = parser.parse_args()

glslc_path = os.getenv("VULKAN_SDK") + "\\bin\\glslc.exe"
dir_path = "C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders"
for root, dirs, files in os.walk(dir_path):
    for file in files:
        if file.endswith(".vert") or file.endswith(".frag") or file.endswith(".comp") or file.endswith(".geom") or file.endswith(".tesc") or file.endswith(".tese") or file.endswith(".rgen") or file.endswith(".rchit") or file.endswith(".rmiss"):
            input_file = os.path.join(root, file)
            output_file = input_file + ".spv"

            add_params = ""
            # if args.g:
            #     add_params = "-g"

            if file.endswith(".rgen") or file.endswith(".rchit") or file.endswith(".rmiss"):
               add_params = add_params + " --target-env vulkan1.2"

            try:
                res = subprocess.call("%s %s -o %s %s" % (glslc_path, input_file, output_file, add_params), shell=True)
                print("%s OJ8K" % (output_file))
            except Exception as e:
                print(e)
            # res = subprocess.call([glslang_path, '-V', input_file, '-o', output_file, add_params], shell=True)
            if res != 0:
                sys.exit()