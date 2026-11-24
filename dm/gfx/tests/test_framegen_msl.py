"""Compile the exact embedded framegen shaders without launching the game."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "nvn/nvn_framegen.inc").read_text()
start = source.index("static NSString *const FG_MSL")
end = source.index("static int fg_pipes", start)
with tempfile.TemporaryDirectory(prefix="ascension-fg-msl-") as tmp:
    p = Path(tmp)
    program = '#import <Foundation/Foundation.h>\n#import <Metal/Metal.h>\n#include "nvn_framegen_color.h"\n'
    program += source[start:end]
    program += r"""
int main(void) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e=nil;
    MTLCompileOptions *opts=[MTLCompileOptions new]; opts.mathMode=MTLMathModeSafe;
    id<MTLLibrary> lib=[d newLibraryWithSource:FG_MSL options:opts error:&e];
    if(!lib) { fprintf(stderr,"%s\n",e.description.UTF8String); return 1; }
    MTLPixelFormat formats[]={MTLPixelFormatRG16Float,MTLPixelFormatRGBA8Unorm,MTLPixelFormatBGRA8Unorm,MTLPixelFormatRGB10A2Unorm};
    for(int i=0;i<4;i++) {
        MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
        pd.vertexFunction=[lib newFunctionWithName:@"fg_vs"];
        pd.fragmentFunction=[lib newFunctionWithName:i==0?@"fg_mv":@"fg_comp"];
        pd.colorAttachments[0].pixelFormat=formats[i];
        if(![d newRenderPipelineStateWithDescriptor:pd error:&e]) { fprintf(stderr,"%s\n",e.description.UTF8String); return 2; }
    }
    puts("Exact frame-generation MSL: 4 pipelines compiled"); return 0;
}}
"""
    (p / "probe.m").write_text(program)
    subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-fobjc-arc", "-I"+str(root/"nvn"), str(p/"probe.m"), "-framework", "Metal", "-framework", "Foundation", "-o", str(p/"probe")],check=True)
    subprocess.run([str(p/"probe")], check=True)
