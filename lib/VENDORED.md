# Vendored third-party libraries

OFS-SE vendors its dependencies directly instead of using git submodules,
so the tree builds offline with no network fetches. The commits below are
the upstream revisions each directory was taken from.

| library | upstream | commit |
|---|---|---|
| SDL2 | https://github.com/libsdl-org/SDL | `5d249570393f` |
| bitsery | https://github.com/fraillt/bitsery | `bcd03b4d685f` |
| civetweb | https://github.com/civetweb/civetweb | `eefb26f82b23` |
| eventpp | https://github.com/wqking/eventpp | `63497c60ef16` |
| glad2 | (vendored in the OFS source tree) | - |
| glm | https://github.com/g-truc/glm | `bf71a8349481` |
| imgui | https://github.com/ocornut/imgui | `c191faf0ba47` |
| json | https://github.com/nlohmann/json | `bc889afb4c5b` |
| libmpv | (vendored in the OFS source tree) | - |
| lua | https://github.com/lua/lua | `5d708c3f9cae` |
| refl-cpp | https://github.com/veselink1/refl-cpp | `27fbd7d2e6d8` |
| sol2 | https://github.com/ThePhD/sol2 | `eba86625b707` |
| stb | (vendored in the OFS source tree) | - |
| tinyfiledialogs | (vendored in the OFS source tree) | - |
| tracy | https://github.com/wolfpld/tracy | `5a1f5371b792` |
