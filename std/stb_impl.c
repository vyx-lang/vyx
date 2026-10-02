// STB Implementation — compile this file to get stb_image.lib
// Usage:
//   clang -c std/stb_impl.c -o stb_impl.obj -O2
//   llvm-ar rcs stb_image.lib stb_impl.obj
//
// Or use: vyxc build (with sources = "std/stb_impl.c" in Vyx.toml)

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_NO_SIMD

// Download stb_image.h and stb_image_write.h from:
// https://github.com/nothings/stb/blob/master/stb_image.h
// https://github.com/nothings/stb/blob/master/stb_image_write.h
// Place them next to this file or in your include path.

#ifdef __has_include
  #if __has_include("stb_image.h")
    #include "stb_image.h"
  #elif __has_include("../vendor/stb_image.h")
    #include "../vendor/stb_image.h"
  #else
    #error "stb_image.h not found. Download from https://github.com/nothings/stb"
  #endif

  #if __has_include("stb_image_write.h")
    #include "stb_image_write.h"
  #elif __has_include("../vendor/stb_image_write.h")
    #include "../vendor/stb_image_write.h"
  #endif
#else
  #include "stb_image.h"
  #include "stb_image_write.h"
#endif
