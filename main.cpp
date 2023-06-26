#define SOKOL_IMPL
#define SOKOL_D3D11

#include <fstream>
#include <string>
#include <sstream>
#include <limits>

#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"

#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

#include "sokol/util/sokol_imgui.h"

#define STB_DS_IMPLEMENTATION
#include "stb/stb_ds.h"

using namespace ImGui;

void program_init();
void program_frame();
void program_handle_event(const sapp_event*);
void program_cleanup();

void constrain_window_to_area(ImVec2 min, ImVec2 max);

void setup_canvas();
void cleanup_canvas();
void draw_canvas();

void resize_canvas_if_dirty();
void recreate_canvas_shader_if_dirty();

std::string read_entire_file(const char *file);

void clamp(float *v, float min, float max) {
    if (*v < min) *v = min;
    if (*v > max) *v = max;
}

void clamp(int *v, int min, int max) {
    if (*v < min) *v = min;
    if (*v > max) *v = max;
}

void inline_replace(char *str, char target, char replacement) {
    for (char *s = str; s && *s != '\0'; s++) {
        if (*s == target) *s = replacement;
    }
}

enum class Layer_Shape : unsigned int {
    None,
    Circle,
    Rectangle,
};

enum class Layer_Kind : unsigned int {
    None,
    Union,
    Intersect,
    Subtract,
};

#define MAX_NUM_LAYER_NAME_BYTES 32

struct Layer {
    Layer_Shape shape = Layer_Shape::None;
    Layer_Kind  kind = Layer_Kind::None;

    char name[MAX_NUM_LAYER_NAME_BYTES] = { 0 };

    float position[2] = { 0.0f, 0.0f };
    float rotation360 = 0.0f;

    struct {
        float radius = 0.3f;
    } circle;

    struct {
        ImVec2 size = { 0.3f, 0.3f };
        ImVec4 corner_radii = { 0.15f, 0.15f, 0.15f, 0.15f };
    } rectangle;

    bool created_this_frame = true;
};

std::string build_sdf_function(Layer* layers);

struct Vertex_Desc {
    struct {
        float x, y, z;
    } pos;

    struct {
        float x, y;
    } sdf;
};

enum class Preview_Mode : unsigned int {
    None,
    Mask,
    Contour
};

struct Preview_Settings {
    Preview_Mode mode = Preview_Mode::Mask;

    float blur_amount = 0.005f;

    float division_distance = 0.25f;
    float division_thickness = 0.01f;
    int num_subdivisions = 3;
    float subdivision_thickness = 0.003f;

    unsigned char padding[8];

    float interior_color[4] = { 0.86f, 0.74f, 0.18f, 1.0f };
    float exterior_color[4] = { 0.23f, 0.68f, 0.62f, 1.0f };
};

struct Canvas {
    bool needs_reshader = true;
    bool needs_resize   = true;

    sg_buffer   vertex_buffer = 0;
    sg_shader   shader        = 0;
    sg_pipeline pipeline      = 0;
    sg_bindings bindings      = { };

    Layer *layers = NULL;

    Preview_Settings preview;

    Canvas() {}
};

struct Program_State {
    Canvas canvas;
};

Program_State state { };

sapp_desc sokol_main(int argc, char **argv) {
    return sapp_desc {
        .init_cb     = program_init,
        .frame_cb    = program_frame,
        .cleanup_cb  = program_cleanup,
        .event_cb    = program_handle_event,
        .enable_clipboard = true,
        .logger      = { slog_func },
    };
}

sg_pass_action clear_action;

void program_init() {
    sg_setup(sg_desc { .logger = { slog_func }, .context = sapp_sgcontext()});

    setup_canvas();

    simgui_setup(simgui_desc_t { });

    clear_action = sg_pass_action {
        .colors = { sg_color_attachment_action { .load_action = SG_LOADACTION_CLEAR, .clear_value = {0.3, 0.4, 0.4, 1.0}}}
    };
}

float elapsed_time = 0.0f;

void program_frame() {
    elapsed_time += sapp_frame_duration();

    simgui_new_frame(simgui_frame_desc_t {
        .width      = sapp_width(),
        .height     = sapp_height(),
        .delta_time = sapp_frame_duration(),
        .dpi_scale  = sapp_dpi_scale()});

    //  Debug Info
    // {
    //     LabelText("dt", "%f", sapp_frame_duration());
    //     LabelText("et", "%f", elapsed_time);
    // }

    //  Draw Palette
    {
        Begin("Palette", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        SetWindowPos(ImVec2 {});

        if (Button("Circle")) {
            state.canvas.needs_reshader = true;

            Layer* layer = arraddnptr(state.canvas.layers, 1);
            *layer = {};

            strcpy(layer->name, "layer");
            layer->kind  = Layer_Kind::Union;
            layer->shape = Layer_Shape::Circle;
        }

        if (Button("Rectangle")) {
            state.canvas.needs_reshader = true;

            Layer* layer = arraddnptr(state.canvas.layers, 1);
            *layer = {};

            strcpy(layer->name, "layer");
            layer->kind  = Layer_Kind::Union;
            layer->shape = Layer_Shape::Rectangle;
        }

        End();
    }

    // Draw Layers
    {
        SetNextWindowSizeConstraints(ImVec2 { 150.0f, 50.0f }, ImVec2 { sapp_widthf(), sapp_heightf() * 0.65f });
        Begin("Layers", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);

        SetWindowPos(ImVec2 { 0.0f, sapp_heightf() - GetWindowSize().y });

        Layer* layers = state.canvas.layers;
        for (int i = 0; i < arrlen(layers); i++) {
            Layer &layer = layers[i];
            PushID(i);
            if (CollapsingHeader((std::stringstream {} << i << ": " << layer.name).str().c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (layer.created_this_frame) {
                    SetKeyboardFocusHere(0);
                }

                InputText("name", layer.name, MAX_NUM_LAYER_NAME_BYTES, ImGuiInputTextFlags_CharsNoBlank);
                inline_replace(layer.name, ' ', '_');

                {
                    int kind_index = (int) layer.kind - 1;
                    if (Combo("Kind", &kind_index, "Add\0Intersect\0Subtract\0\0")) {
                        layer.kind = (Layer_Kind) (kind_index + 1);
                        state.canvas.needs_reshader = true;
                    }
                }

                {
                    int shape_index = (int) layer.shape - 1;
                    if (Combo("Shape", &shape_index, "circle\0rectangle\0\0")) {
                        layer.shape = (Layer_Shape) (shape_index + 1);
                        state.canvas.needs_reshader = true;
                    }
                }

                if (InputFloat2("Position", layer.position)) {
                    state.canvas.needs_reshader = true;
                }

                if (layer.shape != Layer_Shape::Circle) {
                    if (InputFloat("Rotation (Degrees)", &layer.rotation360)) {
                        state.canvas.needs_reshader = true;
                    }
                }

                switch (layer.shape) {
                    case Layer_Shape::Circle: {
                        if (InputFloat("Radius", &layer.circle.radius, 0.01f)) {
                            state.canvas.needs_reshader = true;
                        }
                        break;
                    }

                    case Layer_Shape::Rectangle: {
                        if (InputFloat2("Size", (float *) &layer.rectangle.size)) {
                            state.canvas.needs_reshader = true;
                        }

                        if (InputFloat4("Corner Radii", (float *) &layer.rectangle.corner_radii)) {
                            state.canvas.needs_reshader = true;

                            clamp(&layer.rectangle.corner_radii.w, 0.0f, 1.0f);
                            clamp(&layer.rectangle.corner_radii.x, 0.0f, 1.0f);
                            clamp(&layer.rectangle.corner_radii.y, 0.0f, 1.0f);
                            clamp(&layer.rectangle.corner_radii.z, 0.0f, 1.0f);
                        }
                        break;
                    }
                }
            }

            PopID();

            layer.created_this_frame = false;
        }

        if (!(IsWindowAppearing() || IsWindowCollapsed())) { 
            float inverse_dpi_scale = 1.0f / sapp_dpi_scale();

            ImVec2 dpi_unscaled_window_size = { sapp_widthf(), sapp_heightf() };
            dpi_unscaled_window_size.x *= inverse_dpi_scale;
            dpi_unscaled_window_size.y *= inverse_dpi_scale;
            constrain_window_to_area(ImVec2 { 0.0f, 150.0f}, dpi_unscaled_window_size);
        }

        End();
    }

    //  Draw HLSL Code
    {
        SetNextWindowSizeConstraints(ImVec2 { }, ImVec2 { sapp_widthf() * 0.45f, sapp_heightf() * 0.65f });
        Begin("HLSL", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
        SetWindowCollapsed(true, ImGuiCond_FirstUseEver);

        SetWindowPos(ImVec2 { sapp_widthf() - GetWindowSize().x, sapp_heightf() - GetWindowSize().y });

        std::string hlsl = build_sdf_function(state.canvas.layers);

        if (Button("Copy to Clipboard")) {
            sapp_set_clipboard_string(hlsl.c_str());
        }

        Text("%s", hlsl.c_str());


        if (!(IsWindowAppearing() || IsWindowCollapsed())) { 
            float inverse_dpi_scale = 1.0f / sapp_dpi_scale();

            ImVec2 dpi_unscaled_window_size = { sapp_widthf(), sapp_heightf() };
            dpi_unscaled_window_size.x *= inverse_dpi_scale;
            dpi_unscaled_window_size.y *= inverse_dpi_scale;
            constrain_window_to_area(ImVec2 { }, dpi_unscaled_window_size);
        }

        End();
    }

    //  Preview Settings
    {
        SetNextWindowSizeConstraints(ImVec2 { sapp_widthf() * 0.35f, 0.0f }, ImVec2 { sapp_widthf(), sapp_heightf() });
        Begin("Preview", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
        SetWindowCollapsed(false, ImGuiCond_FirstUseEver);
        SetWindowPos(ImVec2 { sapp_widthf() - GetWindowSize().x, 0.0f });

        Preview_Settings &preview = state.canvas.preview;

        Combo("Mode", (int *) &preview.mode, "None\0Mask\0Contour\0\0");

        switch (preview.mode) {
            case Preview_Mode::Mask: {
                InputFloat("Blur Amount", &preview.blur_amount, 0.01f, 0.1f);
                clamp(&preview.blur_amount, 0.0f, 1.0f);
                break;
            }

            case Preview_Mode::Contour: {
                InputFloat("Division Distance", &preview.division_distance, 0.001f, 0.01f);
                clamp(&preview.division_distance, 0.0f, 1.0f);
                InputFloat("Division Thickness", &preview.division_thickness, 0.001f, 0.01f);
                clamp(&preview.division_thickness, 0.0f, 1.0f);
                InputInt("Num Subdivisions", &preview.num_subdivisions); 
                clamp(&preview.num_subdivisions, 0, preview.num_subdivisions);
                InputFloat("Subdivision Thickness", &preview.subdivision_thickness, 0.001f, 0.01f);
                clamp(&preview.subdivision_thickness, 0.0f, 1.0f);

                Separator();
                ColorEdit4("Interior Color", preview.interior_color);
                ColorEdit4("Exterior Color", preview.exterior_color);
                break;
            }
        }

        if (!(IsWindowAppearing() || IsWindowCollapsed())) { 
            float inverse_dpi_scale = 1.0f / sapp_dpi_scale();

            ImVec2 dpi_unscaled_window_size = { sapp_widthf(), sapp_heightf() };
            dpi_unscaled_window_size.x *= inverse_dpi_scale;
            dpi_unscaled_window_size.y *= inverse_dpi_scale;
            constrain_window_to_area(ImVec2 { }, dpi_unscaled_window_size);
        }

        End();
    }

    sg_begin_default_pass(&clear_action, sapp_width(), sapp_height());
    draw_canvas();
    simgui_render();
    sg_end_pass();
    sg_commit();
}

void program_handle_event(const sapp_event *event) {
    bool handled = simgui_handle_event(event);
    if (handled) return;

    switch (event->type) {
        case SAPP_EVENTTYPE_RESIZED:
            state.canvas.needs_resize = true;
            break;
    }
}

void program_cleanup() {
    simgui_shutdown();

    cleanup_canvas();

    sg_shutdown();
}

void constrain_window_to_area(ImVec2 min, ImVec2 max) {
    ImVec2 window_pos  = GetWindowPos();
    ImVec2 window_size = GetWindowSize();

    if (window_pos.x > max.x - window_size.x) window_pos.x = (int) max.x - window_size.x;
    if (window_pos.y > max.y - window_size.y) window_pos.y = (int) max.y - window_size.y;
    if (window_pos.x < min.x) window_pos.x = (int) min.x;
    if (window_pos.y < min.y) window_pos.y = (int) min.y;
    SetWindowPos(window_pos);

    if (window_size.x > max.x) window_size.x = max.x;
    if (window_size.y > max.y) window_size.y = max.y;
    SetWindowSize(window_size);
}

void setup_canvas() {
    Canvas canvas;

    canvas.vertex_buffer = sg_make_buffer(sg_buffer_desc { .size = 6 * sizeof(Vertex_Desc), .usage = SG_USAGE_DYNAMIC});
    state.canvas = canvas;
}

void cleanup_canvas() {
    Canvas &canvas = state.canvas;

    sg_destroy_pipeline(canvas.pipeline);
    sg_destroy_shader(canvas.shader);
    sg_destroy_buffer(canvas.vertex_buffer);

    arrfree(canvas.layers);

    state.canvas = Canvas {};
}

void draw_canvas() {
    resize_canvas_if_dirty();
    recreate_canvas_shader_if_dirty();

    Canvas &canvas = state.canvas;

    sg_apply_pipeline(canvas.pipeline);
    sg_apply_bindings(sg_bindings { .vertex_buffers = { canvas.vertex_buffer } });
    sg_apply_uniforms(SG_SHADERSTAGE_FS, 0, sg_range { .ptr = &canvas.preview, .size = sizeof(Preview_Settings) });
    sg_draw(0, 6, 1);
}

void resize_canvas_if_dirty() {
    Canvas &canvas = state.canvas;

    if (!canvas.needs_resize) return;
    canvas.needs_resize = false;

    const float x_aspect_ratio = sapp_heightf() / sapp_widthf();
    const float y_aspect_ratio = sapp_widthf() / sapp_heightf();

    const float x_scale = x_aspect_ratio > 1.0f ? 1.0f : x_aspect_ratio;
    const float y_scale = y_aspect_ratio > 1.0f ? 1.0f : y_aspect_ratio;

    Vertex_Desc verts[] = {
        { { -x_scale, -y_scale, 1.0f }, { 0, 0 } },
        { { -x_scale,  y_scale, 1.0f }, { 0, 1 } },
        { {  x_scale,  y_scale, 1.0f }, { 1, 1 } },

        { {  x_scale,  y_scale, 1.0f }, { 1, 1 } },
        { {  x_scale, -y_scale, 1.0f }, { 1, 0 } },
        { { -x_scale, -y_scale, 1.0f }, { 0, 0 } },
    };

    sg_update_buffer(canvas.vertex_buffer, SG_RANGE(verts));
}

void recreate_canvas_shader_if_dirty() {
    Canvas &canvas = state.canvas;
    if (!canvas.needs_reshader) return;
    canvas.needs_reshader = false;

    if (canvas.pipeline.id) {
        sg_destroy_pipeline(canvas.pipeline);
    }

    if (canvas.shader.id) {
        sg_destroy_shader(canvas.shader);
    }

    std::string sdf_function_text = build_sdf_function(canvas.layers);

    std::string preview_text = read_entire_file("preview.hlsl");
    std::string full_frag_shader_text = sdf_function_text + preview_text;

    canvas.shader = sg_make_shader(sg_shader_desc {
        .attrs = {
            { .sem_name = "POSITION" },
            { .sem_name = "TEXCOORD" },
        },

        .vs =
            "struct VS_INPUT {\n"
            "    float4 position : POSITION;"
            "    float2 uv       : TEXCOORD;"
            "};\n"
            "\n"
            "struct VS_OUTPUT {\n"
            "    float2 uv       : TEXCOORD;"
            "    float4 position : SV_Position;"
            "};\n"
            "\n"
            "VS_OUTPUT main(VS_INPUT input) {\n"
            "    VS_OUTPUT output;"
            "    output.position = input.position;\n"
            "    output.uv       = input.uv;\n"
            "    return output;\n"
            "}\n",

        .fs = {
            .source = full_frag_shader_text.c_str(),
            .uniform_blocks = {
                { 
                    .size = sizeof(Preview_Settings),
                    .uniforms = {
                        { .name = "mode",                   .type = SG_UNIFORMTYPE_INT,   },
                        { .name = "blur_amount",            .type = SG_UNIFORMTYPE_FLOAT  },
                        { .name = "division_distance",      .type = SG_UNIFORMTYPE_FLOAT  },
                        { .name = "division_thickness",     .type = SG_UNIFORMTYPE_FLOAT  },
                        { .name = "num_subdivisions",       .type = SG_UNIFORMTYPE_INT    },
                        { .name = "subdivision_thickness",  .type = SG_UNIFORMTYPE_FLOAT  },
                        { .name = "inside_color",           .type = SG_UNIFORMTYPE_FLOAT4 },
                        { .name = "outside_color",          .type = SG_UNIFORMTYPE_FLOAT4 },
                    }
                }
            },
        } 
    });

    canvas.pipeline = sg_make_pipeline(sg_pipeline_desc {
        .shader = canvas.shader,
        .layout = {
            .attrs = {
                { .format = SG_VERTEXFORMAT_FLOAT3 },
                { .format = SG_VERTEXFORMAT_FLOAT2 }
            }
        },
    });
}

std::string read_entire_file(const char *file) {
    std::ifstream file_stream(file);
    if (file_stream.good()) {
        return std::string((std::istreambuf_iterator<char>(file_stream)),
                           (std::istreambuf_iterator<char>()           ));
    }

    return std::string();
}

std::string build_sdf_function(Layer* layers) {
    std::stringstream text;

    text << read_entire_file("palette.hlsl");

    text << "\n\n";
    text << "float build_sdf(float2 uv) {\n";
    text << "    float2 coord = (uv - 0.5) * 2.0;\n";
    text << "    float sdf = " << std::numeric_limits<float>::max() << ";\n";

    for (int i = 0; i < arrlen(layers); i++) {
        Layer layer = layers[i];
        std::string layer_name = (std::stringstream() << layer.name << "_" << i).str(); 

        switch (layer.shape) {
            case Layer_Shape::Circle: {
                text << "    float " + layer_name + " = sdf_circle(coord, float2(" << layer.position[0] << ", " << layer.position[1] << "), " << layer.circle.radius << ");\n";
                break;
            }

            case Layer_Shape::Rectangle: {
                ImVec2 size    = layer.rectangle.size;
                ImVec4 corners = layer.rectangle.corner_radii;
                text << "    float " + layer_name + " = sdf_rect(coord, float2(" << layer.position[0] << ", " << layer.position[1] << "), " << layer.rotation360 / 360.0f << ", float2(" << size.x << ", " << size.y << "), float4(" << corners.y << ", " << corners.w << ", " << corners.x << ", " << corners.z << "));\n";
                break;
            }

            default: {
                text << "    float " + layer_name + " = " << std::numeric_limits<float>::max() << ";\n";
                break;
            }
        }

        switch (layer.kind) {
            case Layer_Kind::Union: {
                text << "    sdf = sdf_union(sdf, " + layer_name + ", 0.01);\n";
                break;
            }

            case Layer_Kind::Intersect: {
                text << "    sdf = sdf_intersect(sdf, " + layer_name + ", 0.01);\n";
                break;
            }

            case Layer_Kind::Subtract: {
                text << "    sdf = sdf_subtract(sdf, " + layer_name + ", 0.01);\n";
                break;
            }

            default: break;
        }
    }

    text << "    return sdf;\n";
    text << "}\n";

    return text.str();
}