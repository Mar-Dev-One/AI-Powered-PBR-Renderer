#include "RHI_GL_Internal.h"

#include <string.h>

// Compiles one stage. `defines` (may be NULL) is spliced in right after the
// source's #version line -- GLSL requires #version to come first -- by
// handing glShaderSource the source in pieces rather than building a joined
// copy. A `#line 2` after the defines keeps compile-error line numbers
// matching the file on disk.
static GLuint
compile_stage(GLenum stage, const char* src, const char* defines)
{
    const char* pieces[4];
    GLint       lengths[4];
    GLsizei     piece_count = 0;

    if (defines && strncmp(src, "#version", 8) == 0)
    {
        const char* body = strchr(src, '\n');
        body = body ? body + 1 : src + strlen(src);

        pieces[0] = src;        lengths[0] = (GLint)(body - src);   // "#version ...\n"
        pieces[1] = defines;    lengths[1] = -1;                    // -1 = NUL-terminated
        pieces[2] = "\n#line 2\n"; lengths[2] = -1;
        pieces[3] = body;       lengths[3] = -1;
        piece_count = 4;
    }
    else
    {
        pieces[0] = src;
        lengths[0] = -1;
        piece_count = 1;
    }

    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, piece_count, pieces, lengths);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if (!success)
    {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOG_ERROR("Shader compile error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

rhi_shader gl_shader_create(rhi_shader_desc desc)
{
    GLuint vs = compile_stage(GL_VERTEX_SHADER, desc.vertex_src, desc.defines);
    GLuint fs = compile_stage(GL_FRAGMENT_SHADER, desc.fragment_src, desc.defines);

    if (!vs || !fs)
    {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return NULL;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!success)
    {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        LOG_ERROR("Shader link error: %s\n", log);
        glDeleteProgram(program);
        return NULL;
    }

    rhi_shader shader = calloc(1, sizeof(*shader));
    if (!shader)
        FATAL("Out of memory creating shader");

    shader->program = program;
    return shader;
}

void gl_shader_bind(rhi_shader shader)
{
    glUseProgram(shader->program);
}

void gl_shader_destroy(rhi_shader shader)
{
    if (!shader)
        return;

    for (uint32 i = 0; i < shader->uniform_capacity; ++i)
        free(shader->uniforms[i].name);
    free(shader->uniforms);

    glDeleteProgram(shader->program);
    free(shader);
}

// --- Uniform location cache ----------------------------------------------
// glGetUniformLocation() is a string lookup inside the driver, and the
// renderer sets dozens of uniforms by name per draw (every light field,
// every cascade) -- so each shader remembers what it already asked for.
// Linear probing over a power-of-two table, grown at 3/4 load.

#define UNIFORM_CACHE_INITIAL_CAPACITY 64

static uint32
hash_name(const char* name)
{
    uint32 hash = 2166136261u;   // FNV-1a
    for (const unsigned char* c = (const unsigned char*)name; *c; ++c)
        hash = (hash ^ *c) * 16777619u;
    return hash;
}

static gl_uniform_slot*
find_slot(gl_uniform_slot* slots, uint32 capacity, const char* name, uint32 hash)
{
    uint32 mask = capacity - 1;
    for (uint32 i = hash & mask;; i = (i + 1) & mask)
    {
        gl_uniform_slot* slot = &slots[i];
        if (!slot->name || (slot->hash == hash && strcmp(slot->name, name) == 0))
            return slot;
    }
}

static void
grow_cache(rhi_shader shader)
{
    uint32 new_capacity = shader->uniform_capacity ? shader->uniform_capacity * 2 : UNIFORM_CACHE_INITIAL_CAPACITY;

    gl_uniform_slot* new_slots = calloc(new_capacity, sizeof(*new_slots));
    if (!new_slots)
        FATAL("Out of memory growing uniform cache");

    for (uint32 i = 0; i < shader->uniform_capacity; ++i)
    {
        gl_uniform_slot* old = &shader->uniforms[i];
        if (old->name)
            *find_slot(new_slots, new_capacity, old->name, old->hash) = *old;
    }

    free(shader->uniforms);
    shader->uniforms         = new_slots;
    shader->uniform_capacity = new_capacity;
}

static GLint
uniform_location(rhi_shader shader, const char* name)
{
    if ((shader->uniform_count + 1) * 4 > shader->uniform_capacity * 3)
        grow_cache(shader);

    uint32           hash = hash_name(name);
    gl_uniform_slot* slot = find_slot(shader->uniforms, shader->uniform_capacity, name, hash);

    if (!slot->name)
    {
        size_t length = strlen(name) + 1;
        slot->name = malloc(length);
        if (!slot->name)
            FATAL("Out of memory caching uniform name");
        memcpy(slot->name, name, length);

        slot->hash     = hash;
        slot->location = glGetUniformLocation(shader->program, name);
        shader->uniform_count++;
    }

    return slot->location;
}

void gl_shader_set_mat4(rhi_shader shader, const char* name, const f32* matrix)
{
    GLint location = uniform_location(shader, name);
    if (location < 0)
        return; // unused-uniform-optimized-out or a typo'd name; not fatal

    // glProgramUniform (DSA, core since GL 4.1) writes to the given
    // program's uniform directly — unlike glUniform*, it does not require
    // that program to be the currently bound one via glUseProgram.
    glProgramUniformMatrix4fv(shader->program, location, 1, GL_FALSE, matrix);
}

void gl_shader_set_vec3(rhi_shader shader, const char* name, f32 x, f32 y, f32 z)
{
    GLint location = uniform_location(shader, name);
    if (location < 0)
        return;

    glProgramUniform3f(shader->program, location, x, y, z);
}

void gl_shader_set_int(rhi_shader shader, const char* name, int32 value)
{
    GLint location = uniform_location(shader, name);
    if (location < 0)
        return;

    glProgramUniform1i(shader->program, location, value);
}

void gl_shader_set_vec4(rhi_shader shader, const char* name, f32 x, f32 y, f32 z, f32 w)
{
    GLint location = uniform_location(shader, name);
    if (location < 0)
        return;

    glProgramUniform4f(shader->program, location, x, y, z, w);
}

void gl_shader_set_float(rhi_shader shader, const char* name, f32 value)
{
    GLint location = uniform_location(shader, name);
    if (location < 0)
        return;

    glProgramUniform1f(shader->program, location, value);
}
