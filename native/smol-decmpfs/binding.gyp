{
  "targets": [
    {
      "target_name": "smol_decmpfs",
      "sources": [
        "src/sha512.cpp",
        "src/smol_decmpfs.cpp",
        "src/napi_addon.cpp"
      ],
      "include_dirs": [
        "include",
        "src",
        "<!(node -p \"process.env.SMOL_ZSTD_PREFIX || '/opt/homebrew/opt/zstd'\")/include"
      ],
      "libraries": [
        "<!(node -p \"process.env.SMOL_ZSTD_PREFIX || '/opt/homebrew/opt/zstd'\")/lib/libzstd.a"
      ],
      "defines": ["NAPI_VERSION=8"],
      "cflags_cc": ["-std=c++17", "-O2", "-fno-rtti"],
      "cflags_cc!": ["-fno-exceptions"],
      "xcode_settings": {
        "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
        "CLANG_CXX_LIBRARY": "libc++",
        "MACOSX_DEPLOYMENT_TARGET": "10.15",
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES"
      },
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1,
          "AdditionalOptions": ["/std:c++17"]
        }
      }
    }
  ]
}
