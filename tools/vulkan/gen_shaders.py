#!/usr/bin/env python3
"""Generates src/kfx/renderer/vulkan/VkShaders_gen.cpp from the OpenGL renderer's shaders (GLShaders.h).

The OpenGL shaders stay the only source: each program is compiled for Vulkan with glslangValidator's relaxed
rules (-R: loose uniforms become one uniform block, std140), auto-mapped bindings and locations. Two edits are
made to every vertex shader: main() is renamed and a new main() remaps depth from OpenGL's [-1,1] to Vulkan's
[0,1]. The generated file holds the SPIR-V and, read from the disassembly, the uniform block's member offsets,
the sampler bindings and the vertex input locations, so the Vulkan passes set uniforms by name as OpenGL does.

Run it (WSL) after changing GLShaders.h:  python3 tools/vulkan/gen_shaders.py
Needs: g++, glslangValidator, spirv-dis, spirv-opt (Ubuntu: glslang-tools, spirv-tools).
"""
import os, re, struct, subprocess, sys, tempfile, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
GLSH = ROOT / "src/kfx/renderer/opengl"
OUT = ROOT / "src/kfx/renderer/vulkan/VkShaders_gen.cpp"
UBO_SET, UBO_BINDING = 0, 15   # the default uniform block's binding; samplers are auto-mapped from 0

PROGRAMS = [
    # name               vertex shader                     fragment shader
    ("ui_sprite",         "UI_VERTEX_SHADER",               "UI_SPRITE_FRAGMENT_SHADER"),
    ("ui_colored",        "UI_VERTEX_SHADER",               "UI_SPRITE_COLORED_FRAGMENT_SHADER"),
    ("ui_remap",          "UI_VERTEX_SHADER",               "UI_REMAP_FRAGMENT_SHADER"),
    ("ui_clut",           "UI_VERTEX_SHADER",               "UI_CLUT_FRAGMENT_SHADER"),
    ("ui_solid",          "UI_VERTEX_SHADER",               "UI_SOLID_FRAGMENT_SHADER"),
    ("world",             "WORLD_VERTEX_SHADER",            "WORLD_FRAGMENT_SHADER"),
    ("shadow",            "SHADOW_VERTEX_SHADER",           "SHADOW_FRAGMENT_SHADER"),
    ("lens_mist",         "LENS_COMPOSITE_VERTEX_SHADER",   "LENS_MIST_FRAGMENT_SHADER"),
    ("lens_remap",        "LENS_COMPOSITE_VERTEX_SHADER",   "LENS_REMAP_FRAGMENT_SHADER"),
    ("lens_overlay",      "LENS_COMPOSITE_VERTEX_SHADER",   "LENS_OVERLAY_FRAGMENT_SHADER"),
    ("lens_copy",         "LENS_COMPOSITE_VERTEX_SHADER",   "LENS_COPY_FRAGMENT_SHADER"),
    ("mapfade",           "LENS_COMPOSITE_VERTEX_SHADER",   "MAPFADE_FRAGMENT_SHADER"),
    ("kspr",              "KSPR_VERTEX_SHADER",             "KSPR_FRAGMENT_SHADER"),
    ("kspr_glow",         "KSPR_VERTEX_SHADER",             "KSPR_GLOW_FRAGMENT_SHADER"),
    ("kspr_array",        "KSPR_VERTEX_SHADER",             "KSPR_ARRAY_FRAGMENT_SHADER"),
    ("kspr_array_glow",   "KSPR_VERTEX_SHADER",             "KSPR_ARRAY_GLOW_FRAGMENT_SHADER"),
    ("kspr_outline",      "KSPR_VERTEX_SHADER",             "KSPR_OUTLINE_FRAGMENT_SHADER"),
    ("kspr_array_outline","KSPR_VERTEX_SHADER",             "KSPR_ARRAY_OUTLINE_FRAGMENT_SHADER"),
    ("kspr_edge",         "KSPR_VERTEX_SHADER",             "KSPR_EDGE_FRAGMENT_SHADER"),
    ("kspr_array_edge",   "KSPR_VERTEX_SHADER",             "KSPR_ARRAY_EDGE_FRAGMENT_SHADER"),
    ("kspr_inst",         "KSPR_INST_VERTEX_SHADER",        "KSPR_INST_FRAGMENT_SHADER"),
    ("kspr_inst_outline", "KSPR_INST_OUTLINE_VERTEX_SHADER","KSPR_INST_OUTLINE_FRAGMENT_SHADER"),
    ("kspr_inst_edge",    "KSPR_INST_OUTLINE_VERTEX_SHADER","KSPR_INST_EDGE_FRAGMENT_SHADER"),
    ("rawimage_blit",     "RAWIMAGE_VERTEX_SHADER",         "RAWIMAGE_BLIT_FRAGMENT_SHADER"),
    ("rawimage_transparent", "RAWIMAGE_VERTEX_SHADER",      "RAWIMAGE_TRANSPARENT_FRAGMENT_SHADER"),
    ("rawimage_coverage", "RAWIMAGE_VERTEX_SHADER",         "RAWIMAGE_TRANSPARENT_COVERAGE_FRAGMENT_SHADER"),
    ("rawimage_zoom",     "RAWIMAGE_VERTEX_SHADER",         "RAWIMAGE_ZOOM_FRAGMENT_SHADER"),
    ("zoombox_tiles",     "ZOOMBOX_TILES_VERTEX_SHADER",    "ZOOMBOX_TILES_FRAGMENT_SHADER"),
]

# Vulkan-only programs, not in GLShaders.h: the Android touch overlay picture laid over the frame.
INLINE = {
    "OVERLAY_VERTEX_SHADER": """#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
uniform vec2 u_screen_size;
out vec2 v_uv;
void main()
{
    gl_Position = vec4((a_pos.x / u_screen_size.x) * 2.0 - 1.0, 1.0 - (a_pos.y / u_screen_size.y) * 2.0, 0.0, 1.0);
    v_uv = a_uv;
}
""",
    "OVERLAY_FRAGMENT_SHADER": """#version 330 core
in vec2 v_uv;
uniform sampler2D u_image;
out vec4 fragColor;
void main()
{
    fragColor = texture(u_image, v_uv); // premultiplied (drawn over transparent black)
}
""",
}
PROGRAMS.append(("overlay_rgba", "OVERLAY_VERTEX_SHADER", "OVERLAY_FRAGMENT_SHADER"))

def run(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        print(" ".join(cmd)); print(r.stdout[-3000:]); print(r.stderr[-3000:]); sys.exit(1)
    return r.stdout

def dump_sources(tmp):
    for n, text in INLINE.items():
        (tmp / f"{n}.glsl").write_text(text)
    names = sorted(({p[1] for p in PROGRAMS} | {p[2] for p in PROGRAMS}) - set(INLINE))
    src = ['#include <cstdio>', '#include "GLShaders.h"', 'int main(){']
    for n in names:
        src.append(f'{{FILE*f=fopen("{n}.glsl","w"); fputs({n},f); fclose(f);}}')
    src.append('return 0;}')
    (tmp / "dump.cpp").write_text("\n".join(src))
    run(["g++", "-std=c++17", f"-I{GLSH}", "dump.cpp", "-o", "dump"], tmp)
    run(["./dump"], tmp)

def vertex_edit(text):
    text, n = re.subn(r"\bvoid\s+main\s*\(\s*\)", "void kfx_gl_main()", text)
    if n != 1:
        raise SystemExit("vertex shader without exactly one main()")
    return text + "\n// Vulkan: OpenGL's depth range [-1,1] -> [0,1]\nvoid main() { kfx_gl_main(); gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5; }\n"

SAMPLER_KIND = {}   # name -> 0 float 2D, 2 uint 2D, 3 int 2D; +4 for an array (per program, filled by assign_varyings)

VARYING = re.compile(r"^(\s*)((?:flat|smooth|noperspective)\s+)?(in|out)\s+(\w+)\s+(\w+)\s*(\[\s*(\d+)\s*\])?\s*;", re.M)

def location_count(gtype, arr):
    n = 1
    m = re.match(r"d?mat(\d)(?:x(\d))?$", gtype)
    if m:
        n = int(m.group(1))
    return n * (int(arr) if arr else 1)

def assign_varyings(vs, fs):
    """Explicit locations for the varyings, the same in both stages (auto-mapping numbers each stage alone)."""
    locs, nxt = {}, 0
    def vs_sub(m):
        nonlocal nxt
        if m.group(3) != "out":
            return m.group(0)
        name = m.group(5)
        locs[name] = nxt
        nxt += location_count(m.group(4), m.group(7))
        return f"{m.group(1)}layout(location = {locs[name]}) {m.group(2) or ''}{m.group(3)} {m.group(4)} {m.group(5)}{m.group(6) or ''};"
    vs = VARYING.sub(vs_sub, vs)
    def fs_sub(m):
        if m.group(3) != "in":
            return m.group(0)
        name = m.group(5)
        if name not in locs:
            raise SystemExit(f"fragment input {name} has no vertex output")
        return f"{m.group(1)}layout(location = {locs[name]}) {m.group(2) or ''}{m.group(3)} {m.group(4)} {m.group(5)}{m.group(6) or ''};"
    fs = VARYING.sub(fs_sub, fs)
    # samplers: one numbering for the program (auto-mapping can give two samplers the same binding)
    binds = {}
    def smp_sub(m):
        name = m.group(3)
        if name not in binds:
            binds[name] = len(binds)
            t = m.group(2)
            SAMPLER_KIND[name] = (2 if t.startswith("usampler") else 3 if t.startswith("isampler") else 0) + (1 if "Array" in t else 0) * 4
        return f"{m.group(1)}layout(binding = {binds[name]}) uniform {m.group(2)} {name};"
    smp = re.compile(r"^(\s*)uniform\s+((?:[iu])?sampler\w+)\s+(\w+)\s*;", re.M)
    vs = smp.sub(smp_sub, vs)
    fs = smp.sub(smp_sub, fs)
    # GLSL 3.30 takes varying locations and bindings only with these extensions
    ext = r"\1\n#extension GL_ARB_separate_shader_objects : enable\n#extension GL_ARB_shading_language_420pack : enable"
    vs = re.sub(r"^(#version[^\n]*)", ext, vs, count=1, flags=re.M)
    fs = re.sub(r"^(#version[^\n]*)", ext, fs, count=1, flags=re.M)
    return vs, fs

def parse_dis(dis):
    names, member_names, decos, member_offs, types, ptr_types, variables = {}, {}, {}, {}, {}, {}, {}
    for line in dis.splitlines():
        line = line.strip()
        m = re.match(r'OpName (%\S+) "(.*)"', line)
        if m: names[m.group(1)] = m.group(2); continue
        m = re.match(r'OpMemberName (%\S+) (\d+) "(.*)"', line)
        if m: member_names[(m.group(1), int(m.group(2)))] = m.group(3); continue
        m = re.match(r'OpDecorate (%\S+) (Location|Binding|DescriptorSet) (\d+)', line)
        if m: decos.setdefault(m.group(1), {})[m.group(2)] = int(m.group(3)); continue
        m = re.match(r'OpMemberDecorate (%\S+) (\d+) Offset (\d+)', line)
        if m: member_offs[(m.group(1), int(m.group(2)))] = int(m.group(3)); continue
        m = re.match(r'(%\S+) = OpTypePointer (\w+) (%\S+)', line)
        if m: ptr_types[m.group(1)] = (m.group(2), m.group(3)); continue
        m = re.match(r'(%\S+) = OpVariable (%\S+) (\w+)', line)
        if m: variables[m.group(1)] = (m.group(2), m.group(3)); continue
        m = re.match(r'(%\S+) = (OpType\w+)(.*)', line)
        if m: types[m.group(1)] = (m.group(2), m.group(3).split())
    res = {"inputs": {}, "outputs": {}, "samplers": {}, "ubo": None}
    for var, (ptype, storage) in variables.items():
        name = names.get(var, var)
        d = decos.get(var, {})
        if storage == "Input" and "Location" in d:
            res["inputs"][name] = d["Location"]
        elif storage == "Output" and "Location" in d:
            res["outputs"][name] = d["Location"]
        elif storage == "UniformConstant":
            res["samplers"][name] = (d.get("DescriptorSet", 0), d.get("Binding", -1))
        elif storage == "Uniform":
            pointee = ptr_types[ptype][1]
            members = []
            i = 0
            while (pointee, i) in member_names:
                members.append((member_names[(pointee, i)], member_offs.get((pointee, i), 0), member_size(types, pointee, i)))
                i += 1
            res["ubo"] = {"name": names.get(pointee, pointee), "set": d.get("DescriptorSet", 0),
                          "binding": d.get("Binding", -1), "members": members}
    return res

def member_size(types, struct_id, index):
    t = types.get(struct_id)
    if not t or t[0] != "OpTypeStruct":
        return 4
    mt = t[1][index]
    return type_size(types, mt)

def type_size(types, tid):
    t = types[tid]
    k = t[0]
    if k in ("OpTypeFloat", "OpTypeInt", "OpTypeBool"):
        return 4
    if k == "OpTypeVector":
        return type_size(types, t[1][0]) * int(t[1][1])
    if k == "OpTypeMatrix":
        # std140: each column takes a vec4
        return 16 * int(t[1][1])
    return 16

def spirv_words(path):
    data = path.read_bytes()
    return struct.unpack("<%dI" % (len(data) // 4), data)

def main():
    with tempfile.TemporaryDirectory(prefix="ekvk-shaders-") as tdir:
        tmp = pathlib.Path(tdir)
        dump_sources(tmp)
        out = ['// GENERATED by tools/vulkan/gen_shaders.py from src/kfx/renderer/opengl/GLShaders.h - do not edit.',
               '#include "pre_inc.h"', '#include "kfx/renderer/vulkan/VkShaders.h"', '#include "post_inc.h"', '']
        table = []
        for name, vs_name, fs_name in PROGRAMS:
            pdir = tmp / name
            pdir.mkdir()
            SAMPLER_KIND.clear()
            vs = vertex_edit((tmp / f"{vs_name}.glsl").read_text())
            fs = (tmp / f"{fs_name}.glsl").read_text()
            vs, fs = assign_varyings(vs, fs)
            (pdir / "p.vert").write_text(vs)
            (pdir / "p.frag").write_text(fs)
            run(["glslangValidator", "-R", "--target-env", "vulkan1.1", "--amb", "--aml",
                 "--sdub", "KfxUniforms", str(UBO_SET), str(UBO_BINDING), "-l", "p.vert", "p.frag"], pdir)
            info = {}
            for stage in ("vert", "frag"):
                dis = run(["spirv-dis", f"{stage}.spv"], pdir)
                info[stage] = parse_dis(dis)
                run(["spirv-opt", "--strip-debug", f"{stage}.spv", "-o", f"{stage}.min.spv"], pdir)
            # the stages must agree on the varyings' locations and on the uniform block
            for vname, loc in info["frag"]["inputs"].items():
                if info["vert"]["outputs"].get(vname) != loc:
                    raise SystemExit(f"{name}: varying {vname} at {loc} in the fragment shader, "
                                     f"{info['vert']['outputs'].get(vname)} in the vertex shader")
            members = {}
            ubo_binding = -1
            for stage in ("vert", "frag"):
                u = info[stage]["ubo"]
                if u is None: continue
                if u["binding"] != UBO_BINDING or u["set"] != UBO_SET:
                    raise SystemExit(f"{name}: uniform block at set {u['set']} binding {u['binding']}")
                ubo_binding = u["binding"]
                for (mname, off, size) in u["members"]:
                    if mname in members and members[mname][0] != off:
                        raise SystemExit(f"{name}: uniform {mname} at offsets {members[mname][0]} and {off}")
                    members[mname] = (off, size)
            samplers = {}
            for stage in ("vert", "frag"):
                for sname, (sset, sbind) in info[stage]["samplers"].items():
                    if sset != 0 or sbind < 0 or sbind == UBO_BINDING:
                        raise SystemExit(f"{name}: sampler {sname} at set {sset} binding {sbind}")
                    if sname in samplers and samplers[sname] != sbind:
                        raise SystemExit(f"{name}: sampler {sname} bound at {samplers[sname]} and {sbind}")
                    samplers[sname] = sbind
            if len(set(samplers.values())) != len(samplers):
                raise SystemExit(f"{name}: two samplers share a binding: {samplers}")
            ubo_size = 0
            for (off, size) in members.values():
                ubo_size = max(ubo_size, off + size)
            ubo_size = (ubo_size + 15) & ~15
            for stage in ("vert", "frag"):
                words = spirv_words(pdir / f"{stage}.min.spv")
                out.append(f"static const uint32_t s_{name}_{stage}[] = {{")
                for i in range(0, len(words), 8):
                    out.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
                out.append("};")
            out.append(f"static const VkShaderUniform s_{name}_uniforms[] = {{")
            for mname, (off, size) in sorted(members.items(), key=lambda kv: kv[1][0]):
                out.append(f'    {{ "{mname}", {off}, {size} }},')
            out.append('    { nullptr, 0, 0 },\n};')
            out.append(f"static const VkShaderSampler s_{name}_samplers[] = {{")
            for sname, b in sorted(samplers.items(), key=lambda kv: kv[1]):
                out.append(f'    {{ "{sname}", {b}, {SAMPLER_KIND.get(sname, 0)} }},')
            out.append('    { nullptr, 0, 0 },\n};')
            out.append(f"static const VkShaderInput s_{name}_inputs[] = {{")
            for iname, loc in sorted(info["vert"]["inputs"].items(), key=lambda kv: kv[1]):
                out.append(f'    {{ "{iname}", {loc} }},')
            out.append('    { nullptr, 0 },\n};\n')
            table.append(f'    {{ "{name}", s_{name}_vert, sizeof(s_{name}_vert), s_{name}_frag, sizeof(s_{name}_frag), '
                         f'{ubo_size}, {ubo_binding}, s_{name}_uniforms, s_{name}_samplers, s_{name}_inputs }},')
            print(f"{name:22s} ubo {ubo_size:4d} B, {len(members)} uniforms, samplers {sorted(samplers.items(), key=lambda kv: kv[1])}")
        out.append("const VkShaderProgramInfo g_vk_shader_programs[] = {")
        out.extend(table)
        out.append("    { nullptr, nullptr, 0, nullptr, 0, 0, -1, nullptr, nullptr, nullptr },\n};")
        OUT.write_text("\n".join(out) + "\n")
        print(f"wrote {OUT} ({OUT.stat().st_size // 1024} KiB)")

if __name__ == "__main__":
    main()
