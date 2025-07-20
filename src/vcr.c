#include "vcr.h"

// Sokol headers
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_glue.h"

// Nuklear configuration and headers
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_STANDARD_VARARGS
#include "nuklear.h"
#define SOKOL_METAL
#include "sokol_nuklear.h"


// Application state
typedef struct {
    zarrinfo zarr_info;
    char info_text[1024];
} app_state_t;

static app_state_t app_state;

static void init(void) {
    // Setup sokol-gfx
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

    // Setup sokol-nuklear integration
    snk_setup(&(snk_desc_t){
        .enable_set_mouse_cursor = true,
        .dpi_scale = sapp_dpi_scale(),
        .logger.func = slog_func,
    });

    // Initialize app state
    memset(&app_state, 0, sizeof(app_state));
    sprintf(app_state.info_text, "Volume Cartographer Ready");
}

static void frame(void) {
    // Start new Nuklear frame
    struct nk_context *ctx = snk_new_frame();

    // Main window
    if (nk_begin(ctx, "Volume Cartographer", nk_rect(10, 10, 400, 500), 
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | 
                 NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE)) {
        
        // Menu bar
        nk_menubar_begin(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 25, 2);
        nk_layout_row_push(ctx, 45);
        if (nk_menu_begin_label(ctx, "File", NK_TEXT_LEFT, nk_vec2(120, 200))) {
            nk_layout_row_dynamic(ctx, 25, 1);
            if (nk_menu_item_label(ctx, "Open Zarr", NK_TEXT_LEFT)) {
                sprintf(app_state.info_text, "Open functionality not implemented yet");
            }
            if (nk_menu_item_label(ctx, "Exit", NK_TEXT_LEFT)) {
                sapp_request_quit();
            }
            nk_menu_end(ctx);
        }
        nk_menubar_end(ctx);

        // Info section
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "Information:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 60, 1);
        nk_label_wrap(ctx, app_state.info_text);

        // Zarr Info Display
        if (app_state.zarr_info.zarr_format > 0) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Zarr Array Info:", NK_TEXT_LEFT);
            
            char buffer[256];
            sprintf(buffer, "Format: %d", app_state.zarr_info.zarr_format);
            nk_label(ctx, buffer, NK_TEXT_LEFT);
            
            sprintf(buffer, "Shape: [%d, %d, %d]", 
                    app_state.zarr_info.shape[0], 
                    app_state.zarr_info.shape[1], 
                    app_state.zarr_info.shape[2]);
            nk_label(ctx, buffer, NK_TEXT_LEFT);
            
            sprintf(buffer, "Chunks: [%d, %d, %d]", 
                    app_state.zarr_info.chunks[0], 
                    app_state.zarr_info.chunks[1], 
                    app_state.zarr_info.chunks[2]);
            nk_label(ctx, buffer, NK_TEXT_LEFT);
            
            sprintf(buffer, "Data Type: %s", app_state.zarr_info.dtype);
            nk_label(ctx, buffer, NK_TEXT_LEFT);
        }

        // Test button to parse sample JSON
        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "Parse Sample Zarr JSON")) {
            const char* sample_json = 
                "{"
                "\"chunks\": [128, 128, 128],"
                "\"compressor\": {"
                "    \"blocksize\": 0,"
                "    \"clevel\": 5,"
                "    \"cname\": \"lz4\","
                "    \"id\": \"blosc\","
                "    \"shuffle\": 1"
                "},"
                "\"dtype\": \"<f4\","
                "\"fill_value\": 0,"
                "\"filters\": null,"
                "\"order\": \"C\","
                "\"shape\": [512, 512, 512],"
                "\"zarr_format\": 2"
                "}";
            
            app_state.zarr_info = parse_zarr_json(sample_json);
            sprintf(app_state.info_text, "Parsed sample Zarr JSON successfully!");
        }
    }
    nk_end(ctx);

    // Render
    sg_begin_pass(&(sg_pass){
        .action = {
            .colors[0] = {
                .load_action = SG_LOADACTION_CLEAR,
                .clear_value = { 0.2f, 0.3f, 0.3f, 1.0f }
            }
        },
        .swapchain = sglue_swapchain()
    });
    snk_render(sapp_width(), sapp_height());
    sg_end_pass();
    sg_commit();
}

static void cleanup(void) {
    snk_shutdown();
    sg_shutdown();
}

static void input(const sapp_event* event) {
    snk_handle_event(event);
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    return (sapp_desc) {
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = input,
        .enable_clipboard = true,
        .width = 800,
        .height = 600,
        .window_title = "Volume Cartographer Reloaded",
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}



zarrinfo parse_zarr_json(const char* json_string) {
    zarrinfo info = {0}; // Initialize all fields to 0

    struct json_value_s* root = json_parse(json_string, strlen(json_string));
    if (!root) {
        printf("Failed to parse JSON\n");
        return info;
    }

    struct json_object_s* root_object = json_value_as_object(root);
    if (!root_object) {
        printf("Root is not an object\n");
        free(root);
        return info;
    }

    struct json_object_element_s* element = root_object->start;

    while (element) {
        const char* key = element->name->string;
        struct json_value_s* value = element->value;

        if (strcmp(key, "chunks") == 0) {
            struct json_array_s* chunks_array = json_value_as_array(value);
            if (chunks_array) {
                struct json_array_element_s* chunk_elem = chunks_array->start;
                int i = 0;
                while (chunk_elem && i < 3) {
                    struct json_number_s* num = json_value_as_number(chunk_elem->value);
                    if (num) {
                        info.chunks[i] = atoi(num->number);
                    }
                    chunk_elem = chunk_elem->next;
                    i++;
                }
            }
        }
        else if (strcmp(key, "compressor") == 0) {
            struct json_object_s* compressor = json_value_as_object(value);
            if (compressor) {
                struct json_object_element_s* comp_elem = compressor->start;
                while (comp_elem) {
                    const char* comp_key = comp_elem->name->string;

                    if (strcmp(comp_key, "blocksize") == 0) {
                        struct json_number_s* num = json_value_as_number(comp_elem->value);
                        if (num) info.compressor.blocksize = atoi(num->number);
                    }
                    else if (strcmp(comp_key, "clevel") == 0) {
                        struct json_number_s* num = json_value_as_number(comp_elem->value);
                        if (num) info.compressor.clevel = atoi(num->number);
                    }
                    else if (strcmp(comp_key, "cname") == 0) {
                        struct json_string_s* str = json_value_as_string(comp_elem->value);
                        if (str) {
                            strncpy(info.compressor.cname, str->string, 31);
                            info.compressor.cname[31] = '\0';
                        }
                    }
                    else if (strcmp(comp_key, "id") == 0) {
                        struct json_string_s* str = json_value_as_string(comp_elem->value);
                        if (str) {
                            strncpy(info.compressor.id, str->string, 31);
                            info.compressor.id[31] = '\0';
                        }
                    }
                    else if (strcmp(comp_key, "shuffle") == 0) {
                        struct json_number_s* num = json_value_as_number(comp_elem->value);
                        if (num) info.compressor.shuffle = atoi(num->number);
                    }

                    comp_elem = comp_elem->next;
                }
            }
        }
        else if (strcmp(key, "dimension_separator") == 0) {
            struct json_string_s* str = json_value_as_string(value);
            if (str && str->string[0]) {
                info.dimension_separator = str->string[0];
            }
        }
        else if (strcmp(key, "dtype") == 0) {
            struct json_string_s* str = json_value_as_string(value);
            if (str) {
                strncpy(info.dtype, str->string, 31);
                info.dtype[31] = '\0';
            }
        }
        else if (strcmp(key, "fill_value") == 0) {
            struct json_number_s* num = json_value_as_number(value);
            if (num) {
                info.fill_value = atoi(num->number);
            }
        }
        else if (strcmp(key, "filters") == 0) {
            if (json_value_is_null(value)) {
                info.filters = NULL;
            }
        }
        else if (strcmp(key, "order") == 0) {
            struct json_string_s* str = json_value_as_string(value);
            if (str && str->string[0]) {
                info.order = str->string[0];
            }
        }
        else if (strcmp(key, "shape") == 0) {
            struct json_array_s* shape_array = json_value_as_array(value);
            if (shape_array) {
                struct json_array_element_s* shape_elem = shape_array->start;
                int i = 0;
                while (shape_elem && i < 3) {
                    struct json_number_s* num = json_value_as_number(shape_elem->value);
                    if (num) {
                        info.shape[i] = atoi(num->number);
                    }
                    shape_elem = shape_elem->next;
                    i++;
                }
            }
        }
        else if (strcmp(key, "zarr_format") == 0) {
            struct json_number_s* num = json_value_as_number(value);
            if (num) {
                info.zarr_format = atoi(num->number);
            }
        }
        element = element->next;
    }

    free(root);

    return info;
}