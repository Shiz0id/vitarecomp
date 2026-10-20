# Vendored Vita3K

`third_party/Vita3K/` is a verbatim copy of [Vita3K](https://github.com/Vita3K/Vita3K)
at upstream commit `287accffe904489fc7853fb28c61c2ca14411217` (2026-10-19),
with its `.git` directory removed. It is **GPLv2** — see
`third_party/Vita3K/COPYING.txt`.

Vita3K's own third-party dependencies are git submodules upstream and are **not**
included. They are pinned at these commits (from `git submodule status`):

```
-82767fe38823c32536726ea798f392b0b49e66b9 external/LibAtrac9
-d8e3e2b141b8c8a167b2e3984736a6baacff316c external/SPIRV-Cross
-3843082eaa6a6f67e6780715fcd602fc81942c9c external/VulkanMemoryAllocator-Hpp
-ff5f55bd97a93b23d6e5cb6d192290fb49b3f002 external/boost
-022575848782a4801fd150fdbc927effcbca0864 external/capstone
-9afb99746f0f5fc94ac8aef737053ae0481ba8d1 external/concurrentqueue
-4274d032355abe24cf6da0b35fad9b8122ea1f61 external/cubeb
-31db6474b5231c180bd4618c8c90e43af50c86d0 external/dirent
-e98f4ee160380d7c39dc1f04e7488bcf0770d391 external/dlmalloc
-86458a0bd369d63ba4c2ef812cacbb6c9080c065 external/dynarmic
-7628e38946e9aeabe6a15228b230fe4ae72f3539 external/ffmpeg
-1be298e1bd68957e4cd352e1f676f00e07dcfb57 external/fmt
-fc9889c889561c5882e83819dcaffef5ed45529b external/glslang
-52eb8108c5bdec04579160ae17225d66034bd723 external/googletest
-8fae8ce254dfc1344527e05301e43f37dea2df80 external/libadrenotools
-d9a890b712dcdb46d3d33230997efc59f5ad8d62 external/libfat16
-fc168e8605bfa51aaec22ab0c4e46b9de665a437 external/nativefiledialog-extended
-f08e577d31a8270a14e64603ff6bfdca614ff4d3 external/printf
-e21df9a74852433f48d6593b8ef203dc7c424e05 external/psvpfstools
-c8033ce9d039e7f9d134877c363397b3cfe20816 external/pugixml
-fa2c02bb6e21974a89ea9824bc53c9932abe5f9c external/sdl
-d6c93f38a08b4ea0f8ffcfd7f7857307d5d5c3fa external/spdlog
-31c1ad37456438565541f4919958214b6e762fb4 external/stb
-319da6b563d8da689f3b9df2fbb839edd41a1943 external/substitute
-05cceee0df3b8d7c6fa87e9638af311dbabc63cb external/tracy
-43fc1e3c686a1fc035eca583fdfeaa5e6419a61a external/vita-toolchain
-e626a72bc2321cd320e953a0ccf1584cad60f363 external/xxHash
-56e3bb550c91fd7005566f19c079cb7a503223cf external/yaml-cpp
```

To build Vita3K itself, clone upstream at the commit above with
`--recursive`; this copy is for reading and for lifting code into a
GPL-licensed game repo.

## Licensing consequence

The vitarecomp toolkit sources (`tools/`, `src/`, `include/`, `tests/`)
remain MIT and contain no Vita3K code. Anything that **links or incorporates**
code from `third_party/Vita3K/` is a combined work under GPLv2.
