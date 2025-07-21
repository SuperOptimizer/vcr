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
#include "sokol_gl.h"


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
    s32 current_slice[3]; // z, y, x indices for the current position
    
    // Textures for displaying slices
    sg_image slice_images[3]; // XY, XZ, YZ views
    snk_image_t snk_imgs[3];
    bool slice_images_created[3];
    
    // Which view is currently active for keyboard navigation
    int active_view; // 0=XY, 1=XZ, 2=YZ
    
    // 3D rendering state
    sgl_context sgl_ctx_3d;
    sgl_pipeline sgl_pip_3d;
    
    // Mesh data from marching cubes
    mesh current_mesh;
    float rotation_x, rotation_y;
    u8 iso_threshold;  // Threshold for isosurface
    
    // Render target for 3D view
    sg_image render_target_3d;
    sg_image depth_target_3d;
    sg_attachments attachments_3d;
    snk_image_t snk_render_target_3d;
    bool render_3d_created;
} app_state_t;

static app_state_t app_state;


// Create or update a specific slice texture
static void update_slice_texture(int view_idx) {
    if (!app_state.loaded_chunk) return;
    
    // Create RGBA data from grayscale
    u8* rgba_data = malloc(CHUNK_LEN * CHUNK_LEN * 4);
    
    // Extract the appropriate slice based on view type
    for (int i = 0; i < CHUNK_LEN; i++) {
        for (int j = 0; j < CHUNK_LEN; j++) {
            u8 gray = 0;
            switch (view_idx) {
                case 0: // XY view (constant Z)
                    gray = (*app_state.loaded_chunk)[app_state.current_slice[0]][i][j];
                    break;
                case 1: // XZ view (constant Y)
                    gray = (*app_state.loaded_chunk)[i][app_state.current_slice[1]][j];
                    break;
                case 2: // YZ view (constant X)
                    gray = (*app_state.loaded_chunk)[i][j][app_state.current_slice[2]];
                    break;
            }
            
            int idx = i * CHUNK_LEN + j;
            
            // Add crosshairs to show current position
            bool is_crosshair = false;
            switch (view_idx) {
                case 0: // XY view - show Y and X position
                    is_crosshair = (i == app_state.current_slice[1] || j == app_state.current_slice[2]);
                    break;
                case 1: // XZ view - show Z and X position
                    is_crosshair = (i == app_state.current_slice[0] || j == app_state.current_slice[2]);
                    break;
                case 2: // YZ view - show Z and Y position
                    is_crosshair = (i == app_state.current_slice[0] || j == app_state.current_slice[1]);
                    break;
            }
            
            if (is_crosshair) {
                // Red crosshairs
                rgba_data[idx * 4 + 0] = 255;  // R
                rgba_data[idx * 4 + 1] = gray / 2;  // G
                rgba_data[idx * 4 + 2] = gray / 2;  // B
            } else {
                rgba_data[idx * 4 + 0] = gray;  // R
                rgba_data[idx * 4 + 1] = gray;  // G
                rgba_data[idx * 4 + 2] = gray;  // B
            }
            rgba_data[idx * 4 + 3] = 255;   // A (fully opaque)
        }
    }
    
    // Destroy existing image if any
    if (app_state.slice_images_created[view_idx]) {
        snk_destroy_image(app_state.snk_imgs[view_idx]);
        sg_destroy_image(app_state.slice_images[view_idx]);
    }
    
    // Create new image
    app_state.slice_images[view_idx] = sg_make_image(&(sg_image_desc){
        .width = CHUNK_LEN,
        .height = CHUNK_LEN,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.subimage[0][0] = {
            .ptr = rgba_data,
            .size = CHUNK_LEN * CHUNK_LEN * 4
        }
    });
    
    // Create sokol-nuklear image wrapper
    app_state.snk_imgs[view_idx] = snk_make_image(&(snk_image_desc_t){
        .image = app_state.slice_images[view_idx],
        // sampler is optional, will use default
    });
    
    app_state.slice_images_created[view_idx] = true;
    
    free(rgba_data);
}

// Update all slice textures
static void update_all_slice_textures(void) {
    for (int i = 0; i < 3; i++) {
        update_slice_texture(i);
    }
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
        // Initialize to center of chunk
        app_state.current_slice[0] = CHUNK_LEN / 2; // Z
        app_state.current_slice[1] = CHUNK_LEN / 2; // Y
        app_state.current_slice[2] = CHUNK_LEN / 2; // X
        app_state.active_view = 0; // Start with XY view
        update_all_slice_textures();
        
        // Generate 3D mesh
        mesh_free(&app_state.current_mesh);
        app_state.current_mesh = generate_mesh_from_chunk(app_state.loaded_chunk, app_state.iso_threshold);
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
    // Initialize app state first
    memset(&app_state, 0, sizeof(app_state));
    sprintf(app_state.info_text, "Volume Cartographer Ready");
    strcpy(app_state.zarr_path, "");
    
    // Initialize chunk parameters
    for (int i = 0; i < 3; i++) {
        app_state.chunk_offset[i] = 0;
        app_state.chunk_size[i] = CHUNK_LEN;
        app_state.current_slice[i] = 0;
        app_state.slice_images_created[i] = false;
    }
    app_state.active_view = 0;
    app_state.iso_threshold = 128;  // Default threshold
    app_state.rotation_x = 0.0f;
    app_state.rotation_y = 0.0f;
    
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
    
    // Setup sokol-gl with larger vertex buffer for marching cubes
    sgl_setup(&(sgl_desc_t){
        .max_vertices = 1024 * 1024,  // 1M vertices (enough for ~333K triangles)
        .logger.func = slog_func,
    });
    
    // Create render target for 3D view
    const int rt_size = 512;
    app_state.render_target_3d = sg_make_image(&(sg_image_desc){
        .usage.render_attachment = true,
        .width = rt_size,
        .height = rt_size,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
    });
    app_state.depth_target_3d = sg_make_image(&(sg_image_desc){
        .usage.render_attachment = true,
        .width = rt_size,
        .height = rt_size,
        .pixel_format = SG_PIXELFORMAT_DEPTH,
    });
    
    // Create attachments for render target
    app_state.attachments_3d = sg_make_attachments(&(sg_attachments_desc){
        .colors[0].image = app_state.render_target_3d,
        .depth_stencil.image = app_state.depth_target_3d,
    });
    
    // Create sokol-gl context for 3D rendering with large vertex buffer
    app_state.sgl_ctx_3d = sgl_make_context(&(sgl_context_desc_t){
        .max_vertices = 1024 * 1024,  // Match the main context size
        .max_commands = 16 * 1024,
        .color_format = SG_PIXELFORMAT_RGBA8,
        .depth_format = SG_PIXELFORMAT_DEPTH,
        .sample_count = 1,
    });
    
    // Create pipeline for 3D rendering
    app_state.sgl_pip_3d = sgl_context_make_pipeline(app_state.sgl_ctx_3d, &(sg_pipeline_desc){
        .cull_mode = SG_CULLMODE_NONE,  // Disable culling to see both sides
        .depth = {
            .write_enabled = true,
            .compare = SG_COMPAREFUNC_LESS_EQUAL,
        },
    });
    
    // Create sokol-nuklear image wrapper for render target
    app_state.snk_render_target_3d = snk_make_image(&(snk_image_desc_t){
        .image = app_state.render_target_3d,
        .sampler = sg_make_sampler(&(sg_sampler_desc){
            .min_filter = SG_FILTER_LINEAR,
            .mag_filter = SG_FILTER_LINEAR,
        })
    });
    app_state.render_3d_created = true;
}

// Render 3D view to texture
static void render_3d_view(void) {
    if (!app_state.loaded_chunk || !app_state.render_3d_created) return;
    
    // Set the sokol-gl context
    sgl_set_context(app_state.sgl_ctx_3d);
    sgl_defaults();
    sgl_load_pipeline(app_state.sgl_pip_3d);
    
    // Setup 3D projection
    sgl_matrix_mode_projection();
    sgl_perspective(sgl_rad(45.0f), 1.0f, 0.1f, 1000.0f);
    
    // Setup camera
    sgl_matrix_mode_modelview();
    sgl_lookat(200.0f, 200.0f, 200.0f,  // eye
               64.0f, 64.0f, 64.0f,      // center (middle of chunk)
               0.0f, 1.0f, 0.0f);        // up
    
    // Apply rotation
    sgl_translate(64.0f, 64.0f, 64.0f);
    sgl_rotate(sgl_rad(app_state.rotation_x), 1.0f, 0.0f, 0.0f);
    sgl_rotate(sgl_rad(app_state.rotation_y), 0.0f, 1.0f, 0.0f);
    sgl_translate(-64.0f, -64.0f, -64.0f);
    
    // Draw the mesh
    if (app_state.current_mesh.vertices && app_state.current_mesh.num_triangles > 0) {
        sgl_begin_triangles();
        
        for (int i = 0; i < app_state.current_mesh.num_triangles; i++) {
            float* v = &app_state.current_mesh.vertices[i * 9];
            float* c = &app_state.current_mesh.colors[i * 9];
            
            // First vertex
            sgl_c3f(c[0], c[1], c[2]);
            sgl_v3f(v[0], v[1], v[2]);
            
            // Second vertex
            sgl_c3f(c[3], c[4], c[5]);
            sgl_v3f(v[3], v[4], v[5]);
            
            // Third vertex
            sgl_c3f(c[6], c[7], c[8]);
            sgl_v3f(v[6], v[7], v[8]);
        }
        
        sgl_end();
    }
    
    // Draw coordinate axes for reference
    sgl_begin_lines();
    // X axis - red
    sgl_c3f(1.0f, 0.0f, 0.0f);
    sgl_v3f(0.0f, 0.0f, 0.0f);
    sgl_v3f(CHUNK_LEN, 0.0f, 0.0f);
    // Y axis - green
    sgl_c3f(0.0f, 1.0f, 0.0f);
    sgl_v3f(0.0f, 0.0f, 0.0f);
    sgl_v3f(0.0f, CHUNK_LEN, 0.0f);
    // Z axis - blue
    sgl_c3f(0.0f, 0.0f, 1.0f);
    sgl_v3f(0.0f, 0.0f, 0.0f);
    sgl_v3f(0.0f, 0.0f, CHUNK_LEN);
    sgl_end();
    
    // Draw current position indicator
    sgl_begin_lines();
    sgl_c3f(1.0f, 1.0f, 0.0f);  // Yellow
    float x = app_state.current_slice[2];
    float y = app_state.current_slice[1];
    float z = app_state.current_slice[0];
    // Small cross at current position
    sgl_v3f(x-5, y, z); sgl_v3f(x+5, y, z);
    sgl_v3f(x, y-5, z); sgl_v3f(x, y+5, z);
    sgl_v3f(x, y, z-5); sgl_v3f(x, y, z+5);
    sgl_end();
}

// Helper function to draw a slice viewer window
void draw_slice_viewer(struct nk_context *ctx, const char* title, int view_idx, float x, float y) {
    const char* axis_names[3] = {"Z", "Y", "X"};
    const char* view_names[3] = {"XY", "XZ", "YZ"};

    if (nk_begin(ctx, title, nk_rect(x, y, 300, 350),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                 NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE)) {

        // Highlight active window
        if (app_state.active_view == view_idx) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "[ACTIVE - Use arrows to navigate]", NK_TEXT_CENTERED);
        }

        // Display current slice info
        char buffer[256];
        sprintf(buffer, "%s View - %s: %d / %d",
                view_names[view_idx],
                axis_names[view_idx],
                app_state.current_slice[view_idx],
                CHUNK_LEN - 1);
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, buffer, NK_TEXT_CENTERED);

        // Make window clickable to set as active
        struct nk_rect bounds = nk_window_get_bounds(ctx);
        if (nk_input_is_mouse_click_in_rect(&ctx->input, NK_BUTTON_LEFT, bounds)) {
            app_state.active_view = view_idx;
        }

        // Display the image with aspect ratio maintained
        if (app_state.slice_images_created[view_idx]) {
            // Get the available content area
            struct nk_rect content_bounds = nk_window_get_content_region(ctx);
            float available_width = content_bounds.w;
            float available_height = content_bounds.h - 60; // Account for the labels above

            // Calculate the size to maintain square aspect ratio
            float size = (available_width < available_height) ? available_width : available_height;

            // Center the image if there's extra space
            if (available_width > size) {
                float padding = (available_width - size) / 2.0f;
                nk_layout_row_begin(ctx, NK_STATIC, size, 3);
                nk_layout_row_push(ctx, padding);
                nk_spacing(ctx, 1);
                nk_layout_row_push(ctx, size);
                nk_image(ctx, nk_image_handle(snk_nkhandle(app_state.snk_imgs[view_idx])));
                nk_layout_row_end(ctx);
            } else {
                nk_layout_row_dynamic(ctx, size, 1);
                nk_image(ctx, nk_image_handle(snk_nkhandle(app_state.snk_imgs[view_idx])));
            }
        }
    }
    nk_end(ctx);
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
                    app_state.current_slice[i] = 0;
                }
                app_state.active_view = 0;
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

    
    // Draw all three slice viewers
    if (app_state.loaded_chunk) {
        draw_slice_viewer(ctx, "XY Slice Viewer", 0, 520, 10);
        draw_slice_viewer(ctx, "XZ Slice Viewer", 1, 830, 10);
        draw_slice_viewer(ctx, "YZ Slice Viewer", 2, 520, 370);
        
        // Draw 3D viewer
        if (nk_begin(ctx, "3D View", nk_rect(830, 370, 300, 400),
                     NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | 
                     NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE)) {
            
            // Threshold control
            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_int(ctx, "Threshold", 0, (int*)&app_state.iso_threshold, 255, 1, 1);
            
            // Regenerate mesh button
            if (nk_button_label(ctx, "Regenerate Mesh")) {
                mesh_free(&app_state.current_mesh);
                app_state.current_mesh = generate_mesh_from_chunk(app_state.loaded_chunk, app_state.iso_threshold);
            }
            
            // Rotation controls
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Drag to rotate", NK_TEXT_CENTERED);
            
            // Display 3D render target
            if (app_state.render_3d_created) {
                // Get the available content area
                struct nk_rect content_bounds = nk_window_get_content_region(ctx);
                float available_width = content_bounds.w;
                float available_height = content_bounds.h - 80; // Account for controls
                
                // Calculate the size to maintain square aspect ratio
                float size = (available_width < available_height) ? available_width : available_height;
                
                // Get mouse input for rotation
                struct nk_rect image_rect = nk_rect(
                    content_bounds.x + (available_width - size) / 2,
                    content_bounds.y + 80,
                    size, size
                );
                
                if (nk_input_is_mouse_hovering_rect(&ctx->input, image_rect)) {
                    if (ctx->input.mouse.buttons[NK_BUTTON_LEFT].down) {
                        app_state.rotation_y += ctx->input.mouse.delta.x * 0.5f;
                        app_state.rotation_x += ctx->input.mouse.delta.y * 0.5f;
                    }
                }
                
                // Center the image if there's extra space
                if (available_width > size) {
                    float padding = (available_width - size) / 2.0f;
                    nk_layout_row_begin(ctx, NK_STATIC, size, 3);
                    nk_layout_row_push(ctx, padding);
                    nk_spacing(ctx, 1);
                    nk_layout_row_push(ctx, size);
                    nk_image(ctx, nk_image_handle(snk_nkhandle(app_state.snk_render_target_3d)));
                    nk_layout_row_end(ctx);
                } else {
                    nk_layout_row_dynamic(ctx, size, 1);
                    nk_image(ctx, nk_image_handle(snk_nkhandle(app_state.snk_render_target_3d)));
                }
            }
        }
        nk_end(ctx);
    }

    // Render 3D view to texture first
    if (app_state.loaded_chunk && app_state.render_3d_created) {
        render_3d_view();
        
        // Render to texture
        sg_begin_pass(&(sg_pass){
            .action = {
                .colors[0] = {
                    .load_action = SG_LOADACTION_CLEAR,
                    .clear_value = { 0.1f, 0.1f, 0.1f, 1.0f }
                }
            },
            .attachments = app_state.attachments_3d
        });
        sgl_context_draw(app_state.sgl_ctx_3d);
        sg_end_pass();
        
        // Check for any sokol-gl errors
        sgl_error_t err = sgl_context_error(app_state.sgl_ctx_3d);
        if (err.any) {
            printf("sokol-gl error: vertices_full=%d, commands_full=%d\n", 
                   err.vertices_full, err.commands_full);
        }
    }
    
    // Main render pass
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
    
    // Clean up mesh
    mesh_free(&app_state.current_mesh);
    
    // Clean up textures
    for (int i = 0; i < 3; i++) {
        if (app_state.slice_images_created[i]) {
            snk_destroy_image(app_state.snk_imgs[i]);
            sg_destroy_image(app_state.slice_images[i]);
        }
    }
    
    // Clean up 3D rendering resources
    if (app_state.render_3d_created) {
        snk_destroy_image(app_state.snk_render_target_3d);
        sg_destroy_attachments(app_state.attachments_3d);
        sg_destroy_image(app_state.render_target_3d);
        sg_destroy_image(app_state.depth_target_3d);
        sgl_destroy_context(app_state.sgl_ctx_3d);
    }
    
    sgl_shutdown();
    snk_shutdown();
    sg_shutdown();
}

static void input(const sapp_event* event) {
    // Handle slice navigation based on active view
    if (event->type == SAPP_EVENTTYPE_KEY_DOWN && app_state.loaded_chunk) {
        bool update_needed = false;
        
        if (event->key_code == SAPP_KEYCODE_RIGHT) {
            // Go to next slice (wrap around)
            app_state.current_slice[app_state.active_view] = 
                (app_state.current_slice[app_state.active_view] + 1) % CHUNK_LEN;
            update_needed = true;
        } else if (event->key_code == SAPP_KEYCODE_LEFT) {
            // Go to previous slice (wrap around)
            app_state.current_slice[app_state.active_view] = 
                (app_state.current_slice[app_state.active_view] - 1 + CHUNK_LEN) % CHUNK_LEN;
            update_needed = true;
        } else if (event->key_code == SAPP_KEYCODE_TAB) {
            // Tab to cycle through views
            app_state.active_view = (app_state.active_view + 1) % 3;
        }
        
        if (update_needed) {
            // Update all views since they're connected
            update_all_slice_textures();
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
        .width = 1200,
        .height = 800,
        .window_title = "Volume Cartographer Reloaded",
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}


