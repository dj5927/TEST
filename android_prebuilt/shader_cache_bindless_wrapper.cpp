// Android ships two Vulkan guest-shader caches:
//   generated/shader_cache.cpp -> compact mobile-core (9/1/3/9)
//   this TU                   -> original bindless cache
// Rename every generated global before including the original prebuilt cache
// so both variants can coexist in one libmain.so and be selected at runtime.
#define g_shaderCacheEntries g_bindlessShaderCacheEntries
#define g_shaderCacheEntryCount g_bindlessShaderCacheEntryCount
#define g_compressedDxilCache g_bindlessCompressedDxilCache
#define g_dxilCacheCompressedSize g_bindlessDxilCacheCompressedSize
#define g_dxilCacheDecompressedSize g_bindlessDxilCacheDecompressedSize
#define g_compressedSpirvCache g_bindlessCompressedSpirvCache
#define g_spirvCacheCompressedSize g_bindlessSpirvCacheCompressedSize
#define g_spirvCacheDecompressedSize g_bindlessSpirvCacheDecompressedSize
#include "shader_cache.cpp"
