#include "render_api.h"
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>

static GLuint shader_program;
static GLuint textures[3]; // Y, U, V
static GLint  uniforms[4]; // Sampler Y, U, V, Scale
static GLuint vbo, ui_vbo;
static int r_width = 1280;
static int r_height = 720;

// Simple Quad
static const float vertices[] = {
    // Pos        // Tex
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

static const char *vs_source = 
    "attribute vec2 position;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "uniform vec2 u_scale;\n"
    "void main() {\n"
    "    gl_Position = vec4(position * u_scale, 0.0, 1.0);\n"
    "    v_texcoord = texcoord;\n"
    "}\n";

static const char *fs_source = 
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D tex_y;\n"
    "uniform sampler2D tex_u;\n"
    "uniform sampler2D tex_v;\n"
    "void main() {\n"
    "    float y = texture2D(tex_y, v_texcoord).r;\n"
    "    float u = texture2D(tex_u, v_texcoord).r - 0.5;\n"
    "    float v = texture2D(tex_v, v_texcoord).r - 0.5;\n"
    "    float r = y + 1.402 * v;\n"
    "    float g = y - 0.344136 * u - 0.714136 * v;\n"
    "    float b = y + 1.772 * u;\n"
    "    gl_FragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

static GLuint CompileShader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    
    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "Shader Compile Error: %s\n", log);
        return 0;
    }
    return shader;
}

static MemoryArena *g_render_arena;

void Render_Init(MemoryArena *arena) {
    g_render_arena = arena;
    
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vs_source);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fs_source);
    
    shader_program = glCreateProgram();
    glAttachShader(shader_program, vs);
    glAttachShader(shader_program, fs);
    glBindAttribLocation(shader_program, 0, "position");
    glBindAttribLocation(shader_program, 1, "texcoord");
    glLinkProgram(shader_program);
    
    glUseProgram(shader_program);
    uniforms[0] = glGetUniformLocation(shader_program, "tex_y");
    uniforms[1] = glGetUniformLocation(shader_program, "tex_u");
    uniforms[2] = glGetUniformLocation(shader_program, "tex_v");
    uniforms[3] = glGetUniformLocation(shader_program, "u_scale");
    
    glUniform1i(uniforms[0], 0);
    glUniform1i(uniforms[1], 1);
    glUniform1i(uniforms[2], 2);
    glUniform2f(uniforms[3], 1.0f, 1.0f);
    
    // Gen Textures
    glGenTextures(3, textures);
    for (int i=0; i<3; ++i) {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    
    // VBO (Wait, GLES2 doesn't technically have VAO in core, but usually available as extension OES_vertex_array_object? 
    // Or just use VBO and EnableVertexAttribArray every frame. Safer for basic GLES2.)
    // Let's just use VBO.
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
}

void Render_SetScreenSize(int width, int height) {
    if (width > 0 && height > 0) {
        r_width = width;
        r_height = height;
        glViewport(0, 0, width, height);
    }
}

void Render_Clear(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Render_DrawFrame(VideoFrame *frame, int target_width, int target_height) {
    if (!frame || frame->width == 0) return;
    
    if (target_width <= 0) target_width = 1280;
    if (target_height <= 0) target_height = 720;
    
    glViewport(0, 0, target_width, target_height);
    
    // Clear background to black to handle letterboxing/sizing changes
    Render_Clear(0.0f, 0.0f, 0.0f, 1.0f);
    
    // Calculate Aspect Ratio (Letterboxing)
    float video_aspect = (float)frame->width / (float)frame->height;
    float window_aspect = (float)target_width / (float)target_height;
    
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    
    if (video_aspect > window_aspect) {
        // Video is wider: Fit to width
        scale_x = 1.0f;
        scale_y = window_aspect / video_aspect;
    } else {
        // Video is taller: Fit to height
        scale_x = video_aspect / window_aspect;
        scale_y = 1.0f;
    }
    
    glUseProgram(shader_program);
    glUniform2f(uniforms[3], scale_x, scale_y);
    
    // Upload Textures
    // Y
    // Y
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    
    if (frame->linesize[0] == frame->width) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame->width, frame->height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->data[0]);
    } else {
        // Stride mismatch: Upload row-by-row
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame->width, frame->height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
        for (int i = 0; i < frame->height; i++) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i, frame->width, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->data[0] + i * frame->linesize[0]);
        }
    }

    // U
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    
    int w2 = frame->width / 2;
    int h2 = frame->height / 2;
    
    if (frame->linesize[1] == w2) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w2, h2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->data[1]);
    } else {
         glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w2, h2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
         for (int i = 0; i < h2; i++) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i, w2, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->data[1] + i * frame->linesize[1]);
         }
    }

    // V
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, textures[2]);
    
    if (frame->linesize[2] == w2) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w2, h2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->data[2]);
    } else {
         glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w2, h2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
         for (int i = 0; i < h2; i++) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i, w2, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->data[2] + i * frame->linesize[2]);
         }
    }
    
    // Draw Quad
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2*sizeof(float)));
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// --- UI Rendering ---
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static GLuint ui_program;
static GLuint font_tex;
static stbtt_bakedchar cdata[96]; // ASCII 32..126 is 95 glyphs
static float font_height = 32.0f;


static const char *ui_vs = 
    "attribute vec2 pos;\n"
    "attribute vec2 uv;\n"
    "uniform vec2 u_res;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = pos / u_res;\n"
    "    p.y = 1.0 - p.y;\n"
    "    p = p * 2.0 - 1.0;\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "    v_uv = uv;\n"
    "}\n";

static const char *ui_fs = 
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform vec4 u_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_use_tex;\n"
    "uniform vec2 u_rect_size;\n"
    "uniform float u_radius;\n"
    "void main() {\n"
    "    if (u_use_tex > 0.5) {\n"
    "        float a = texture2D(u_tex, v_uv).a;\n"
    "        if (a < 0.1) discard;\n"
    "        gl_FragColor = vec4(u_color.rgb, u_color.a * a);\n"
    "    } else {\n"
    "        // Rounded Rect SDF\n"
    "        vec2 size = u_rect_size;\n"
    "        float radius = u_radius;\n"
    "        vec2 d = abs(v_uv * size - size * 0.5) - (size * 0.5 - radius);\n"
    "        float dist = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;\n"
    "        float alpha = 1.0 - smoothstep(-1.0, 0.0, dist);\n"
    "        gl_FragColor = vec4(u_color.rgb, u_color.a * alpha);\n"
    "    }\n"
    "}\n";

static void InitUI() {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, ui_vs);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, ui_fs);
    ui_program = glCreateProgram();
    glAttachShader(ui_program, vs);
    glAttachShader(ui_program, fs);
    glBindAttribLocation(ui_program, 0, "pos");
    glBindAttribLocation(ui_program, 1, "uv");
    glLinkProgram(ui_program);
    
    // UI VBO (Dynamic)
    glGenBuffers(1, &ui_vbo);
    
    // Font Loading
    FILE* f = fopen("src/ui/default.ttf", "rb");
    if (!f) {
        fprintf(stderr, "Failed to open font file src/ui/default.ttf\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    unsigned char* ttf_buffer = (unsigned char*)ArenaPush(g_render_arena, size);
    fread(ttf_buffer, 1, size, f);
    fclose(f);

    int tex_w = 512;
    int tex_h = 512;
    unsigned char* temp_bitmap = (unsigned char*)ArenaPushZero(g_render_arena, tex_w * tex_h);
    
    stbtt_BakeFontBitmap(ttf_buffer, 0, font_height, temp_bitmap, tex_w, tex_h, 32, 96, cdata);
    
    glGenTextures(1, &font_tex);
    glBindTexture(GL_TEXTURE_2D, font_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, tex_w, tex_h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, temp_bitmap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

void Render_DrawRoundedRect(float x, float y, float w, float h, float rad, float r, float g, float b, float a) {
    if (ui_program == 0) InitUI();
    glUseProgram(ui_program);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glUniform2f(glGetUniformLocation(ui_program, "u_res"), (float)r_width, (float)r_height); 
    glUniform4f(glGetUniformLocation(ui_program, "u_color"), r, g, b, a);
    glUniform1f(glGetUniformLocation(ui_program, "u_use_tex"), 0.0);
    glUniform2f(glGetUniformLocation(ui_program, "u_rect_size"), w, h);
    glUniform1f(glGetUniformLocation(ui_program, "u_radius"), rad);
    
    float verts[] = {
        x, y,       0,0,
        x+w, y,     1,0,
        x, y+h,     0,1,
        x+w, y+h,   1,1
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
    
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), 0);
    glEnableVertexAttribArray(1); // Enable even if unused
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    glDisable(GL_BLEND);
}

void Render_DrawRect(float x, float y, float w, float h, float r, float g, float b, float a) {
    Render_DrawRoundedRect(x, y, w, h, 0.0f, r, g, b, a);
}

void Render_DrawText(const char *text, float x, float y, float scale, float r, float g, float b, float a) {
    if (!text || ui_program == 0) return;
    
    glUseProgram(ui_program);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glUniform2f(glGetUniformLocation(ui_program, "u_res"), (float)r_width, (float)r_height);
    glUniform4f(glGetUniformLocation(ui_program, "u_color"), r, g, b, a);
    glUniform1f(glGetUniformLocation(ui_program, "u_use_tex"), 1.0);
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, font_tex);
    glUniform1i(glGetUniformLocation(ui_program, "u_tex"), 0);
    
    float font_scale = scale * (8.0f / font_height);
    float cursor_x = x;
    // Baseline offset: find a good default or calculate from ascent.
    // Liberation Sans at 32px has roughly 26px ascent.
    float cursor_y = y + (font_height * font_scale * 0.85f); 

    glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));

    while (*text) {
        unsigned char c = (unsigned char)*text;
        if (c == '\n') {
            cursor_x = x;
            cursor_y += font_height * font_scale;
        } else if (c >= 32 && c < 128) {
             stbtt_aligned_quad q;
             float dummy_x = 0;
             float dummy_y = 0;
             stbtt_GetBakedQuad(cdata, 512, 512, c-32, &dummy_x, &dummy_y, &q, 1);
             
             float x0 = cursor_x + q.x0 * font_scale;
             float x1 = cursor_x + q.x1 * font_scale;
             float y0 = cursor_y + q.y0 * font_scale;
             float y1 = cursor_y + q.y1 * font_scale;

             float vdata[] = {
                 x0, y0,    q.s0, q.t0,
                 x1, y0,    q.s1, q.t0,
                 x0, y1,    q.s0, q.t1,
                 x1, y1,    q.s1, q.t1
             };
             
             cursor_x += dummy_x * font_scale;

             glBufferData(GL_ARRAY_BUFFER, sizeof(vdata), vdata, GL_STREAM_DRAW);
             glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        text++;
    }
    glDisable(GL_BLEND);
}

float Render_GetTextWidth(const char *text, float scale) {
    if (!text) return 0.0f;
    float font_scale = scale * (8.0f / font_height);
    float width = 0.0f;
    const char *p = text;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c >= 32 && c < 128) {
            width += cdata[c-32].xadvance;
        }
        p++;
    }
    return width * font_scale;
}
