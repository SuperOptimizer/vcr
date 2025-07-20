#include "vcr.h"
#include <stdio.h>

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
    char zarr_path[512];
    
    // Chunk loading parameters
    s32 chunk_offset[3]; // z, y, x
    s32 chunk_size[3];   // z, y, x
    
    // Loaded chunk data
    chunk* loaded_chunk;
    s32 current_z_slice;
    
    // Texture for displaying slice
    sg_image slice_image;
    snk_image_t snk_img;
    bool slice_image_created;
} app_state_t;

static app_state_t app_state;

// Create or update the slice texture
static void update_slice_texture(void) {
    if (!app_state.loaded_chunk) return;
    
    // Get pointer to current z slice data
    u8* slice_data = &(*app_state.loaded_chunk)[app_state.current_z_slice][0][0];
    
    // Create RGBA data from grayscale
    u8* rgba_data = malloc(CHUNK_LEN * CHUNK_LEN * 4);
    
    
    for (int i = 0; i < CHUNK_LEN * CHUNK_LEN; i++) {
        u8 gray = slice_data[i];
        rgba_data[i * 4 + 0] = gray;  // R
        rgba_data[i * 4 + 1] = gray;  // G
        rgba_data[i * 4 + 2] = gray;  // B
        rgba_data[i * 4 + 3] = 255;   // A (fully opaque)
    }
    
    // Destroy existing image if any
    if (app_state.slice_image_created) {
        snk_destroy_image(app_state.snk_img);
        sg_destroy_image(app_state.slice_image);
    }
    
    // Create new image
    app_state.slice_image = sg_make_image(&(sg_image_desc){
        .width = CHUNK_LEN,
        .height = CHUNK_LEN,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.subimage[0][0] = {
            .ptr = rgba_data,
            .size = CHUNK_LEN * CHUNK_LEN * 4
        }
    });
    
    // Create sokol-nuklear image wrapper
    app_state.snk_img = snk_make_image(&(snk_image_desc_t){
        .image = app_state.slice_image,
        // sampler is optional, will use default
    });
    
    app_state.slice_image_created = true;
    
    free(rgba_data);
}

// Load chunk from zarr
static void load_chunk(void) {
    if (strlen(app_state.zarr_path) == 0 || app_state.zarr_info.zarr_format == 0) {
        sprintf(app_state.info_text, "Please load a Zarr array first");
        return;
    }
    
    // Validate chunk alignment
    for (int i = 0; i < 3; i++) {
        if (app_state.chunk_offset[i] % CHUNK_LEN != 0) {
            sprintf(app_state.info_text, "Chunk offset must be aligned to %d", CHUNK_LEN);
            return;
        }
        if (app_state.chunk_size[i] != CHUNK_LEN) {
            sprintf(app_state.info_text, "Chunk size must be %d", CHUNK_LEN);
            return;
        }
    }
    
    // Free previous chunk if any
    if (app_state.loaded_chunk) {
        chunk_free(app_state.loaded_chunk);
        app_state.loaded_chunk = NULL;
    }
    
    // Construct chunk path using dimension separator
    char chunk_path[1024];
    char sep = app_state.zarr_info.dimension_separator;
    
    // Default to '.' if separator is not set
    if (sep == '\0') {
        sep = '.';
    }
    
    snprintf(chunk_path, sizeof(chunk_path), "%s/%d%c%d%c%d", 
             app_state.zarr_path,
             app_state.chunk_offset[0] / CHUNK_LEN,
             sep,
             app_state.chunk_offset[1] / CHUNK_LEN,
             sep,
             app_state.chunk_offset[2] / CHUNK_LEN);
    
    // Load the chunk
    app_state.loaded_chunk = zarr_read_chunk(chunk_path, app_state.zarr_info);
    
    if (app_state.loaded_chunk) {
        sprintf(app_state.info_text, "Successfully loaded chunk from: %s", chunk_path);
        app_state.current_z_slice = 0;
        update_slice_texture();
    } else {
        sprintf(app_state.info_text, "Failed to load chunk from: %s", chunk_path);
    }
}

// Function to load and parse .zarray file from a zarr volume path
static void load_zarr_array(const char* zarr_path) {
    char zarray_path[1024];
    snprintf(zarray_path, sizeof(zarray_path), "%s/.zarray", zarr_path);

    char* json_content = read_file(zarray_path);
    if (json_content) {
        app_state.zarr_info = zarr_parse_zarray(json_content);
        snprintf(app_state.info_text, sizeof(app_state.info_text),
                 "Successfully loaded .zarray from: %s", zarray_path);
        free(json_content);
    } else {
        snprintf(app_state.info_text, sizeof(app_state.info_text),
                 "Failed to load .zarray from: %s", zarray_path);
        memset(&app_state.zarr_info, 0, sizeof(zarrinfo));
    }
}

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
    strcpy(app_state.zarr_path, "");
    
    // Initialize chunk parameters
    for (int i = 0; i < 3; i++) {
        app_state.chunk_offset[i] = 0;
        app_state.chunk_size[i] = CHUNK_LEN;
    }
}

static void frame(void) {
    // Start new Nuklear frame
    struct nk_context *ctx = snk_new_frame();

    // Main window
    if (nk_begin(ctx, "Volume Cartographer", nk_rect(10, 10, 500, 750), 
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | 
                 NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE)) {
        
        // Menu bar
        nk_menubar_begin(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 25, 2);
        nk_layout_row_push(ctx, 45);
        if (nk_menu_begin_label(ctx, "File", NK_TEXT_LEFT, nk_vec2(120, 200))) {
            nk_layout_row_dynamic(ctx, 25, 1);
            if (nk_menu_item_label(ctx, "Clear", NK_TEXT_LEFT)) {
                memset(&app_state.zarr_info, 0, sizeof(zarrinfo));
                strcpy(app_state.zarr_path, "");
                sprintf(app_state.info_text, "Cleared");
                
                // Clean up chunk data
                if (app_state.loaded_chunk) {
                    chunk_free(app_state.loaded_chunk);
                    app_state.loaded_chunk = NULL;
                }
                
                // Reset chunk parameters
                for (int i = 0; i < 3; i++) {
                    app_state.chunk_offset[i] = 0;
                    app_state.chunk_size[i] = CHUNK_LEN;
                }
                app_state.current_z_slice = 0;
            }
            if (nk_menu_item_label(ctx, "Exit", NK_TEXT_LEFT)) {
                sapp_request_quit();
            }
            nk_menu_end(ctx);
        }
        nk_menubar_end(ctx);

        // Zarr path input section
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "Zarr Volume Path:", NK_TEXT_LEFT);
        
        // Show hint text if path is empty
        if (strlen(app_state.zarr_path) == 0) {
            nk_layout_row_dynamic(ctx, 15, 1);
            nk_label(ctx, "(Enter path to directory containing .zarray file)", NK_TEXT_LEFT);
        }
        
        nk_layout_row_dynamic(ctx, 35, 1);
        // Use NK_EDIT_SIMPLE for better text editing behavior
        nk_flags evt = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER | NK_EDIT_CLIPBOARD, 
                                                       app_state.zarr_path, sizeof(app_state.zarr_path), 
                                                       nk_filter_default);
        
        // Load button or Enter key pressed
        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "Load Zarr Array") || (evt & NK_EDIT_COMMITED)) {
            if (strlen(app_state.zarr_path) > 0) {
                load_zarr_array(app_state.zarr_path);
            } else {
                sprintf(app_state.info_text, "Please enter a Zarr volume path");
            }
        }

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
            
            if (app_state.zarr_info.compressor.id[0]) {
                sprintf(buffer, "Compressor: %s (level %d)", 
                        app_state.zarr_info.compressor.id, 
                        app_state.zarr_info.compressor.clevel);
                nk_label(ctx, buffer, NK_TEXT_LEFT);
            }
            
            // Chunk loading section
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "", NK_TEXT_LEFT); // Spacing
            nk_label(ctx, "Load Chunk:", NK_TEXT_LEFT);
            
            // Offset inputs
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Offset (Z, Y, X):", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 30, 3);
            nk_property_int(ctx, "Z", 0, &app_state.chunk_offset[0], 10000, CHUNK_LEN, 1);
            nk_property_int(ctx, "Y", 0, &app_state.chunk_offset[1], 10000, CHUNK_LEN, 1);
            nk_property_int(ctx, "X", 0, &app_state.chunk_offset[2], 10000, CHUNK_LEN, 1);
            
            // Size inputs (fixed to CHUNK_LEN but shown for clarity)
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Size (must be 128):", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 30, 3);
            nk_property_int(ctx, "Z", CHUNK_LEN, &app_state.chunk_size[0], CHUNK_LEN, CHUNK_LEN, 0);
            nk_property_int(ctx, "Y", CHUNK_LEN, &app_state.chunk_size[1], CHUNK_LEN, CHUNK_LEN, 0);
            nk_property_int(ctx, "X", CHUNK_LEN, &app_state.chunk_size[2], CHUNK_LEN, CHUNK_LEN, 0);
            
            // Load chunk button
            nk_layout_row_dynamic(ctx, 30, 1);
            if (nk_button_label(ctx, "Load Chunk")) {
                load_chunk();
            }
        }
    }
    nk_end(ctx);
    
    // Chunk viewer window
    if (app_state.loaded_chunk) {
        if (nk_begin(ctx, "Chunk Viewer", nk_rect(520, 10, 400, 450),
                     NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | 
                     NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE)) {
            
            // Display current slice info
            char buffer[256];
            sprintf(buffer, "Z Slice: %d / %d (use ← → arrows to navigate)", 
                    app_state.current_z_slice, CHUNK_LEN - 1);
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, buffer, NK_TEXT_CENTERED);
            
            // Display the image with aspect ratio maintained
            if (app_state.slice_image_created) {
                // Get the available content area
                struct nk_rect bounds = nk_window_get_content_region(ctx);
                float available_width = bounds.w;
                float available_height = bounds.h - 30; // Account for the label above
                
                // Calculate the size to maintain square aspect ratio
                float size = (available_width < available_height) ? available_width : available_height;
                
                // Center the image if there's extra space
                if (available_width > size) {
                    float padding = (available_width - size) / 2.0f;
                    nk_layout_row_begin(ctx, NK_STATIC, size, 3);
                    nk_layout_row_push(ctx, padding);
                    nk_spacing(ctx, 1);
                    nk_layout_row_push(ctx, size);
                    nk_image(ctx, nk_image_handle(snk_nkhandle(app_state.snk_img)));
                    nk_layout_row_end(ctx);
                } else {
                    nk_layout_row_dynamic(ctx, size, 1);
                    nk_image(ctx, nk_image_handle(snk_nkhandle(app_state.snk_img)));
                }
            }
        }
        nk_end(ctx);
    }

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
    // Clean up loaded chunk
    if (app_state.loaded_chunk) {
        chunk_free(app_state.loaded_chunk);
    }
    
    // Clean up texture
    if (app_state.slice_image_created) {
        snk_destroy_image(app_state.snk_img);
        sg_destroy_image(app_state.slice_image);
    }
    
    snk_shutdown();
    sg_shutdown();
}

static void input(const sapp_event* event) {
    // Handle z-slice navigation
    if (event->type == SAPP_EVENTTYPE_KEY_DOWN && app_state.loaded_chunk) {
        if (event->key_code == SAPP_KEYCODE_RIGHT) {
            // Go to next z slice (wrap around)
            app_state.current_z_slice = (app_state.current_z_slice + 1) % CHUNK_LEN;
            update_slice_texture();
        } else if (event->key_code == SAPP_KEYCODE_LEFT) {
            // Go to previous z slice (wrap around)
            app_state.current_z_slice = (app_state.current_z_slice - 1 + CHUNK_LEN) % CHUNK_LEN;
            update_slice_texture();
        }
    }
    
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


